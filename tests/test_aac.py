#!/usr/bin/env python3
"""真实 AAC 编码集成测试：模拟 ALSA，独立解析 ADTS 并解码，验证正常与失败路径。"""
import array
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import tempfile

binary, api = [Path(x).resolve() for x in sys.argv[1:3]]
root = Path(__file__).resolve().parents[1]
count = 0


def adts_frames(path, channels=2):
    """独立解析每个 ADTS 头，确认帧边界、LC、48kHz、声道及文件无残尾。"""
    data = path.read_bytes()
    pos = frames = 0
    while pos < len(data):
        h = data[pos:pos+7]
        assert len(h) == 7 and h[0] == 255 and h[1] == 241, (path, pos)
        assert h[2] >> 6 == 1 and (h[2] >> 2) & 15 == 3
        assert ((h[2] & 1) << 2 | h[3] >> 6) == channels
        length = ((h[3] & 3) << 11) | h[4] << 3 | h[5] >> 5
        assert length > 7 and pos + length <= len(data) and h[6] & 3 == 0
        pos += length
        frames += 1
    assert pos == len(data)
    return frames


def decode(path, channels=2):
    """通过独立 FFmpeg 解码器验证码流可解码，并检查实际输出采样数量。"""
    result = subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(path),
                             '-f', 's16le', '-c:a', 'pcm_s16le', '-'], capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    assert len(result.stdout) == adts_frames(path, channels) * 1024 * channels * 2
    return result.stdout


def run_case(name='normal', expected=0, args=()):
    """运行正式音频流水线，检查退出码、资源释放和 AAC/采样计数一致性。"""
    global count
    path = directory / f'capture-{count}.aac'
    result = subprocess.run([str(binary), '-c', str(config), '--audio-encode', '--seconds', '1',
                             '--aac', str(path), *args], env=dict(os.environ, IPC_MOCK_ALSA_CASE=name),
                            capture_output=True, text=True, timeout=12)
    assert result.returncode == expected, (name, result.returncode, result.stderr)
    assert 'ALSA_MOCK_CLEANUP_OK' in result.stderr, result.stderr
    if not expected:
        match = re.search(r'\[aac\] summary: (.*)', result.stderr)
        assert match, result.stderr
        stats = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', match[1])}
        assert stats['input_samples'] == stats['converted_samples']
        assert stats['submitted_samples'] == stats['input_samples'] + stats['padding_samples']
        assert stats['drained'] == 1 and stats['packets'] == adts_frames(path)
        assert stats['bytes'] == path.stat().st_size and stats['padding_samples'] < 1024
        decode(path)
    count += 1
    return result, path


with tempfile.TemporaryDirectory(prefix='ipc-aac-') as temporary:
    directory = Path(temporary)
    config = directory / 'ipc.conf'
    config.write_text((root/'configs/ipc.conf').read_text())
    result = subprocess.run([str(api), str(config), str(directory)], capture_output=True, text=True, timeout=25)
    assert result.returncode == 0, result.stderr
    print(result.stdout.strip())
    for n in (1, 1023, 1024, 1025, 480000):
        path = directory / f'tone-{n}-2.aac'
        decoded = decode(path)
        assert n <= len(decoded)//4 <= n + 2048
    decode(directory/'tone-48000-1.aac', 1)
    # AAC 有损，验证左右声道主频和能量，不要求逐字节相等。
    samples = array.array('h', decode(directory/'tone-480000-2.aac'))
    if sys.byteorder != 'little':
        samples.byteswap()
    for channel, frequency in ((0,440),(1,880)):
        signal_values = samples[48000*2+channel:96000*2:2]
        def energy_at(hz):
            """计算指定频率的复数投影幅度，用于发现声道互换或采样速率错误。"""
            real = sum(v*math.cos(2*math.pi*hz*i/48000) for i,v in enumerate(signal_values))
            imag = sum(v*math.sin(2*math.pi*hz*i/48000) for i,v in enumerate(signal_values))
            return math.hypot(real, imag)
        assert energy_at(frequency) > 20*energy_at(880 if frequency == 440 else 440)
        assert 7000 < math.sqrt(sum(v*v for v in signal_values)/len(signal_values)) < 10000
    count += 7
    for name in ['normal','short-read','eagain-once','zero-once','eintr-once','wait-eintr','silence']:
        run_case(name)
    result, path = run_case(args=['--seconds','10'])
    assert 'input_samples=480000 ' in result.stderr and 'padding_samples=256 ' in result.stderr
    probe = subprocess.run(['ffprobe','-v','error','-show_entries','stream=codec_name,profile,sample_rate,channels',
                            '-of','json',str(path)], capture_output=True,text=True,check=True)
    stream = json.loads(probe.stdout)['streams'][0]
    assert stream == dict(codec_name='aac', profile='LC',sample_rate='48000',channels=2),stream
    for name in ['open-fail','start-fail','xrun','suspend','read-fail','wait-fail','timeout','drop-fail',
                 'close-fail','write-fail','flush-fail','consumer-thread-fail','producer-thread-fail']:
        run_case(name, expected=1)
    run_case(expected=1,args=['--consumer-delay-ms','100'])
    keep = directory/'keep.aac'; keep.write_bytes(b'keep')
    run_case(expected=1,args=['--aac',str(keep)])
    assert keep.read_bytes() == b'keep'
    run_case(expected=1,args=['--aac',str(directory/'missing'/'out.aac')])
    for args in [[],['--pcm','bad.pcm'],['--audio-capture'],['--encode'],['--frames','10'],
                 ['--output','bad.aac'],['--encode-fps','25'],['--seconds','-1']]:
        argv = [str(binary),'-c',str(config),'--audio-encode']
        if args:
            argv += ['--aac',str(directory/'invalid.aac'),*args]
        result = subprocess.run(argv,capture_output=True,text=True,timeout=5)
        assert result.returncode == 2, (args,result.stderr)
        assert not (directory/'invalid.aac').exists()
        count += 1
    for stop_signal in (signal.SIGINT,signal.SIGTERM):
        path = directory/f'signal-{stop_signal}.aac'
        process = subprocess.Popen([str(binary),'-c',str(config),'--audio-encode','--seconds','0','--aac',str(path)],
                                   stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        selector = selectors.DefaultSelector(); selector.register(process.stderr,selectors.EVENT_READ)
        log = b''
        try:
            while b'first block:' not in log:
                assert selector.select(8), log
                part = os.read(process.stderr.fileno(),4096)
                assert part, log
                log += part
            process.send_signal(stop_signal)
            _,tail = process.communicate(timeout=8); log += tail
            assert process.returncode == 130,log
            assert b'drained=1' in log and b'ALSA_MOCK_CLEANUP_OK' in log,log
            decode(path)
        finally:
            selector.close()
            if process.poll() is None:
                process.kill(); process.wait()
        count += 1
print(f'PASS: {count} real AAC/API/integration checks; ALSA input simulated, board playback still required.')
