#!/bin/bash
# Build the smart-thermostat demo with the RZ/A2M Linux 7.x BSP toolchain and
# install it into that BSP's rootfs overlay.
#
#   ./install.sh <BSP folder> [board]            build and install (board: rza2mevb)
#   ./install.sh <BSP folder> [board] remove     take the demo out of the overlay
#
# Then, in the BSP:  ./build.sh buildroot && ./build.sh images
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BSP=$(cd "${1:?usage: ./install.sh <BSP folder> [board] [remove]}" && pwd)
BOARD=${2:-rza2mevb}
OVL=$BSP/board/$BOARD/overlay
[ -d "$OVL" ] || { echo "No overlay folder: $OVL"; exit 1; }

if [ "$3" = "remove" ]; then
  rm -f "$OVL/root/bin/thermostat_a2m" "$OVL/etc/init.d/S99demo"
  rm -rf "$OVL/root/video"
  echo "Demo removed from $OVL. In the BSP: ./build.sh buildroot && ./build.sh images"
  exit 0
fi

export ROOTDIR=$BSP
. "$BSP/setup_env.sh" >/dev/null
[ -n "$CROSS_COMPILE" ] || { echo "No BSP toolchain yet: run ./build.sh buildroot in the BSP first"; exit 1; }

cd "$HERE"
make CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)"
mkdir -p "$OVL/root/bin" "$OVL/etc/init.d"
"${CROSS_COMPILE}strip" -o "$OVL/root/bin/thermostat_a2m" thermostat_a2m
install -m 755 bsp/S99demo "$OVL/etc/init.d/S99demo"
mkdir -p "$OVL/root/video"
install -m 644 video/doorbell.h264 "$OVL/root/video/doorbell.h264"
ls -lh "$OVL/root/bin/thermostat_a2m" "$OVL/etc/init.d/S99demo"
echo "Installed. In the BSP: ./build.sh buildroot && ./build.sh images"
