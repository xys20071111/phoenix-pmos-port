#!/bin/bash
# split-image.sh - split pmbootstrap combined image into boot/root partition images
# Usage: ./split-image.sh <combined.img> [outdir]
# Verifies MBR, extracts by partition table (not hardcoded offsets),
# checks filesystem labels/UUIDs with blkid, prints flash commands.
set -euo pipefail

IMG="${1:?Usage: $0 <combined.img> [outdir]}"
OUT="${2:-./images}"

if [ ! -f "$IMG" ]; then
	echo "ERROR: $IMG not found" >&2
	exit 1
fi

SIG=$(dd if="$IMG" bs=512 count=1 2>/dev/null | od -A n -t x1 -j 510 -N 2 | tr -d ' \n')
if [ "$SIG" != "55aa" ]; then
	echo "ERROR: no MBR signature (got $SIG), not a pmbootstrap combined image?" >&2
	exit 1
fi

read -r B_LBA B_SZ R_LBA R_SZ <<< "$(python3 - "$IMG" <<'PYEOF'
import struct, sys
d = open(sys.argv[1], 'rb').read(4096)
parts = []
for i in range(4):
    e = d[446+i*16:446+i*16+16]
    boot, typ = e[0], e[4]
    lba, sz = struct.unpack('<II', e[8:16])
    if sz and typ == 0x83:
        parts.append((boot, lba, sz))
if len(parts) < 2:
    sys.exit('ERROR: need 2+ Linux partitions')
parts.sort(key=lambda p: p[1])
(b0, blba, bsz), (_, rlba, rsz) = parts[0], parts[1]
print(blba, bsz, rlba, rsz)
PYEOF
)"
echo "boot part : LBA $B_LBA sectors $B_SZ ($((B_SZ*512/1024/1024)) MiB)"
echo "root part : LBA $R_LBA sectors $R_SZ ($((R_SZ*512/1024/1024)) MiB)"

mkdir -p "$OUT"
dd if="$IMG" of="$OUT/boot-part.img" bs=512 skip="$B_LBA" count="$B_SZ" status=none
dd if="$IMG" of="$OUT/root-part.img" bs=512 skip="$R_LBA" count="$R_SZ" status=none

echo "--- verify ---"
blkid "$OUT/boot-part.img" "$OUT/root-part.img"

BOOT_UUID=$(blkid -s UUID -o value "$OUT/boot-part.img")
ROOT_UUID=$(blkid -s UUID -o value "$OUT/root-part.img")
echo "--- next steps (fastboot, device in bootloader) ---"
echo "fastboot flash -S 200M system $OUT/boot-part.img"
echo "fastboot flash -S 200M userdata $OUT/root-part.img"
echo "--- then set fdt in extlinux.conf (TWRP shell) ---"
echo "mount -t ext2 /dev/block/mmcblk0p22 /tmp/pmsys  # adjust fdt line, umount"
echo "expected pmos_boot_uuid=$BOOT_UUID"
echo "expected pmos_root_uuid=$ROOT_UUID"
