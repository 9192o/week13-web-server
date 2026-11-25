export QA=/workspace/qa
set -o pipefail
{
  uname -smr; id; ulimit -n
  cc --version; make --version; python3 --version; curl --version; valgrind --version
  printf 'cpu.max '; head -n 1 /sys/fs/cgroup/cpu.max
  printf 'memory.max '; head -n 1 /sys/fs/cgroup/memory.max
} > "$QA/logs/environment.log" 2>&1
test "$(id -u)" -ne 0 && echo nonroot-ok
cd "$QA"
python3 - <<'PY'
from pathlib import Path
for name in ("one", "two"):
    root = Path(name)
    root.mkdir(exist_ok=True)
    for ext in ("html", "css", "js", "png", "jpg"):
        suffix = b"\0\xff" if ext in ("png", "jpg") else b""
        (root / f"file.{ext}").write_bytes((ext.encode() + suffix) * 21 if name == "one" else b"other root")
Path("one/small.html").write_bytes(b"x" * 1024)
Path("one/big.png").write_bytes(b"\0\xde\xff\x01" * (2 * 1024 * 1024))
locked = Path("one/locked.html")
locked.touch()
locked.chmod(0)
Path("post-text.bin").write_bytes(b"text-1")
Path("post-binary.bin").write_bytes(b"\0\xffbinary")
PY
sha256sum one/file.* one/small.html one/big.png two/file.html post-*.bin > logs/fixtures.sha256
cat logs/environment.log | head -30; cat logs/fixtures.sha256
