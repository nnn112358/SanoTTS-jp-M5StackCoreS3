#!/usr/bin/env python3
"""QEMU（Espressif fork）で esp32 のビルドを起動し、シリアルに 1 行入れてログを取る。

  uv run --no-project python scripts/qemu_run.py build_basic_qemu logs/qemu_basic.log [--line 今日は良い天気ですね。] [--secs 240]

ESP-IDF の export.sh を通した環境で呼ぶこと（qemu-system-xtensa と esptool が PATH に要る）。
flash イメージは build/flash_args から esptool merge_bin で作る（辞書パーティション込み、16 MB 埋め）。
"""
import argparse, binascii, os, re, subprocess, sys, time, threading

ap = argparse.ArgumentParser()
ap.add_argument("build"); ap.add_argument("log")
ap.add_argument("--line", default="今日は良い天気ですね。")
ap.add_argument("--secs", type=float, default=240)
ap.add_argument("--target", default="esp32")
a = ap.parse_args()

bdir = os.path.abspath(a.build)
flash = os.path.join(bdir, "qemu_flash.bin")
efuse = os.path.join(bdir, "qemu_efuse.bin")
args = open(os.path.join(bdir, "flash_args")).read().split()
size = args[args.index("--flash_size") + 1]
merge = ["esptool.py", "--chip", a.target, "merge_bin", "-o", flash, "--fill-flash-size", size] + args
subprocess.run(merge, cwd=bdir, check=True, stdout=subprocess.DEVNULL)

# eFuse の既定値は IDF の qemu_ext.py から拾う（chip revision 3）
qe = open(os.path.join(os.environ["IDF_PATH"], "tools/idf_py_actions/qemu_ext.py")).read()
m = re.search(r"'esp32': QemuTarget\(.*?binascii\.unhexlify\((.*?)\)", qe, re.S)
hexs = "".join(re.findall(r"'([0-9a-f]+)'", m.group(1)))
open(efuse, "wb").write(binascii.unhexlify(hexs))

cmd = ["qemu-system-xtensa", "-M", "esp32", "-m", "4M",
       "-drive", f"file={flash},if=mtd,format=raw",
       "-drive", f"file={efuse},if=none,format=raw,id=efuse",
       "-global", "driver=nvram.esp32.efuse,property=drive,value=efuse",
       "-global", "driver=timer.esp32.timg,property=wdt_disable,value=true",
       "-nic", "user,model=open_eth", "-nographic", "-monitor", "none"]
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = open(os.path.abspath(a.log), "wb")
buf = b""; sent = False; t0 = time.time(); done_utter = 0
def reader():
    global buf
    while True:
        c = p.stdout.read(1)
        if not c: break
        buf += c; out.write(c); out.flush()
threading.Thread(target=reader, daemon=True).start()
while time.time() - t0 < a.secs:
    time.sleep(0.5)
    if not sent and b"\xe3\x81\x8b\xe3\x81\xaa> " in buf and buf.count("結果".encode()) >= 1:
        time.sleep(1.0)
        p.stdin.write((a.line + "\n").encode()); p.stdin.flush(); sent = True
        print(f"[{time.time()-t0:.0f}s] 入力: {a.line}", flush=True)
    if sent and buf.count("結果".encode()) >= 2 and buf.count("かな> ".encode()) >= 2:
        time.sleep(1.0); break
    if b"Guru Meditation" in buf or b"abort()" in buf:
        time.sleep(2.0); break
p.kill(); out.close()
print(f"log: {a.log} ({len(buf)} B, {time.time()-t0:.0f} s)")
