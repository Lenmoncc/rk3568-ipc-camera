#!/usr/bin/env python3
"""检查 4 个线程的 400 条日志完整、不交错，且等级过滤生效。"""
import re
import subprocess
import sys
result = subprocess.run([sys.argv[1]], capture_output=True, text=True, timeout=10)
assert result.returncode == 0, result.stderr
lines = result.stderr.splitlines()
assert len(lines) == 400, len(lines)
pattern = re.compile(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[INFO\] \[test\] thread=(\d) item=(\d+)")
records = []
for line in lines:
    match = pattern.fullmatch(line)
    assert match, line
    records.append(tuple(map(int, match.groups())))
assert set(records) == {(t, i) for t in range(4) for i in range(100)}
assert "THIS_MUST_NOT_APPEAR" not in result.stderr
print("PASS: 400 complete concurrent log records and level filtering.")
