#!/bin/bash
# Builds the R36S-family mainline U-Boot (U-Boot v2026.10 + the andr36oid board
# support in this tree) in two flavors:
#
#   mainline  idbloader.img = Rockchip DDR init v2.12 (TPL) + mainline SPL
#             u-boot.itb    = FIT: U-Boot + BL31 v1.34 (rkbin) + control DTB
#             Write idbloader.img at sector 64 and u-boot.itb at sector 16384
#             (or u-boot-rockchip.bin at sector 64). The old trust.img area
#             (sector 24576) is not used.
#   classic   uboot.img     = mainline U-Boot proper packed for the Rockchip
#             miniloader (4 x 1 MiB at sector 16384). Keeps andr36oid's
#             idbloader.img (DDR v2.12 + miniloader v1.40) and trust.img (BL31
#             v1.14 + OP-TEE v1.10) as they are: only U-Boot itself changes.
#
# Output: $OUT/<flavor>/ (build tree, default /tmp/uboot-mainline-out) and
# andr36oid/dist/ (the files to write).
# Needs: aarch64-linux-gnu- cross compiler, make, bison, flex, bc, python3 with
# pyelftools, swig, openssl and uuid headers, curl, git.
# Usage: andr36oid/build.sh [mainline|classic|all]   (JOBS=N, OUT=build dir)
set -euo pipefail
TOP=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$TOP/.." && pwd)
FLAVORS=${1:-all}
JOBS=${JOBS:-$(nproc)}
OUT=${OUT:-/tmp/uboot-mainline-out}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
export CROSS_COMPILE

"$TOP/fetch-blobs.sh" >/dev/null
mkdir -p "$TOP/dist"

build_one() {
    local flavor=$1 out=$OUT/$1
    shift
    mkdir -p "$out"
    make -s -C "$SRC" O="$out" r36s-rk3326_defconfig </dev/null
    if [ "$flavor" = classic ]; then
        # U-Boot proper runs where the miniloader puts uboot.img
        "$SRC/scripts/config" --file "$out/.config" \
            --set-val TEXT_BASE 0x00200000 \
            --set-str IDENT_STRING " classic"
    else
        "$SRC/scripts/config" --file "$out/.config" \
            --set-str IDENT_STRING " mainline"
    fi
    make -s -C "$SRC" O="$out" olddefconfig </dev/null
    make -C "$SRC" O="$out" -j"$JOBS" "$@" </dev/null
}

if [ "$FLAVORS" = mainline ] || [ "$FLAVORS" = all ]; then
    build_one mainline \
        ROCKCHIP_TPL="$TOP/blobs/ddr.bin" BL31="$TOP/blobs/bl31-v1.34.elf"
    for f in idbloader.img u-boot.itb u-boot-rockchip.bin; do
        cat "$OUT/mainline/$f" > "$TOP/dist/$f"
    done
fi

if [ "$FLAVORS" = classic ] || [ "$FLAVORS" = all ]; then
    # SPL and binman output are not used; BL31 only keeps binman quiet
    build_one classic \
        ROCKCHIP_TPL="$TOP/blobs/ddr.bin" BL31="$TOP/blobs/bl31-v1.34.elf"
    python3 -I "$TOP/pack-uboot-img.py" "$OUT/classic/u-boot.bin" \
        "$TOP/dist/uboot.img" 0x00200000
fi

(cd "$TOP/dist" && ls -l && sha256sum ./*)
