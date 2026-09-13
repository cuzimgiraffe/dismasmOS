#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

which gcc >/dev/null 2>&1 || { echo "error: gcc not found"; exit 1; }
which ld >/dev/null 2>&1 || { echo "error: ld not found"; exit 1; }
which grub-mkrescue >/dev/null 2>&1 || { echo "error: grub-mkrescue not found"; exit 1; }
which xorriso >/dev/null 2>&1 || { echo "error: xorriso not found"; exit 1; }

make clean
make

mkdir -p isodir/boot/grub
cp dismasmOS.bin isodir/boot/dismasmOS.bin
cp grub.cfg isodir/boot/grub/grub.cfg
grub-mkrescue -o dismasmOS.iso isodir
rm -rf isodir

echo "ISO build completed: dismasmOS.iso"
