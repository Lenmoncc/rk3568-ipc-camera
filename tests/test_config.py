#!/usr/bin/env python3
"""配置/命令行行为测试：使用临时文件，不访问摄像头、声卡或网络。"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
base = (root / "configs/ipc.conf").read_text()
count = 0


def change(key, value, source=base):
    return "\n".join(key + "=" + value if line.startswith(key + "=") else line
                     for line in source.splitlines()) + "\n"


def run(args, cwd=None, env=None):
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, timeout=5)


with tempfile.TemporaryDirectory(prefix="ipc-config-test-") as name:
    work = Path(name)
    config = work / "test.conf"

    def check(label, content, expected=0, contains=None):
        global count
        config.write_bytes(content if isinstance(content, bytes) else content.encode())
        result = run([str(binary), "--config", str(config), "--check-config"], cwd=work)
        assert result.returncode == expected, (label, result.returncode, result.stderr)
        if contains:
            assert contains in result.stderr, (label, contains, result.stderr)
        count += 1

    check("disabled placeholder", base, contains="enabled=false")
    check("empty URL disabled", change("output.rtmp_url", ""))
    check("empty URL enabled", change("output.rtmp_url", "", change("output.rtmp_enabled", "true")), 1,
          "output.rtmp_url")
    check("placeholder enabled", change("output.rtmp_enabled", "true"), 1, "SRS_SERVER_IP")
    enabled = change("output.rtmp_url", "rtmp://192.0.2.1:1935/live/camera",
                     change("output.rtmp_enabled", "true"))
    # 文档保留地址不需要可达，确保配置检查不会探测服务器。
    check("syntactically valid enabled URL", enabled)
    check("hostname URL", change("output.rtmp_url", "rtmp://srs.example.com/live/camera", enabled))
    for label, value in [("bad scheme", "http://host/live/stream"),
                         ("bad port", "rtmp://host:65536/live/stream"),
                         ("bad host", "rtmp://-host/live/stream"),
                         ("bad IPv4", "rtmp://999.1.1.1/live/stream"),
                         ("missing stream", "rtmp://host/live/"),
                         ("userinfo unsupported", "rtmp://user@host/live/stream")]:
        check(label, change("output.rtmp_url", value), 1, "output.rtmp_url")
    check("CRLF BOM", b"\xef\xbb\xbf" + base.replace("\n", "\r\n").encode())
    check("no final newline", base.rstrip("\n"))
    check("whitespace", base.replace("video.width=1280", "  video.width \t= 1280 \t"))
    check("unknown key", base + "video.wdith=1280\n", 1, "unknown key")
    check("duplicate key", base + "video.width=1280\n", 1, "first defined at line")
    check("missing key", base.replace("video.width=1280\n", ""), 1, "video.width: missing required key")
    check("empty key", base + "=something\n", 1, "<empty-key>")
    check("bad syntax", base + "not-a-key-value\n", 1, "expected key=value")
    line = next(i for i, s in enumerate(base.splitlines(), 1) if s.startswith("audio.channels="))
    check("line-number error", change("audio.channels", "0"), 1, f":{line}: audio.channels:")
    for value in ("-1", "+1", "1x", "1.5", "184467440737095516160"):
        check("bad integer " + value, change("video.fps", value), 1, "video.fps")
    check("4K not accepted", change("video.width", "3840"), 1, "video.width")
    check("format checked", change("video.pixel_format", "YU12"), 1, "requires NV12")
    check("codec checked", change("video.codec", "hevc"), 1, "requires h264")
    check("audio zero", change("audio.sample_rate", "0"), 1, "audio.sample_rate")
    check("audio nonstandard rate", change("audio.sample_rate", "45000"), 1, "unsupported AAC sample rate")
    check("audio FLTP capture rejected", change("audio.sample_format", "FLTP"), 1, "requires S16_LE")
    check("queue cannot be empty", change("queue.video_raw_capacity", "0"), 1, "queue.video_raw_capacity")
    check("queue upper bound", change("queue.video_raw_capacity", "17"), 1, "queue.video_raw_capacity")
    check("overlong string", change("audio.device", "a" * 128), 1, "string too long")
    check("empty device", change("audio.device", ""), 1, "required value is empty")
    check("non-absolute record", change("output.record_path", "record.mp4"), 1, "absolute path")
    check("wrong recording suffix", change("output.record_path", "/tmp/record.flv"), 1, ".mp4")
    check("overlong comment", base + "#" * 1024, 1, "longer than")
    check("embedded NUL", base.encode() + b"log.level=info\0garbage\n", 1, "control character")
    check("embedded control", change("audio.device", "hw:0,\t0"), 1, "control characters")
    check("unsupported inline comment", change("video.fps", "30 # fps"), 1, "video.fps")
    check("optional log.level", base.replace("log.level=info\n", ""))
    check("debug level", change("log.level", "debug"), contains="[DEBUG]")
    check("invalid log.level", change("log.level", "verbose"), 1, "log.level")
    check("invalid bool", change("output.rtmp_enabled", "yes"), 1, "expected true or false")
    config.write_text(change("log.level", "warn"))
    result = run([str(binary), "-c", str(config)])
    assert result.returncode == 0 and "[INFO]" not in result.stderr and "[WARN]" in result.stderr
    count += 1
    for args, expected in [(["--config", str(work / "missing.conf")], 1),
                           (["--config"], 2), (["--unknown"], 2),
                           (["unexpected"], 2), (["--help"], 0)]:
        result = run([str(binary), *args], cwd=work)
        assert result.returncode == expected, (args, result.stderr)
        count += 1

    # 模拟 ARM64 识别，仅验证启动脚本的路径拼接；仍运行本机二进制。
    deployment = work / "deployment with spaces"
    for part in ("scripts", "configs", "bin", "mock-tools"):
        (deployment / part).mkdir(parents=True)
    shutil.copy2(root / "scripts/run.sh", deployment / "scripts/run.sh")
    shutil.copy2(binary, deployment / "bin/ipc_camera")
    (deployment / "configs/ipc.conf").write_text(base)
    mock = deployment / "mock-tools/uname"
    mock.write_text("#!/bin/sh\nprintf 'aarch64\\n'\n")
    mock.chmod(0o755)
    env = dict(os.environ, PATH=str(mock.parent) + os.pathsep + os.environ["PATH"])
    result = run(["sh", str(deployment / "scripts/run.sh"), "--check-config"], cwd=work, env=env)
    assert result.returncode == 0 and "configuration validation passed" in result.stderr, result.stderr
    count += 1
    alternate = work / "alternate.conf"
    alternate.write_text(change("video.bitrate", "3000000"))
    result = run(["sh", str(deployment / "scripts/run.sh"), "--config", str(alternate)], cwd=work, env=env)
    assert result.returncode == 0 and "bitrate=3000000" in result.stderr, result.stderr
    count += 1

print(f"PASS: {count} configuration, CLI and launch-path cases (host only).")
