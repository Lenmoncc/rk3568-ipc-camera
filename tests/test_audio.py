#!/usr/bin/env python3
"""正式 ALSA 采集、队列和保存集成测试；模拟库不代表真实麦克风或板端回放。"""
import os
from pathlib import Path
import re
import selectors
import signal
import struct
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
count = 0


def counters(text):
    """提取样本统计，保留每声道样本数与交错字节数的区别。"""
    match = re.search(r'\[audio\] summary: ([^\n]+)', text)
    assert match, text
    return {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', match.group(1))}


def check_payload(path, silence=False):
    """核对全部左右声道样本的值和连续下标，检测重复、丢失或块内存被复用。"""
    data = path.read_bytes()
    assert len(data) % 4 == 0
    for index, (left, right) in enumerate(struct.iter_unpack('<hh', data)):
        expected = 0 if silence else index % 30000 + 1
        assert (left, right) == (expected, -expected), (index, left, right)


def run_case(name='normal', args=(), expected=0):
    """运行独立子进程并检查退出码、资源释放和成功时的样本/文件长度闭合。"""
    global count
    path = directory / f'{count}-{name}.pcm'
    result = subprocess.run([str(binary), '-c', str(config), '--audio-capture', '--seconds', '1',
                             '--pcm', str(path), *args], env=dict(os.environ, IPC_MOCK_ALSA_CASE=name),
                            capture_output=True, text=True, timeout=12)
    assert result.returncode == expected, (name, result.returncode, result.stderr)
    assert 'ALSA_MOCK_CLEANUP_OK' in result.stderr, result.stderr
    if expected == 0:
        stats = counters(result.stderr)
        assert stats['captured_samples'] == stats['enqueued_samples'] == stats['consumed_samples'] == stats['saved_samples']
        assert stats['saved_samples'] > 0 and stats['bytes'] == path.stat().st_size == stats['saved_samples'] * 4
        assert stats['xruns'] == stats['suspends'] == stats['queue_full'] == 0
        assert 'audio complete; queue drained and resources released' in result.stderr
        check_payload(path, name == 'silence')
    count += 1
    return result, path


with tempfile.TemporaryDirectory(prefix='ipc-audio-') as temporary:
    directory = Path(temporary)
    config = directory / 'ipc.conf'
    base = (root/'configs/ipc.conf').read_text()
    config.write_text(base)
    for case in ['normal', 'short-read', 'eagain-once', 'zero-once', 'eintr-once', 'wait-eintr', 'silence']:
        result, path = run_case(case)
        assert counters(result.stderr)['saved_samples'] == 48000
        if case == 'short-read':
            assert re.search(r'short_reads=[1-9]', result.stderr), result.stderr
        if case == 'silence':
            assert 'all saved PCM samples are zero' in result.stderr
    result, path = run_case(args=['--seconds', '10'])
    assert path.stat().st_size == 1920000
    for case in ['open-fail','hw-alloc-fail','hw-any-fail','access-fail','format-fail','channels-fail',
                 'rate-fail','huge-period','buffer-fail','apply-hw-fail','read-hw-fail','actual-rate',
                 'actual-channels','actual-format','actual-access','actual-buffer','sw-alloc-fail',
                 'sw-current-fail','avail-fail','apply-sw-fail','prepare-fail','start-fail','xrun','suspend',
                 'read-fail','wait-fail','timeout','too-many','drop-fail','close-fail','write-fail',
                 'flush-fail','consumer-thread-fail','producer-thread-fail']:
        result, path = run_case(case, expected=1)
        if case in ['xrun','suspend']:
            assert counters(result.stderr)['xruns' if case == 'xrun' else 'suspends'] == 1
            check_payload(path)
    config.write_text(base.replace('audio.channels=2','audio.channels=1'))
    result, path = run_case(expected=1)
    assert not path.exists() and 'set_channels' in result.stderr
    config.write_text(base.replace('queue.audio_raw_capacity=16','queue.audio_raw_capacity=1'))
    result, path = run_case(args=['--consumer-delay-ms','50'],expected=1)
    assert counters(result.stderr)['queue_full'] == 1 and 'raw queue full' in result.stderr
    check_payload(path)
    config.write_text(base)
    protected = directory/'keep.pcm'
    protected.write_bytes(b'KEEP')
    run_case(args=['--pcm',str(protected)],expected=1)
    assert protected.read_bytes() == b'KEEP'
    run_case(args=['--pcm',str(directory/'missing/out.pcm')],expected=1)
    # 模式和参数错误必须在打开录音设备前被拒绝。
    for args in [['--audio-capture'], ['--pcm','x'], ['--seconds','1'],
                 ['--audio-capture','--pcm',''], ['--audio-capture','--pcm','x','--capture'],
                 ['--audio-capture','--pcm','x','--encode'], ['--audio-capture','--pcm','x','--check-config'],
                 ['--audio-capture','--pcm','x','--frames','1'], ['--audio-capture','--pcm','x','--dump','y'],
                 ['--audio-capture','--pcm','x','--output','y'], ['--audio-capture','--pcm','x','--encode-fps','25'],
                 ['--audio-capture','--pcm','x','--seconds','-1'], ['--audio-capture','--pcm','x','--seconds','86401'],
                 ['--audio-capture','--pcm','x','--seconds','1x'], ['--audio-capture','--pcm','x','--consumer-delay-ms','1001']]:
        result = subprocess.run([str(binary),'-c',str(config),*args],capture_output=True,text=True,timeout=5)
        assert result.returncode == 2 and 'ALSA_MOCK_CLEANUP_OK' not in result.stderr, result.stderr
        count += 1
    # 数据正常流动以及持续无数据时都可被信号停止，不依赖 readi 无限阻塞。
    for case, signum in [('normal',signal.SIGINT),('normal',signal.SIGTERM),('timeout',signal.SIGINT)]:
        path = directory/f'signal-{case}-{signum}.pcm'
        proc = subprocess.Popen([str(binary),'-c',str(config),'--audio-capture','--pcm',str(path),'--seconds','0'],
                                env=dict(os.environ,IPC_MOCK_ALSA_CASE=case),stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        selector = selectors.DefaultSelector()
        selector.register(proc.stderr,selectors.EVENT_READ)
        data = b''
        ready = b'timestamp source:' if case == 'timeout' else b'first block:'
        try:
            while ready not in data:
                assert selector.select(timeout=5), 'audio worker did not start'
                chunk = proc.stderr.read1(4096)
                assert chunk, data
                data += chunk
            proc.send_signal(signum)
            _, tail = proc.communicate(timeout=5)
            text = (data+tail).decode()
            assert proc.returncode == 130 and 'ALSA_MOCK_CLEANUP_OK' in text, text
            stats = counters(text)
            assert stats['captured_samples'] == stats['enqueued_samples'] == stats['saved_samples']
            assert path.stat().st_size == stats['saved_samples']*4
            check_payload(path)
            count += 1
        finally:
            if proc.poll() is None:
                proc.kill(); proc.wait()
            selector.close()
print(f'PASS: {count} audio integration scenarios (mock ALSA, not board validation).')
