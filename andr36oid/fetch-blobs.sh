#!/bin/bash
# Fetches the binary parts the R36S mainline U-Boot is built with, pinned by
# commit and checked by SHA-256, into andr36oid/blobs/:
#   ddr.bin   Rockchip DDR init v2.12 (rkbin), debug UART moved to UART2 M1 at
#             115200 like the rest of the boot chain (rkbin's default M0 is the
#             SD card's data pins). Same DDR init as andr36oid's idbloader.img,
#             so the clones with two RAM chips start too.
#   bl31-v1.34.elf  Rockchip BL31 v1.34 for RK3326 (rkbin), for the mainline
#             chain (SPL + FIT). It loads at 0x40000, above SPL; ROCKNIX ships
#             it the same way. andr36oid's trust.img has v1.14, which loads at
#             0x10000 where SPL runs, so v1.14 only works in the classic chain.
#   bl31-v1.14.elf, bl32-v1.10.bin  the BL31 and OP-TEE in andr36oid's
#             trust.img, kept for reference and comparison.
# Needs git, curl, python3 (rkbin's ddrbin_tool.py) and sha256sum.
# Usage: andr36oid/fetch-blobs.sh        (skips what is already there)
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)/blobs
mkdir -p "$OUT"
RKBIN_REPO=https://github.com/rockchip-linux/rkbin
RKBIN_COMMIT=3e288fe814e059dd06833495f845cab04ac20a5c
DDR=bin/rk33/rk3326_ddr_333MHz_v2.12.bin
# "rk3326: bl31: update version to v1.34"
RKBIN_BL31_COMMIT=11c6b64
BL31_134=bin/rk33/rk3326_bl31_v1.34.elf
UBOOT_RAW=https://raw.githubusercontent.com/andr36oid/u-boot/d16f5c632732d0976a54a18fa63bad30dfa5d8bb
BL31=tools/rk_tools/bin/rk33/rk3326_bl31_v1.14.elf
BL32=tools/rk_tools/bin/rk33/rk3326_bl32_v1.10.bin

check() { echo "$1  $2" | sha256sum -c --quiet -; }

if [ ! -f "$OUT/ddr.bin" ]; then
    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT
    git clone -q --filter=blob:none --no-checkout "$RKBIN_REPO" "$WORK/rkbin"
    git -C "$WORK/rkbin" checkout -q "$RKBIN_COMMIT" -- "$DDR" tools/ddrbin_tool.py
    check 285a87e760a3586953f637b6de0fe80745b0ecf455dfb052bc3ecfc8b756cb7e "$WORK/rkbin/$DDR"
    cp "$WORK/rkbin/$DDR" "$WORK/ddr.bin"
    (cd "$WORK" && python3 rkbin/tools/ddrbin_tool.py rk3326 -g param.txt ddr.bin >/dev/null)
    sed -i -e 's/^uart iomux=.*/uart iomux=1/' -e 's/^uart baudrate=.*/uart baudrate=115200/' "$WORK/param.txt"
    (cd "$WORK" && python3 rkbin/tools/ddrbin_tool.py rk3326 param.txt ddr.bin >/dev/null)
    cp "$WORK/ddr.bin" "$OUT/ddr.bin"
fi

if [ ! -f "$OUT/bl31-v1.34.elf" ]; then
    WORK2=$(mktemp -d)
    git clone -q --filter=blob:none --no-checkout "$RKBIN_REPO" "$WORK2/rkbin"
    git -C "$WORK2/rkbin" checkout -q "$RKBIN_BL31_COMMIT" -- "$BL31_134"
    check 387aaee716a6753a9e6dd18c72b27783d125a294f6aa817531a5a6f340f16080 "$WORK2/rkbin/$BL31_134"
    cp "$WORK2/rkbin/$BL31_134" "$OUT/bl31-v1.34.elf"
    rm -rf "$WORK2"
fi

for f in bl31-v1.14.elf:$BL31:2ea5c13f0f253cbf0d388802fe57c82217c5ec5d0b9ef4cae6867d710d073d07 \
         bl32-v1.10.bin:$BL32:8ca2812707e3b08c765fe2a569c5b672b9d2c0417f1209e5156e1e3938bcba51; do
    name=${f%%:*}; rest=${f#*:}; path=${rest%%:*}; sum=${rest#*:}
    if [ ! -f "$OUT/$name" ]; then
        curl -fsSL "$UBOOT_RAW/$path" -o "$OUT/$name.tmp"
        check "$sum" "$OUT/$name.tmp"
        mv "$OUT/$name.tmp" "$OUT/$name"
    fi
done

(cd "$OUT" && sha256sum ddr.bin bl31-v1.34.elf bl31-v1.14.elf bl32-v1.10.bin)
