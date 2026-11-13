#!/usr/bin/env python3
"""Pack a U-Boot binary as the vendor uboot.img the Rockchip miniloader loads.

Same output as Rockchip's tools/rockchip/loaderimage.c ("--pack --uboot"):
a 2 KiB header (magic "LOADER  ", load address, size, Rockchip CRC-32 and a
SHA-256 over the payload and the header fields) in front of the payload,
the whole 1 MiB slot repeated 4 times.

Usage: pack-uboot-img.py <u-boot.bin> <uboot.img> [load_addr] [--unpack]
"""
import hashlib
import struct
import sys

HDR_SIZE = 2048
SLOT_SIZE = 1024 * 1024
SLOTS = 4
MAGIC = b"LOADER  "


def _crc_table():
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C10DB7) if c & 0x80000000 else (c << 1)
        table.append(c & 0xFFFFFFFF)
    return table


CRC_TABLE = _crc_table()


def crc32_rk(data, crc=0):
    for b in data:
        crc = (CRC_TABLE[((crc >> 24) ^ b) & 0xFF] ^ (crc << 8)) & 0xFFFFFFFF
    return crc


def pack(payload, load_addr, version=0):
    if len(payload) > SLOT_SIZE - HDR_SIZE:
        raise SystemExit("payload is %d bytes, the slot holds %d"
                         % (len(payload), SLOT_SIZE - HDR_SIZE))
    size = (len(payload) + 3) & ~3
    body = payload + b"\0" * (size - len(payload))
    sha = hashlib.sha256(body)
    if version > 0:
        sha.update(struct.pack("<II", version, 0))
    sha.update(struct.pack("<III", load_addr, size, 32))
    hdr = MAGIC + struct.pack("<IIIIII", version, 0, load_addr, size,
                              crc32_rk(body), 32) + sha.digest()
    hdr += b"\0" * (HDR_SIZE - len(hdr))
    slot = hdr + body
    slot += b"\0" * (SLOT_SIZE - len(slot))
    return slot * SLOTS


def unpack(img):
    if img[:8] != MAGIC:
        raise SystemExit("not a uboot.img (magic %r)" % img[:8])
    version, _, load_addr, size, crc, hash_len = struct.unpack_from("<IIIIII", img, 8)
    body = img[HDR_SIZE:HDR_SIZE + size]
    if crc32_rk(body) != crc:
        raise SystemExit("CRC mismatch")
    return body, load_addr, version


def main(argv):
    if len(argv) >= 4 and argv[-1] == "--unpack":
        body, addr, ver = unpack(open(argv[1], "rb").read())
        open(argv[2], "wb").write(body)
        print("unpacked %d bytes, load address 0x%x, version %d" % (len(body), addr, ver))
        return
    if len(argv) < 3:
        raise SystemExit(__doc__)
    load_addr = int(argv[3], 0) if len(argv) > 3 else 0x00200000
    payload = open(argv[1], "rb").read()
    open(argv[2], "wb").write(pack(payload, load_addr))
    print("packed %d bytes at 0x%x into %s" % (len(payload), load_addr, argv[2]))


if __name__ == "__main__":
    main(sys.argv)
