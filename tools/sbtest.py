#!/usr/bin/env python3
"""Minimal Secure Boot test kit (for QEMU testing only -- not a production signer).

  sbtest.py keygen DIR            -> DIR/db.key, DIR/db.pem (self-signed RSA-2048)
  sbtest.py sign KEY CERT IN OUT  -> Authenticode-sign a PE (like `sbsign`)

Needs python-cryptography. Signatures are PKCS#7 SignedData over an
SpcIndirectDataContent with the SHA-256 Authenticode image hash.
"""
import datetime
import hashlib
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID

# ---- tiny DER encoder -------------------------------------------------------

def _len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, content):
    return bytes([tag]) + _len(len(content)) + content


def seq(*items):
    return tlv(0x30, b"".join(items))


def set_of(*items):
    return tlv(0x31, b"".join(sorted(items)))


def oid(dotted):
    parts = [int(x) for x in dotted.split(".")]
    out = bytes([40 * parts[0] + parts[1]])
    for p in parts[2:]:
        enc = [p & 0x7F]
        p >>= 7
        while p:
            enc.append(0x80 | (p & 0x7F))
            p >>= 7
        out += bytes(reversed(enc))
    return tlv(0x06, out)


def integer(v):
    return tlv(0x02, v.to_bytes(v.bit_length() // 8 + 1, "big", signed=True))


def octets(b):
    return tlv(0x04, b)


NULL = b"\x05\x00"
SHA256 = seq(oid("2.16.840.1.101.3.4.2.1"), NULL)
RSA = seq(oid("1.2.840.113549.1.1.1"), NULL)
SPC_INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4"
SPC_PE_IMAGE_DATA = "1.3.6.1.4.1.311.2.1.15"
SPC_SP_OPUS_INFO = "1.3.6.1.4.1.311.2.1.12"

# ---- Authenticode PE hash ------------------------------------------------------

def pe_layout(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"
    nsect, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    secdir = opt + (144 if magic == 0x20B else 128)
    return opt, secdir, nsect, opt + opt_size


def authenticode_hash(data):
    opt, secdir, nsect, sects = pe_layout(data)
    cksum = opt + 64
    hdr_size = struct.unpack_from("<I", data, opt + 60)[0]
    h = hashlib.sha256()
    h.update(data[:cksum])
    h.update(data[cksum + 4:secdir])
    h.update(data[secdir + 8:hdr_size])
    sections = []
    for i in range(nsect):
        raw_size, raw_ptr = struct.unpack_from("<II", data, sects + 40 * i + 16)
        if raw_size:
            sections.append((raw_ptr, raw_size))
    done = hdr_size
    for ptr, size in sorted(sections):
        h.update(data[ptr:ptr + size])
        done += size
    cert_off, cert_size = struct.unpack_from("<II", data, secdir)
    end = len(data) - (cert_size if cert_off else 0)
    if end > done:
        h.update(data[done:end])
    return h.digest()

# ---- commands -------------------------------------------------------------------

def keygen(outdir):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "partybox test db")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .sign(key, hashes.SHA256()))
    with open(f"{outdir}/db.key", "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    with open(f"{outdir}/db.pem", "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))


def sign(key_path, cert_path, src, dst):
    key = serialization.load_pem_private_key(open(key_path, "rb").read(), None)
    cert = x509.load_pem_x509_certificate(open(cert_path, "rb").read())
    data = bytearray(open(src, "rb").read())
    _, secdir, _, _ = pe_layout(data)
    assert struct.unpack_from("<II", data, secdir) == (0, 0), "already signed"
    data += b"\0" * (-len(data) % 8)  # certificate table must be 8-byte aligned

    digest = authenticode_hash(bytes(data))
    obsolete = "<<<Obsolete>>>".encode("utf-16-be")
    pe_image_data = seq(tlv(0x03, b"\x00"), tlv(0xA0, tlv(0xA2, tlv(0x80, obsolete))))
    spc = seq(seq(oid(SPC_PE_IMAGE_DATA), pe_image_data), seq(SHA256, octets(digest)))
    # Authenticode's messageDigest covers the SpcIndirectDataContent value only (no tag/length).
    spc_value = seq_content(spc)

    attrs = [
        seq(oid("1.2.840.113549.1.9.3"), set_of(oid(SPC_INDIRECT_DATA))),
        seq(oid("1.2.840.113549.1.9.4"), set_of(octets(hashlib.sha256(spc_value).digest()))),
        seq(oid(SPC_SP_OPUS_INFO), set_of(seq())),
    ]
    signed_attrs = set_of(*attrs)
    signature = key.sign(signed_attrs, padding.PKCS1v15(), hashes.SHA256())

    cert_der = cert.public_bytes(serialization.Encoding.DER)
    signer = seq(
        integer(1),
        seq(cert.issuer.public_bytes(), integer(cert.serial_number)),
        SHA256,
        tlv(0xA0, b"".join(sorted(attrs))),
        RSA,
        octets(signature),
    )
    signed_data = seq(
        integer(1),
        set_of(SHA256),
        seq(oid(SPC_INDIRECT_DATA), tlv(0xA0, spc)),
        tlv(0xA0, cert_der),
        set_of(signer),
    )
    pkcs7 = seq(oid("1.2.840.113549.1.7.2"), tlv(0xA0, signed_data))

    wincert = struct.pack("<IHH", 8 + len(pkcs7), 0x0200, 0x0002) + pkcs7
    wincert += b"\0" * (-len(wincert) % 8)
    struct.pack_into("<II", data, secdir, len(data), len(wincert))
    data += wincert
    open(dst, "wb").write(data)


def seq_content(der):
    """Content bytes of a DER TLV."""
    n = der[1]
    if n < 0x80:
        return der[2:2 + n]
    k = n & 0x7F
    ln = int.from_bytes(der[2:2 + k], "big")
    return der[2 + k:2 + k + ln]


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "keygen":
        keygen(sys.argv[2])
    elif len(sys.argv) == 6 and sys.argv[1] == "sign":
        sign(*sys.argv[2:])
    else:
        sys.exit(__doc__)
