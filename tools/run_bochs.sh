#!/bin/sh
set -eu

IMG_PATH=${1:-}
if [ -z "$IMG_PATH" ]; then
  echo "Usage: $0 /absolute/path/to/floppy.img" >&2
  exit 1
fi

if ! command -v bochs >/dev/null 2>&1; then
  echo "bochs not found" >&2
  exit 1
fi

exec bochs -q -f /dev/stdin <<EOF
megs: 16
romimage: file=/usr/share/bochs/BIOS-bochs-latest
vgaromimage: file=/usr/share/bochs/VGABIOS-lgpl-latest
boot: floppy
log: bochs.log
floppya: 1_44=$IMG_PATH, status=inserted
ata0-master: type=disk, path="$IMG_PATH", mode=flat
clock: sync=realtime
cpu: count=1, ips=1000000
panic: action=ask
error: action=report
info: action=report
debug: action=ignore
EOF
