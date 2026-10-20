#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

source_dir=${1:?source directory}
output_dir=${2:?output directory}
sdk=${3:?OpenWrt SDK directory}

# Locate the prepared kernel build tree for mediatek/filogic.
kernel_symvers=$(find "$sdk/build_dir" -type f \
    -path '*/linux-mediatek_filogic/linux-*/Module.symvers' \
    -print -quit)

test -n "$kernel_symvers" || {
    echo "ERROR: matching kernel Module.symvers not found in SDK"
    exit 1
}

kernel=${kernel_symvers%/Module.symvers}
test -r "$kernel/.config"

# Locate the AArch64 cross-compiler supplied by this SDK.
gcc=$(find "$sdk/staging_dir" -type f \
    -path '*/bin/aarch64-openwrt-linux-musl-gcc' \
    -print -quit)

test -n "$gcc" || {
    echo "ERROR: AArch64 cross-compiler not found in SDK"
    exit 1
}

cross=${gcc%gcc}
kernel_release=$(make -s -C "$kernel" \
    ARCH=arm64 CROSS_COMPILE="$cross" kernelrelease)

test ! -e "$output_dir/mt7981_oc_hold.ko" || {
    echo "ERROR: output module already exists"
    exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

export STAGING_DIR="$sdk/staging_dir/target-aarch64_cortex-a53_musl"

cp "$source_dir/mt7981_oc_hold.c" \
   "$source_dir/mt7981_oc_freq.h" \
   "$source_dir/Makefile" "$work/"

make -C "$kernel" M="$work" ARCH=arm64 \
    CROSS_COMPILE="$cross" HOSTCC=/usr/bin/gcc modules

ko="$work/mt7981_oc_hold.ko"
test -s "$ko"

vermagic=$(modinfo -F vermagic "$ko")
case "$vermagic" in
    "$kernel_release "*) ;;
    *)
        echo "ERROR: vermagic does not match kernel build tree"
        echo "Expected kernel: $kernel_release"
        echo "Module vermagic: $vermagic"
        exit 1
        ;;
esac

# Verify imported symbols against this exact kernel's symbol table.
"${cross}nm" -u "$ko" |
while read -r _type name; do
    test -n "$name" || continue
    awk -v name="$name" \
        '$2 == name { found=1 } END { exit !found }' \
        "$kernel_symvers" || {
            echo "ERROR: symbol not found in Module.symvers: $name"
            exit 1
        }
done

mkdir -p "$output_dir"
cp "$ko" "$output_dir/mt7981_oc_hold.ko"

modinfo "$ko"
sha256sum "$output_dir/mt7981_oc_hold.ko"
echo "BUILD_OK_NOT_LOADED"
