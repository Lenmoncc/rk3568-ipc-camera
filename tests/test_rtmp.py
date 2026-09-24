#!/usr/bin/env python3
"""真实 AAC/MP4/FLV + 编码分发/子进程监督集成检查；设备与 MPP 采用现有替身。"""
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
count = 0


def probe(path):
    """用独立 ffprobe 提取双轨、时间戳和 payload 摘要，检验两输出引用的一致性。"""
    result = subprocess.run(['ffprobe','-v','error','-show_streams','-show_packets','-show_data_hash','sha256',
                             '-of','json',str(path)],capture_output=True,text=True,timeout=10)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def validate_media(path):
    """验证 H.264/AAC 双轨和严格递增 DTS，再用独立解码器完整解码。"""
    data = probe(path)
    assert [s['codec_name'] for s in data['streams']]==['h264','aac'],data['streams']
    assert data['streams'][0]['width']==1280 and data['streams'][0]['height']==720
    assert data['streams'][1]['sample_rate']=='48000' and data['streams'][1]['channels']==2
    for i in (0,1):
        packets=[p for p in data['packets'] if p['stream_index']==i]
        assert packets
        dts=[int(p['dts']) for p in packets]
        assert all(b>a for a,b in zip(dts,dts[1:])),dts
    result=subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(path),'-map','0:v','-map','0:a',
                           '-f','null','-'],capture_output=True,timeout=15)
    assert result.returncode==0,result.stderr
    return data


def counters(log, tag):
    """解析对应输出汇总，不把播放器存在或空文件误判为推流成功。"""
    match=re.search(r'\['+tag+r'\] summary: (.*)',log)
    assert match,log
    return {k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',match[1])}


def compare_outputs(local, remote):
    """确认编码 payload 与包数一致，两轨统一平移而非各自归零，容忍 FLV 毫秒量化。"""
    differences=[]
    for i in (0,1):
        a=[p for p in local['packets'] if p['stream_index']==i]
        b=[p for p in remote['packets'] if p['stream_index']==i]
        assert len(a)==len(b),(i,len(a),len(b))
        assert [p['data_hash'] for p in a]==[p['data_hash'] for p in b]
        differences.extend(float(y['dts_time'])-float(x['dts_time']) for x,y in zip(a,b))
    assert max(differences)-min(differences)<.002,differences
    assert min(float(p['dts_time']) for p in remote['packets'])>=0


def run(case='normal', expected=0, stream_only=False, seconds=1, signal_number=None,
        config_text=None, extra_env=None, extra_args=(), validate=True, after_start=None):
    """运行隔离测试并检查超时边界、子进程已回收、MP4 收尾及网络结果。"""
    global count
    count+=1
    path=directory/f'local-{count}.mp4'; flv=directory/f'network-{count}.flv'; pid=directory/f'child-{count}.pid'
    config=directory/f'config-{count}.conf';config.write_text(config_text or base)
    env=dict(os.environ,IPC_RECORD_FIXTURE=str(fixture),IPC_RTMP_CASE=case,
             IPC_RTMP_FLV=str(flv),IPC_RTMP_PID_FILE=str(pid))
    env.update(extra_env or {})
    args=[str(binary),'-c',str(config),'--stream' if stream_only else '--record','--seconds',str(seconds),'--encode-fps','25']
    if not stream_only:args+=['--mp4',str(path)]
    start=time.monotonic()
    p=subprocess.Popen([*args,*extra_args],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
    if after_start:after_start()
    if signal_number:
        # 一秒足够得到音视频；网络阻塞替身不会自行超时，必须由正式监督逻辑结束。
        time.sleep(1)
        if p.poll() is None:os.killpg(p.pid,signal_number)
    out,log=p.communicate(timeout=12)
    elapsed=time.monotonic()-start
    assert p.returncode==expected,(case,p.returncode,log)
    assert elapsed < (5.5 if signal_number else seconds+5.5),(case,elapsed)
    assert 'RECORD_DEVICES_CLEAN' in log,log
    if pid.exists():
        child=int(pid.read_text())
        assert not Path(f'/proc/{child}').exists(),f'network child {child} was not reaped'
    local=None
    if validate and not stream_only:
        stats=counters(log,'record')
        assert stats['trailer']==stats['video_eos']==stats['audio_drained']==1,log
        assert stats['video_enqueued']==stats['video_encoded']==stats['video_packets']>0
        local=validate_media(path)
        assert len([x for x in local['packets'] if x['stream_index']==0])==stats['video_packets']
        assert len([x for x in local['packets'] if x['stream_index']==1])==stats['audio_packets']
        if seconds and not signal_number:
            offset=.45 if env.get('IPC_RECORD_CASE')=='late-audio' else .15
            assert stats['audio_samples']>=(seconds-offset)*48000,log
    if stream_only:assert not path.exists()
    if expected in (0,130):
        stats=counters(log,'rtmp')
        assert stats['error']==0 and stats['header']==stats['completed']==1,log
        assert stats['video_packets']==stats['video_accepted']>0
        assert stats['audio_packets']==stats['audio_accepted']>0
        if flv.exists():
            remote=validate_media(flv)
            if local:compare_outputs(local,remote)
    elif expected==3:
        assert 'MP4 finalized; RTMP failed (exit=3)' in log
        assert counters(log,'rtmp')['error']<0
    print(f'PASS: {case} stream_only={stream_only} exit={expected} ({elapsed:.2f}s)',flush=True)
    return log,local


with tempfile.TemporaryDirectory(prefix='ipc-rtmp-') as temporary:
    directory=Path(temporary);fixture=directory/'fixture.h264'
    subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=blue:size=1280x720:rate=25',
                    '-frames:v','1','-c:v','libx264','-profile:v','baseline','-pix_fmt','yuv420p',
                    '-x264-params','keyint=1:bframes=0','-f','h264',str(fixture)],check=True,timeout=15)
    base=(root/'configs/ipc-rtmp.conf').read_text()
    run()
    run(stream_only=True)
    run(extra_args=['--stream'])
    run(extra_env={'IPC_RECORD_CASE':'late-video'})
    run(extra_env={'IPC_RECORD_CASE':'late-audio'})
    for case in ('open-fail','header-fail','write-fail','close-fail','hang-close'):
        run(case,3)
    for case in ('hang-open','hang-write','slow-write'):
        run(case,3,seconds=3)
    # 大队列防止先触发队列满，单独检验监督连接和写入的实际截止时间。
    large=base.replace('queue.video_packet_capacity=32','queue.video_packet_capacity=256').replace('queue.audio_packet_capacity=64','queue.audio_packet_capacity=512')
    run('hang-open',3,seconds=6,config_text=large)
    run('hang-write',3,seconds=4,config_text=large)
    # 容量是每输出共用配置；须给 MP4 留够启动偏差缓存，避免测试自身使本地队列先满。
    run('slow-write',3,seconds=3,config_text=base.replace('queue.audio_packet_capacity=64','queue.audio_packet_capacity=256'))
    run('slow-write',3,seconds=3,config_text=base.replace('queue.video_packet_capacity=32','queue.video_packet_capacity=256'))
    run('open-fail',1,stream_only=True,validate=False)
    run('hang-open',1,stream_only=True,seconds=5,validate=False)
    run(extra_env={'IPC_RECORD_THREAD_FAIL':'2'},expected=3)
    for n in (1,3,4,5,6):
        run(extra_env={'IPC_RECORD_THREAD_FAIL':str(n)},expected=1,validate=False)
    for sig in (signal.SIGINT,signal.SIGTERM):
        run(seconds=0,signal_number=sig,expected=130)
    run(stream_only=True,seconds=0,signal_number=signal.SIGINT,expected=130)
    run('hang-write',3,seconds=0,signal_number=signal.SIGINT,config_text=large)
    run('hang-open',3,seconds=0,signal_number=signal.SIGTERM,config_text=large)
    # 已有文件保护和模式隔离：均在设备启动之前拒绝不合法组合。
    for args in (['--stream'],['--stream','--mp4','x.mp4'],['--stream','--capture'],
                 ['--stream','--frames','10'],['--stream','--consumer-delay-ms','1']):
        result=subprocess.run([str(binary),'-c',str(root/'configs/ipc.conf'),*args],capture_output=True,text=True,timeout=5)
        assert result.returncode==2,(args,result.stderr)
        count+=1
    # 真实 FFmpeg RTMP/TCP 协议回环，不依赖真实摄像头或远程 SRS。
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
    url=f'rtmp://127.0.0.1:{port}/live/cam01'
    actual=base.replace('rtmp://192.168.137.100:1935/live/cam01',url)
    received=directory/'real-network.flv'
    receiver=subprocess.Popen(['ffmpeg','-v','error','-listen','1','-i',url,'-c','copy','-f','flv',str(received)],
                              stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    try:
        # 只检查监听状态，不用 TCP 探测连接消费单客户端 RTMP 接收器。
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            tcp=Path('/proc/net/tcp').read_text()
            if any(f':{port:04X} ' in line and ' 0A ' in line for line in tcp.splitlines()):break
            if receiver.poll() is not None:raise AssertionError(receiver.communicate()[1])
            time.sleep(.02)
        else:raise AssertionError('RTMP receiver did not listen')
        log,local=run('real',seconds=2,config_text=actual)
        receiver.communicate(timeout=8)
        remote=validate_media(received)
        # 接收端可能统一归零；仍应保留包 payload 与双轨相对时间差。
        for i in (0,1):
            left=[p['data_hash'] for p in local['packets'] if p['stream_index']==i]
            right=[p['data_hash'] for p in remote['packets'] if p['stream_index']==i]
            assert left==right,(i,len(left),len(right))
    finally:
        if receiver.poll() is None:receiver.kill();receiver.communicate()
    # 真实 TCP 连接中途断开：接收器退出后 MP4 仍须录满三秒并正常解码。
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
    url=f'rtmp://127.0.0.1:{port}/live/cam01'
    actual=base.replace('rtmp://192.168.137.100:1935/live/cam01',url)
    receiver=subprocess.Popen(['ffmpeg','-v','error','-listen','1','-i',url,'-c','copy','-f','flv',str(directory/'disconnected.flv')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    try:
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            if any(f':{port:04X} ' in line and ' 0A ' in line for line in Path('/proc/net/tcp').read_text().splitlines()):break
            if receiver.poll() is not None:raise AssertionError(receiver.communicate()[1])
            time.sleep(.02)
        else:raise AssertionError('RTMP receiver did not listen')
        def disconnect():
            """待发送一秒后终止测试接收器，模拟服务器中途停止。"""
            time.sleep(1)
            receiver.kill();receiver.communicate(timeout=5)
        run('real',3,seconds=3,config_text=actual,after_start=disconnect)
    finally:
        if receiver.poll() is None:receiver.kill();receiver.communicate()
print(f'PASS: {count} RTMP scenarios; real AAC/FLV/MP4 and TCP loopback, simulated devices/MPP.')
