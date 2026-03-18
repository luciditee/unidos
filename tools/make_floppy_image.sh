#!/bin/sh
set -eu

: "${STAGE1_BIN:?STAGE1_BIN is required}"
: "${STAGE2_BIN:?STAGE2_BIN is required}"
: "${KERNEL_BIN:?KERNEL_BIN is required}"
: "${IMG_OUT:?IMG_OUT is required}"
: "${STAGE2_RESERVED_SECTORS:=6}"
: "${KPARAMS_DEFAULT_SRC:=}"
: "${NOKPARAMS:=OFF}"

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

need_cmd mkfs.fat
need_cmd mcopy
need_cmd dd
need_cmd truncate
need_cmd stat

if [ "$(stat -c%s "$STAGE1_BIN")" -ne 512 ]; then
  echo "stage1 binary must be exactly 512 bytes (got $(stat -c%s "$STAGE1_BIN"))" >&2
  exit 1
fi

stage2_max_bytes=$((STAGE2_RESERVED_SECTORS * 512))
stage2_bytes=$(stat -c%s "$STAGE2_BIN")
if [ "$stage2_bytes" -gt "$stage2_max_bytes" ]; then
  echo "stage2 binary ($stage2_bytes bytes) exceeds reserved area (${stage2_max_bytes} bytes = ${STAGE2_RESERVED_SECTORS} sectors)" >&2
  exit 1
fi

# Reserved sectors in FAT include boot sector at LBA 0 + stage2 raw sectors.
fat_reserved_sectors=$((1 + STAGE2_RESERVED_SECTORS))

mkdir -p "$(dirname "$IMG_OUT")"
rm -f "$IMG_OUT"

truncate -s 1474560 "$IMG_OUT"
mkfs.fat -F 12 -R "$fat_reserved_sectors" -n UNIDOS "$IMG_OUT" >/dev/null

# Copy kernel into FAT12 filesystem.
mcopy -i "$IMG_OUT" "$KERNEL_BIN" ::KERNEL.BIN

should_skip_kparams=0
case "$NOKPARAMS" in
  1|ON|on|TRUE|true|YES|yes)
    should_skip_kparams=1
    ;;
esac

if [ "$should_skip_kparams" -eq 0 ]; then
  if [ -z "$KPARAMS_DEFAULT_SRC" ]; then
    echo "KPARAMS injection requested but KPARAMS_DEFAULT_SRC is not set" >&2
    exit 1
  fi
  if [ ! -f "$KPARAMS_DEFAULT_SRC" ]; then
    echo "Default kparams file not found: $KPARAMS_DEFAULT_SRC" >&2
    exit 1
  fi
  mcopy -i "$IMG_OUT" "$KPARAMS_DEFAULT_SRC" ::KPARAMS.DAT
  echo "Injected KPARAMS.DAT from: $KPARAMS_DEFAULT_SRC"
else
  echo "Skipping KPARAMS.DAT injection (NOKPARAMS=$NOKPARAMS)"
fi

# Write stage2 raw loader into reserved sectors starting at LBA 1.
dd if="$STAGE2_BIN" of="$IMG_OUT" bs=512 seek=1 conv=notrunc status=none

# Write stage1 boot sector last.
dd if="$STAGE1_BIN" of="$IMG_OUT" bs=512 count=1 conv=notrunc status=none

echo "Built image: $IMG_OUT"
