#!/bin/bash
# Program RZ/A2M EVK QSPI flash with SEGGER J-Link (Linux host)
#
#   ./flash.sh all       U-Boot, DTB, kernel and rootfs
#   ./flash.sh uboot     only U-Boot (also: dtb, kernel, rootfs)
#   ./flash.sh           asks which one
#
# Images are taken from this folder, else from ../output/images.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -f "$HERE/xipImage.bin" ]; then DIR=$HERE; else DIR=$(cd "$HERE/../output/images" && pwd); fi
JLINK=${JLINK:-$(command -v JLinkExe || echo /opt/SEGGER/JLink/JLinkExe)}
[ -x "$JLINK" ] || { echo "JLinkExe not found: install the SEGGER J-Link software"; exit 1; }

CHOICE=$1
if [ -z "$CHOICE" ]; then
  echo "Flash which image?  all  uboot  dtb  kernel  rootfs"
  read -r -p "> " CHOICE
fi
case $CHOICE in
  all)    IMAGES="u-boot.bin:0x20000000 r7s9210-rza2mevb.dtb.bin:0x200C0000 xipImage.bin:0x20200000 rootfs.cramfs.bin:0x20800000" ;;
  uboot)  IMAGES="u-boot.bin:0x20000000" ;;
  dtb)    IMAGES="r7s9210-rza2mevb.dtb.bin:0x200C0000" ;;
  kernel) IMAGES="xipImage.bin:0x20200000" ;;
  rootfs) IMAGES="rootfs.cramfs.bin:0x20800000" ;;
  *) echo "Choose one of: all uboot dtb kernel rootfs"; exit 1 ;;
esac

CMD=$(mktemp); trap 'rm -f "$CMD"' EXIT
echo "rx 100" > "$CMD"
n=0
for i in $IMAGES; do
  f=${i%%:*}; a=${i##*:}
  [ -f "$DIR/$f" ] || { echo "Missing $DIR/$f"; exit 1; }
  echo "$f -> $a"
  echo "loadbin $f,$a" >> "$CMD"
  echo "verifybin $f,$a" >> "$CMD"
  n=$((n + 1))
done
echo "exit" >> "$CMD"

echo
echo "Before flashing:"
echo "  SUB SW6 program mode: 1 OFF, 2 ON, 3 OFF, 4 ON, 5 ON, 6 ON"
echo "  J-Link on CN5, power on, stop in U-Boot (press a key in the terminal)"
read -r -p "Press Enter to flash (Ctrl+C to cancel) "

cd "$DIR"
LOG=$DIR/flash.log
START=$(date +%s)
"$JLINK" -device R7S921053VCBG -if JTAG -speed 15000 -jtagconf -1,-1 -autoconnect 1 -CommanderScript "$CMD" | tee "$LOG"
ok=$(grep -c 'Verify successful' "$LOG" || true)

echo
echo "Verified $ok of $n images in $(( $(date +%s) - START )) s (log: $LOG)"
if [ "$ok" -eq "$n" ]; then
  echo "DONE. Power off, set SUB SW6 run mode: 1 OFF, 2 OFF, 3 OFF, 4 ON, 5 ON, 6 ON, 7 ON, 8 OFF, 9 OFF, 10 OFF"
  echo "Power on: U-Boot boots Linux after 3 s (press a key to stop at the prompt)"
else
  echo "FAILED: check SW6, the J-Link cable and that the board is stopped in U-Boot, then run again"; exit 1
fi
