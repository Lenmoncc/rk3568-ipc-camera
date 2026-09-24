#!/usr/bin/env python3
"""真实 MP4/AAC 集成检查；采集设备和 MPP 为按时间工作的替身，不代表真实板端性能。"""
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
count = 0


def probe(path):
    """用独立 ffprobe 读取容器、流和每个包的实际时间戳。"""
    result = subprocess.run(['ffprobe','-v','error','-show_streams','-show_format','-show_packets',
                             '-of','json',str(path)],capture_output=True,text=True,timeout=10)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def validate(path, log, case):
    """核验双轨可解码、计数闭合、单轨 DTS 严格递增以及起始偏差没有被分别归零。"""
    data = probe(path)
    streams = data['streams']
    assert len(streams) == 2
    video,audio = streams
    assert video['codec_name']=='h264' and video['width']==1280 and video['height']==720
    assert video['is_avc']=='true' and video['nal_length_size']=='4'
    assert audio['codec_name']=='aac' and audio['sample_rate']=='48000' and audio['channels']==2
    match = re.search(r'\[record\] summary: (.*)',log)
    assert match,log
    counters = {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',match[1])}
    assert counters['video_enqueued']==counters['video_encoded']==counters['video_packets']>0
    assert counters['video_eos']==counters['audio_drained']==counters['trailer']==1
    assert counters['audio_samples']>0
    for index,key in ((0,'video_packets'),(1,'audio_packets')):
        packets=[p for p in data['packets'] if p['stream_index']==index]
        assert len(packets)==counters[key],(index,len(packets),counters)
        dts=[int(p['dts']) for p in packets]
        assert all(b>a for a,b in zip(dts,dts[1:])),dts
        assert all(int(p['duration'])>0 for p in packets)
    expected_start = .4 if case=='late-video' else .175
    assert abs(float(video['start_time'])-expected_start)<.002,video['start_time']
    if case=='late-audio':
        # AAC 首包带编码预卷，流起点比真实音频采样早约 1024/48000 秒。
        assert .32<float(audio['start_time'])<.36,audio['start_time']
    decode = subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(path),'-map','0:v:0','-map','0:a:0',
                             '-f','null','-'],capture_output=True,timeout=15)
    assert decode.returncode==0,decode.stderr
    return data


def run(case='normal', expected=0, args=(), extra_env=None):
    """运行正式录像流水线，验证退出码及测试设备完整清理。"""
    global count
    path = directory/f'record-{count}.mp4'
    env = dict(os.environ,IPC_RECORD_FIXTURE=str(fixture),IPC_RECORD_CASE=case)
    env.update(extra_env or {})
    result = subprocess.run([str(binary),'-c',str(config),'--record','--seconds','1','--encode-fps','25',
                             '--mp4',str(path),*args],env=env,capture_output=True,text=True,timeout=12)
    assert result.returncode==expected,(case,result.returncode,result.stderr)
    assert 'RECORD_DEVICES_CLEAN' in result.stderr,result.stderr
    if expected==0:
        assert 'record complete; both streams drained and MP4 finalized' in result.stderr
        validate(path,result.stderr,case)
    else:
        assert 'record failed' in result.stderr,result.stderr
    count += 1
    return result,path


with tempfile.TemporaryDirectory(prefix='ipc-record-') as temporary:
    directory = Path(temporary)
    fixture = directory/'fixture.h264'
    # 有效的一帧 Baseline IDR，供桥接器逐帧复制；测试不把假字节当作 H.264 可解码证据。
    subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=blue:size=1280x720:rate=25',
                    '-frames:v','1','-c:v','libx264','-profile:v','baseline','-pix_fmt','yuv420p',
                    '-x264-params','keyint=1:bframes=0','-f','h264',str(fixture)],check=True,timeout=15)
    base = (root/'configs/ipc.conf').read_text()
    config = directory/'ipc.conf';config.write_text(base)
    for case in ('normal','late-video','late-audio','short-write'):
        run(case)
    # 三秒多次交错检查短期软件时间轴；硬件长期漂移留给板端验收。
    result,path = run(args=['--seconds','3'])
    data=probe(path)
    assert 2.9<float(data['format']['duration'])<3.2,data['format']
    for case in ('video-init-fail','audio-init-fail','encoder-init-fail','video-start-fail','audio-start-fail',
                 'video-read-fail','audio-read-fail','encode-fail','eos-fail','header-write-fail',
                 'packet-write-fail','seek-fail','close-fail'):
        run(case,1)
    for n in range(1,6):
        run(expected=1,extra_env={'IPC_RECORD_THREAD_FAIL':str(n)})
    for case in ('video-timeout','audio-timeout'):
        run(case,1,args=['--seconds','5'])
    config.write_text(base.replace('queue.video_packet_capacity=32','queue.video_packet_capacity=1'))
    run(expected=1,args=['--consumer-delay-ms','100'])
    config.write_text(base.replace('queue.audio_packet_capacity=64','queue.audio_packet_capacity=1'))
    run(expected=1)
    config.write_text(base)
    keep=directory/'keep.mp4';keep.write_bytes(b'KEEP')
    run(expected=1,args=['--mp4',str(keep)])
    assert keep.read_bytes()==b'KEEP'
    run(expected=1,args=['--mp4',str(directory/'missing'/'out.mp4')])
    for args in (['--encode'],['--audio-encode'],['--capture'],['--audio-capture'],['--frames','10'],
                 ['--pcm','a.pcm'],['--aac','a.aac'],['--output','v.h264'],['--dump','v.nv12'],['--seconds','-1']):
        result=subprocess.run([str(binary),'-c',str(config),'--record',*args],capture_output=True,text=True,timeout=5)
        assert result.returncode==2,(args,result.stderr)
        count+=1
    # SIGINT/SIGTERM 时双路已有输入均须排空，文件保留可解码的 trailer。
    for sig in (signal.SIGINT,signal.SIGTERM):
        path=directory/f'stop-{sig}.mp4'
        env=dict(os.environ,IPC_RECORD_FIXTURE=str(fixture),IPC_RECORD_CASE='normal')
        process=subprocess.Popen([str(binary),'-c',str(config),'--record','--seconds','0','--encode-fps','25',
                                  '--mp4',str(path)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        selector=selectors.DefaultSelector();selector.register(process.stderr,selectors.EVENT_READ)
        log=b''
        try:
            while b'[record] starting:' not in log:
                assert selector.select(6),log
                part=os.read(process.stderr.fileno(),4096);assert part,log;log+=part
            # 测试端等待约一秒让两条轨都有数据，再发停止信号。
            import time
            time.sleep(1)
            process.send_signal(sig)
            _,tail=process.communicate(timeout=8);log+=tail
            assert process.returncode==130,log
            validate(path,log.decode(),'normal')
            assert b'RECORD_DEVICES_CLEAN' in log
        finally:
            selector.close()
            if process.poll() is None:process.kill();process.wait()
        count+=1
print(f'PASS: {count} MP4 recording scenarios; real AAC/mux/decode, simulated devices and MPP.')
