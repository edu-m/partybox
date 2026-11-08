#!/usr/bin/env python3
"""Build a fake ESP for the host tests: python3 mkesp.py OUTDIR [--conf TEXT|@file]

Layout: EFI/partybox/partybox.efi plus a PARTYBOX folder with valid apps, apps
the firmware would reject, and assorted malformed or hostile files."""
import os, shutil, struct, sys

out = sys.argv[1]
build = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build")
hello = open(os.path.join(build, "test", "hello.efi"), "rb").read()
shutil.rmtree(out, ignore_errors=True)
pb = os.path.join(out, "EFI", "partybox")
box = os.path.join(pb, "PARTYBOX")
os.makedirs(box)
shutil.copy(os.path.join(build, "partybox.efi"), pb)

def put(rel, data):
    p = os.path.join(box, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)

def patch(data, off, fmt, val):
    b = bytearray(data)
    struct.pack_into(fmt, b, off, val)
    return bytes(b)

pe = struct.unpack_from("<I", hello, 0x3c)[0]
opt = pe + 24
# Valid applications and the canned behaviours of the fake firmware.
for n in ["hello", "exitdata", "binexit", "binerr", "longexit", "crashy", "warn", "modeswitch", "secfail", "denied"]:
    put(n + ".efi", hello)
shutil.copy(os.path.join(build, "blackjack.efi"), box)
# Not startable here: wrong CPU, driver, other subsystem.
put("arm64.efi", patch(hello, pe + 4, "<H", 0xAA64))
put("ia32.efi", patch(hello, pe + 4, "<H", 0x014C))
put("ebc.efi", patch(hello, pe + 4, "<H", 0x0EBC))
put("riscv128.efi", patch(hello, pe + 4, "<H", 0x5128))
put("driver.efi", patch(hello, opt + 68, "<H", 11))
put("rtdriver.efi", patch(hello, opt + 68, "<H", 12))
put("gui.efi", patch(hello, opt + 68, "<H", 2))
# Recognised by content although the name says nothing.
put("vmlinuz-fake", hello)
put("README.txt", b"not an image\n")
put("initrd.img", os.urandom(4096))
put("zero.efi", b"")
put("text.efi", b"hello world, definitely not a PE file\n" * 10)
put("mz-only.efi", b"MZ" + b"\0" * 62)
put("._junk.efi", hello)
put(".hidden.efi", hello)
# Malformed PE headers.
put("trunc-dos.efi", hello[:40])
put("trunc-pe.efi", hello[:pe + 10])
put("trunc-opt.efi", hello[:opt + 30])
put("pe-offset-huge.efi", patch(hello, 0x3c, "<I", 0xFFFFFFF0))
put("pe-offset-eof.efi", patch(hello, 0x3c, "<I", len(hello) - 4))
put("opt-size-0.efi", patch(hello, pe + 20, "<H", 0))
put("opt-size-huge.efi", patch(hello, pe + 20, "<H", 0xFFFF))
put("opt-magic-bad.efi", patch(hello, opt, "<H", 0x1234))
put("nrva-huge.efi", patch(hello, opt + 108, "<I", 0xFFFFFFFF))
# Authenticode corner cases: security directory entry 4 at opt+112+4*8.
sec = opt + 112 + 4 * 8
def with_cert(cert_off, cert_size, blob):
    d = bytearray(hello)
    struct.pack_into("<II", d, sec, cert_off, cert_size)
    while len(d) < cert_off:
        d += b"\0"
    d[cert_off:cert_off + len(blob)] = blob
    return bytes(d)
n = (len(hello) + 7) & ~7
good_pkcs7 = bytes([0x30, 0x82, 0x00, 0x20, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02]) + b"\0" * 25
cert = struct.pack("<IHH", 8 + len(good_pkcs7), 0x0200, 0x0002) + good_pkcs7
cert += b"\0" * ((8 - len(cert) % 8) % 8)
put("signed.efi", with_cert(n, len(cert), cert))
put("sec-unaligned.efi", with_cert(n + 4, len(cert), cert))
put("sec-past-eof.efi", with_cert(n, len(cert) + 4096, cert))
put("sec-off-huge.efi", with_cert(n, len(cert), cert).replace(b"", b""))
put("sec-off-wrap.efi", patch(patch(hello, sec, "<I", 0xFFFFFFF8), sec + 4, "<I", 0x10))
put("cert-len-small.efi", with_cert(n, 16, struct.pack("<IHH", 4, 0x0200, 0x0002) + b"\0" * 8))
put("cert-len-huge.efi", with_cert(n, 64, struct.pack("<IHH", 0xFFFFFFF0, 0x0200, 0x0002) + b"\0" * 56))
put("cert-der-huge.efi", with_cert(n, 64, struct.pack("<IHH", 40, 0x0200, 0x0002) + bytes([0x30, 0x84, 0xFF, 0xFF, 0xFF, 0xFF]) + b"\0" * 50))
put("cert-der-indef.efi", with_cert(n, 64, struct.pack("<IHH", 40, 0x0200, 0x0002) + bytes([0x30, 0x80]) + b"\0" * 54))
put("cert-guid-short.efi", with_cert(n, 32, struct.pack("<IHH", 24, 0x0200, 0x0EF1) + b"\0" * 24))
many = b"".join(struct.pack("<IHH", 8, 0x0200, 0x0001) for _ in range(40))
put("cert-many.efi", with_cert(n, len(many), many))
# Names and structure.
put("n" * 251 + ".efi", hello)                      # 255-char FAT name
put("café-中文.efi".encode("utf-8").decode("utf-8"), hello) if False else None
os.makedirs(os.path.join(box, "unicode-é中"), exist_ok=True)
put("unicode-é中/über.efi", hello)
put("tools/nested.efi", hello)
put("tools/deeper/ignored.efi", hello)              # two levels down: not scanned
put("tools/data.bin", os.urandom(1000))
put("empty-dir/placeholder.txt", b"x")
for i in range(40):                                  # forces the app array to grow
    put("bulk/app%02d.efi" % i, hello)
# Config next to partybox.efi.
conf = None
if "--conf" in sys.argv:
    v = sys.argv[sys.argv.index("--conf") + 1]
    conf = open(v[1:], "rb").read() if v.startswith("@") else v.encode()
if conf is not None:
    with open(os.path.join(pb, "partybox.conf"), "wb") as f:
        f.write(conf)
print(out)
