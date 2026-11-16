# RZ/A2M Smart Thermostat Demo (LVGL 9, XIP Linux 7.x)

A smart-thermostat GUI for the RZ/A2M EVK, built with LVGL 9.5 and the D/AVE 2D
GPU, running on XIP Linux from the
[rza_linux-7.x_bsp](https://github.com/renesas-rz/rza_linux-7.x_bsp).

| Item | Value |
|---|---|
| App size | 1.4 MB (runs in place from QSPI flash) |
| App RAM (RSS) | 212 kB |
| Root file system | 4.5 MB cramfs (XIP) |
| CPU load | ~5 % idle, ~10 % while touching |
| Kernel start to GUI | ~3.8 s |

## Folders

| Folder | Contents |
|---|---|
| `source/` | App source, LVGL 9.5, D/AVE 2D driver, `install.sh`, start script |
| `prebuilt/` | `u-boot.bin` and `rootfs.cramfs.bin` (app included) |

## Hardware

| Part | Setting |
|---|---|
| Board | RZ/A2M EVK, powered from USB-C |
| Display | 7" LCD, 800x480, or HDMI through the RGB-to-HDMI board (TFP410) |
| Touch | USB touch screen on CN12, with SW6-1 and SW6-3 OFF (USB host) |

Touch can be plugged in before or after power-on.

## Build

1. Build the BSP once (see its README), so its toolchain exists.
2. Build the app and add it to the BSP's rootfs overlay:

   ```sh
   cd source
   ./install.sh <path to rza_linux-7.x_bsp>
   ```

3. In the BSP, rebuild the rootfs:

   ```sh
   ./build.sh buildroot && ./build.sh images
   ```

`./install.sh <BSP> rza2mevb remove` takes the demo out of the overlay again.

## Flash

Program U-Boot, the kernel, the DTB and the rootfs as described in the BSP
README. The prebuilt images go to:

| Image | QSPI flash address |
|---|---|
| `prebuilt/u-boot.bin` | 0x20000000 |
| `prebuilt/rootfs.cramfs.bin` | 0x20800000 |

The kernel (0x20200000) and DTB (0x200C0000) come from the BSP build.

## Run

The demo starts at boot (`/etc/init.d/S99demo`). Options, set as environment
variables before starting `/root/bin/thermostat_a2m`:

| Variable | Values | Purpose |
|---|---|---|
| `THERMO_UI` | `elite` (default), `classic` | Choose the UI style |
| `OVEN_GPU` | `1` (default), `0` | Turn the D/AVE 2D GPU on or off |
| `A2M_MAX_FPS` | number, e.g. `30` | Limit the frame rate |

Restart the demo with `/etc/init.d/S99demo restart`.

Brightness: the slider sets the panel backlight through
`/sys/class/backlight` when present; otherwise the screen is dimmed in
software.
