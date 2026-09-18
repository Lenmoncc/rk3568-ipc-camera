#!/usr/bin/env python3
"""正式采集/队列/编码集成测试；模拟输出不可播放，不等同于硬件编码验证。"""
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


def counters(text, module):
    """提取指定模块的计数，避免混淆采集帧数和编码包数。"""
    match = re.search(r'\[' + module + r'\] summary: ([^\n]+)', text)
    assert match, text
    return {key: int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', match.group(1))}


def run_case(name='normal', args=(), expected=0, driver='normal', mpp_cleanup=True):
    """运行一个编码场景，验证退出码、生命周期和正常输出计数关系。"""
    global count
    output = directory / f'{count}-{name}.h264'
    result = subprocess.run([str(binary), '-c', str(config), '--encode', '--output', str(output),
                             '--encode-fps', '25', '--frames', '8', *args],
                            env=dict(os.environ, IPC_MOCK_CASE=driver, IPC_MOCK_MPP_CASE=name),
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == expected, (name, result.returncode, result.stderr)
    assert 'MOCK_CLEANUP_OK' in result.stderr, (name, result.stderr)
    if mpp_cleanup:
        assert 'MPP_MOCK_CLEANUP_OK' in result.stderr, (name, result.stderr)
    if expected == 0:
        video = counters(result.stderr, 'capture')
        encoded = counters(result.stderr, 'encoder')
        assert video['enqueued'] == video['consumed'] == encoded['submitted'] == encoded['encoded']
        assert encoded['eos'] == 1 and encoded['fps_config'] == 25
        assert encoded['bytes'] == output.stat().st_size
        assert encoded['first_pts_us'] >= 0 and encoded['last_pts_us'] > encoded['first_pts_us']
        assert 'capture complete; queue drained and resources released' in result.stderr
    count += 1
    return result, output


def check_payload(output, partition=False):
    """逐包检查 QBUF 后像素副本、PTS 和包顺序，确认写出的不是失效 MPP 内存。"""
    data = output.read_bytes()
    assert data[:14] == bytes([0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xce])
    body = data[14:]
    assert len(body) == 8 * 32 * (2 if partition else 1)
    stamps = []
    for index, offset in enumerate(range(0, len(body), 32)):
        packet = body[offset:offset + 32]
        frame = index // 2 if partition else index
        assert packet[:4] == b'\0\0\0\1' and packet[5] == frame
        stamps.append(struct.unpack('=q', packet[8:16])[0])
    assert all(a <= b if partition else a < b for a, b in zip(stamps, stamps[1:]))


with tempfile.TemporaryDirectory(prefix='ipc-encoder-') as temporary:
    directory = Path(temporary)
    config = directory / 'ipc.conf'
    base = (root / 'configs/ipc.conf').read_text().replace('/dev/video0', '/dev/mock-video')
    config.write_text(base)
    for case in ['normal', 'legacy-fps-keys', 'eos-no-meta', 'eos-no-frame',
                 'put-busy-once', 'get-empty-once', 'get-nok-once', 'partition']:
        result, output = run_case(case)
        check_payload(output, case == 'partition')
    run_case(driver='rkisp-field-any')
    result, output = run_case(args=['--dump', str(directory / 'raw.nv12'), '--dump-frames', '3'])
    assert (directory / 'raw.nv12').stat().st_size == 3 * 1280 * 720 * 3 // 2
    for case in ['create-fail', 'control-fail', 'init-fail', 'cfg-init-fail', 'get-cfg-fail',
                 'cfg-key-fail', 'set-cfg-fail', 'header-mode-fail', 'group-fail', 'buffer-fail',
                 'header-init-fail', 'header-fail', 'empty-header', 'frame-init-fail', 'put-fail',
                 'put-timeout', 'get-fail', 'get-timeout', 'eos-timeout', 'wrong-pts',
                 'empty-packet', 'missing-returned-frame', 'eos-with-data', 'unexpected-eos',
                 'packet-write-fail', 'destroy-fail']:
        result, _ = run_case(case, expected=1)
        if case in ['eos-timeout', 'eos-with-data', 'unexpected-eos', 'missing-returned-frame']:
            assert counters(result.stderr, 'encoder')['eos'] == 0
    for driver in ['write-fail', 'flush-fail', 'streamon-fail', 'consumer-thread-fail', 'producer-thread-fail', 'poll-error']:
        run_case(expected=1, driver=driver)
    # 文件覆盖、缺失目录、两个输出路径相同：失败但不可破坏已有内容。
    protected = directory / 'protected.h264'
    protected.write_bytes(b'KEEP THIS FILE')
    run_case(args=['--output', str(protected)], expected=1, mpp_cleanup=False)
    assert protected.read_bytes() == b'KEEP THIS FILE'
    run_case(args=['--output', str(directory / 'missing' / 'out.h264')], expected=1, mpp_cleanup=False)
    run_case(args=['--dump', str(directory / 'same'), '--output', str(directory / 'same')], expected=1, mpp_cleanup=False)
    # 慢消费触发原始帧队列丢新帧，编码不丢中间输出包，计数仍闭合。
    config.write_text(base.replace('queue.video_raw_capacity=4', 'queue.video_raw_capacity=1'))
    result, output = run_case(args=['--frames', '30', '--consumer-delay-ms', '30'])
    assert counters(result.stderr, 'capture')['dropped_full'] > 0
    assert 'cannot preserve capture timestamp gaps' in result.stderr
    config.write_text(base)
    # 无模式、错误组合和数字越界均应在访问设备前拒绝。
    for arguments in [['--encode'], ['--output', 'x.h264'], ['--encode-fps', '25'],
                      ['--encode', '--capture', '--output', 'x.h264'],
                      ['--encode', '--check-config', '--output', 'x.h264'],
                      ['--encode', '--output', 'x.h264', '--encode-fps', '0'],
                      ['--encode', '--output', 'x.h264', '--encode-fps', '31'],
                      ['--encode', '--output', 'x.h264', '--encode-fps', '25x'],
                      ['--encode', '--output', '']]:
        process = subprocess.run([str(binary), '-c', str(config), *arguments], capture_output=True, text=True, timeout=5)
        assert process.returncode == 2 and 'first DQBUF' not in process.stderr, process.stderr
        count += 1
    # SIGINT/SIGTERM 都在首帧已消费之后发送，必须排空队列与 MPP 并返回 130。
    for signum, mpp_case in [(sig, case) for sig in [signal.SIGINT, signal.SIGTERM]
                             for case in ['normal', 'eos-no-meta']]:
        target = directory / f'interrupt-{signum}-{mpp_case}.h264'
        process = subprocess.Popen([str(binary), '-c', str(config), '--encode', '--output', str(target),
                                    '--encode-fps', '25', '--frames', '0'], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, env=dict(os.environ, IPC_MOCK_CASE='normal',
                                                                  IPC_MOCK_MPP_CASE=mpp_case))
        selector = selectors.DefaultSelector()
        selector.register(process.stderr, selectors.EVENT_READ)
        data = b''
        try:
            while b'[consumer] consumed=1 ' not in data:
                assert selector.select(timeout=5), 'encoder did not start'
                chunk = process.stderr.read1(4096)
                assert chunk, data
                data += chunk
            process.send_signal(signum)
            _, tail = process.communicate(timeout=10)
            text = (data + tail).decode()
            assert process.returncode == 130 and 'MPP_MOCK_CLEANUP_OK' in text, text
            video = counters(text, 'capture')
            encoded = counters(text, 'encoder')
            assert encoded['eos'] == 1 and video['enqueued'] == encoded['encoded']
            assert target.stat().st_size == encoded['bytes']
            count += 1
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            selector.close()
print(f'PASS: {count} encoder integration scenarios (mock MPP, not hardware encoding).')
