#!/usr/bin/env python3
"""对正式 main/capture/pipeline 的模拟集成测试；不打开真实设备。"""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
count = 0


def run_case(name, args=(), expected=0, env_extra=None, cleanup=True):
    """运行一个隔离场景，检查退出码及设备资源回收标记。"""
    global count
    env = dict(os.environ, IPC_MOCK_CASE=name)
    env.update(env_extra or {})
    result = subprocess.run([str(binary), '-c', str(config), '--capture', '--frames', '4', *args],
                            capture_output=True, text=True, env=env, timeout=12)
    assert result.returncode == expected, (name, result.returncode, result.stderr)
    if cleanup:
        assert 'MOCK_CLEANUP_OK' in result.stderr, (name, result.stderr)
    if expected == 0:
        assert 'capture complete; queue drained and resources released' in result.stderr
    count += 1
    return result


def stats(result):
    """从汇总日志提取帧计数，核对生产、消费及丢帧关系。"""
    match = re.search(r'summary: (.*)', result.stderr)
    assert match, result.stderr
    return {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', match.group(1))}


with tempfile.TemporaryDirectory(prefix='ipc-capture-') as name:
    directory = Path(name)
    config = directory / 'ipc.conf'
    config.write_text((root / 'configs/ipc.conf').read_text().replace('/dev/video0', '/dev/mock-video'))
    output = directory / 'packed.nv12'
    result = run_case('normal', ['--dump', str(output), '--dump-frames', '3'])
    values = stats(result)
    assert values['captured'] == values['enqueued'] == values['consumed'] == 4
    assert values['saved'] == 3 and values['dropped_full'] == 0
    # 每帧全部像素精确比较，同时证明 QBUF 后覆盖 MMAP 不会破坏应用帧。
    expected = b''.join(bytes([i]) * (1280 * 720) + bytes([128]) * (1280 * 360) for i in range(3))
    assert output.read_bytes() == expected
    # RKISP 的逐帧 ANY 仅在协商 NONE 时兼容；验证图像内容与正常路径完全一致。
    field_output = directory / 'field-any.nv12'
    result = run_case('rkisp-field-any', ['--dump', str(field_output), '--dump-frames', '3'])
    assert field_output.read_bytes() == expected
    assert 'field=0' in result.stderr and 'using negotiated progressive' in result.stderr
    assert stats(result)['captured'] == stats(result)['consumed'] == 4
    for case in ['eagain', 'unknown-clock', 'bad-once', 'short-once', 'bad-offset', 'bad-size',
                 'timestamp-backward', 'generic-field-any', 'interlaced-once', 'plane-count-once']:
        result = run_case(case)
        values = stats(result)
        assert values['captured'] == values['consumed'] == 4
        if case not in ['eagain', 'unknown-clock']:
            assert values['dequeued'] == 5 and 'invalid=1' in result.stderr
        if case == 'unknown-clock':
            assert 'timestamp_fallback=4' in result.stderr
        if case == 'timestamp-backward':
            assert 'timestamp_rejected=1' in result.stderr
    for case in ['capability', 'set-format', 'format', 'two-planes', 'zero-buffers',
                 'query-fail', 'mmap-fail', 'streamon-fail', 'qbuf-fail', 'poll-error',
                 'invalid-index', 'streamoff-fail', 'timeout', 'all-bad',
                 'consumer-thread-fail', 'producer-thread-fail']:
        run_case(case, expected=1)
    run_case('open-fail', expected=1, cleanup=False)
    # 已有文件不允许覆盖，错误路径也必须关闭已配置设备。
    run_case('normal', ['--dump', str(output)], expected=1)
    assert output.read_bytes() == expected
    run_case('normal', ['--dump', str(directory / 'missing' / 'out.nv12')], expected=1)
    for case in ['write-fail', 'flush-fail']:
        run_case(case, ['--dump', str(directory / (case + '.nv12'))], expected=1)
    # 容量 1 + 慢消费者触发明确的丢新帧路径；保存内容不能出现损坏。
    config.write_text(config.read_text().replace('queue.video_raw_capacity=4', 'queue.video_raw_capacity=1'))
    result = run_case('normal', ['--frames', '30', '--consumer-delay-ms', '50'])
    values = stats(result)
    assert values['dropped_full'] > 0
    assert values['captured'] == values['enqueued'] + values['dropped_full']
    assert values['enqueued'] == values['consumed']
    for args in [['--frames', '-1'], ['--frames', '4294967296'], ['--frames', '2x'],
                 ['--consumer-delay-ms', '1001'], ['--dump-frames', '0'],
                 ['--dump-frames', '2'], ['--check-config']]:
        run_case('normal', args, expected=2, cleanup=False)
    # 子进程打开期间定向发送 SIGINT；读取首个消费日志后再发信号，避免固定 sleep 猜测。
    log = directory / 'interrupt.log'
    with log.open('w+') as handle:
        process = subprocess.Popen([str(binary), '-c', str(config), '--capture', '--frames', '0'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                   env=dict(os.environ, IPC_MOCK_CASE='normal'))
        try:
            import selectors
            selector = selectors.DefaultSelector()
            selector.register(process.stderr, selectors.EVENT_READ)
            lines = []
            # 使用 read1 的字节接口防止文本缓冲预读使 selector 漏掉已缓存行。
            accumulated = b''
            while b'[consumer] consumed=1 ' not in accumulated:
                assert selector.select(timeout=5), 'capture did not start'
                chunk = process.stderr.buffer.read1(4096)
                assert chunk, 'capture ended before signal'
                accumulated += chunk
            process.send_signal(signal.SIGINT)
            _, tail = process.communicate(timeout=8)
            text = accumulated.decode() + tail
            handle.write(text)
            assert process.returncode == 130 and 'capture interrupted' in text and 'MOCK_CLEANUP_OK' in text, text
            count += 1
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            selector.close()
print(f'PASS: {count} capture integration scenarios (mock driver, not board validation).')
