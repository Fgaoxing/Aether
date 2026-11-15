#!/usr/bin/env python3
"""
pty_test.py — 端到端交互冒烟：在 pty 里跑 aether，发送命令并检查输出。
用法：python3 tools/pty_test.py ./build-nolibc/aether
"""
import os, pty, sys, time, select

cmd = sys.argv[1:]
if not cmd:
    sys.exit("usage: pty_test.py <aether-binary>")

pid, fd = pty.fork()
if pid == 0:
    os.execv(cmd[0], cmd)

out = b""
timeline = [
    (0.0, b"echo pty-works\n"),
    (0.8, b"echo a | cat > /tmp/aether_pty_check.txt\n"),
    (1.6, b"cat /tmp/aether_pty_check.txt\n"),
    (2.4, b"\x03"),                 # Ctrl-C：shell 必须活着
    (3.2, b"echo still-alive\n"),
    (4.0, b"exit\n"),
]
start = time.time()
sent = 0
deadline = start + 8.0
child_status = None
while time.time() < deadline:
    now = time.time() - start
    while sent < len(timeline) and timeline[sent][0] <= now:
        os.write(fd, timeline[sent][1])
        sent += 1
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            wpid, status = os.waitpid(pid, 0)  # 子进程已退出（EIO=pty 已关闭），立即返回
            child_status = status
            break
        if not chunk:
            break
        out += chunk
    wpid, status = os.waitpid(pid, os.WNOHANG)
    if wpid == pid:
        child_status = status
        for _ in range(20):
            r, _, _ = select.select([fd], [], [], 0.1)
            if not r:
                break
            try:
                out += os.read(fd, 4096)
            except OSError:
                break
        break

text = out.decode("utf-8", "replace")
print(text)
# 注意 OPOST 把 \n 变成 \r\n，检查子串时按终端实际输出
ok = all(s in text for s in ("pty-works", "still-alive", "aether /"))
print("RC:", child_status, "PASS" if ok else "FAIL")
sys.exit(0 if ok and child_status == 0 else 1)
