# OPPO A59s (`a59`) — mainline-less Linux port notes

Goal: run a minimal Linux (Alpine + X11 on fbdev) on the OPPO A59s, keeping the
vendor 3.10 kernel so display, WiFi and audio keep working.

**Mainline is not a route on this SoC.** Upstream `mt6755.dtsi` contains only CPUs,
timer, GIC, sysirq and two UARTs — no clocks, pinctrl, MMC, USB, display or GPU.
The only community effort ([mtk-mainline/mt6755](https://gitlab.com/mtk-mainline/mt6755/linux))
has been dead since 2023-07 and targets the Nokia 3.1. So: downstream kernel,
Linux userspace.

## Device facts

| | |
|---|---|
| SoC | MT6750 (Helio P10 family), 8× Cortex-A53, Mali-T860MP2 |
| RAM / eMMC | 3.7 GiB usable / 32 GB |
| MTK project | `oppo6750_15131` (from `ro.build.description`) |
| Panel | `oppo_hx8394_truly_cpt_hd720_dsi_vdo`, 720×1280 DSI video |
| Framebuffer | `mtkfb`, 32bpp, `virtual_size=736,3840` — **stride is 736, not 720** |
| DRM/KMS | **none** — `/sys/class/drm` and `/dev/dri` absent |
| Audio | ALSA card 0 `mtsndcard` (`mt-snd-card`), ~26 PCMs |
| WiFi | `wlan0` on platform driver `mt-wifi`, `cfg80211` present |
| Touch | `mtk-tpd` on `event2` (plain evdev) |
| Backlight | `/sys/class/leds/lcd-backlight`, max 255 (**not** `/sys/class/backlight`) |
| Battery | `/sys/class/power_supply/{ac,battery,usb}` |

Consequences: no Wayland (wlroots/Weston need DRM), no Panfrost, no GPU
acceleration. X11 with `xf86-video-fbdev`, software rendered.

## Why Alpine/OpenRC and not Debian/systemd

The vendor kernel exposes only two cgroup controllers:

```
#subsys_name  hierarchy  num_cgroups  enabled
cpu           2          2            1
cpuacct       1          112          1
```

No `memory`, `devices`, `freezer`, `pids`, no unified hierarchy. systemd will not
boot on this without further kernel work. OpenRC does not care.

## Kernel config changes (`a59_linux_defconfig`)

Derived from `oppo6750_15131_defconfig`. Diff that matters:

| Option | Stock | Here | Why |
|---|---|---|---|
| `ANDROID_PARANOID_NETWORK` | `y` | **n** | restricts sockets to Android's `AID_INET` group; leaves normal distro users with no networking at all |
| `VT`, `VT_CONSOLE` | not set | **y** | no `/dev/tty0` otherwise — no Linux console |
| `FRAMEBUFFER_CONSOLE` | not set | **y** | console visible on the panel |
| `DEVTMPFS`, `DEVTMPFS_MOUNT` | not set | **y** | non-Android init expects `/dev` populated |
| `FHANDLE` | not set | **y** | cheap, some tooling wants it |
| `VGA_CONSOLE` | `y` | **n** | see below |

`VGA_CONSOLE` is the trap. Enabling `VT` pulls in `vgacon`, which references
`screen_info` — a symbol that does not exist on arm64. The link fails with:

```
drivers/video/console/vgacon.c:1338: relocation truncated to fit:
  R_AARCH64_ADR_PREL_PG_HI21 against undefined symbol `screen_info'
```

Disable `VGA_CONSOLE`, keep `DUMMY_CONSOLE` + `FRAMEBUFFER_CONSOLE`.

Verified on device after flashing: `/dev/tty0` and `/dev/tty1` now exist,
`/sys/class/graphics/fbcon` appears, and `mtkfb`/`wlan0`/`mtsndcard` all survive.
`devtmpfs` won't show in `mount` under Android because init covers `/dev` with
tmpfs; it is compiled in and available to a non-Android init.

## Building

Host: aarch64 Linux. Toolchain: AOSP `aarch64-linux-android-4.9`
(branch `android10-dev`; the toolchain was deleted from `main`).

```bash
export ARCH=arm64 SUBARCH=arm64
export CROSS_COMPILE=/path/toolchain-4.9/bin/aarch64-linux-android-
make a59_linux_defconfig
make -j"$(nproc)" HOSTCFLAGS="-Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -fcommon" Image
```

Three host-side gotchas, all cost real time:

1. **`-fcommon` is mandatory.** Host gcc ≥10 defaults to `-fno-common`, which
   breaks this tree's bundled `dtc`:
   `multiple definition of 'yylloc'` (dtc-parser.tab.o vs dtc-lexer.lex.o).
2. **`aarch64-linux-android-gcc` is a python2 wrapper.** On a modern distro
   there is no `/usr/bin/python`. Symlink it to the real ELF
   `aarch64-linux-android-gcc-4.9.x`.
3. **Do not build on a case-insensitive filesystem.** The tree has colliding
   names (`net/netfilter/xt_HL.c` vs `xt_hl.c`); macOS silently clobbers one of
   each pair and `git status` shows phantom modifications.

Also: building over a network filesystem is not viable — throughput collapsed to
~1 object per 90 s. A macOS-shared (virtiofs) path additionally hits
`Too many open files` under `-j8` even with `ulimit -n` at 1048576, because the
limit lives on the host side of the share, not in the shell. Build on local disk.

## Boot image

The A59s boot image is **plain Android format — no MTK 512-byte headers** on
kernel or ramdisk (many MT6750 devices do have them; this one does not). The
kernel payload is `gzip(Image)` with a **DTB appended after the gzip stream**:

```
kernel_size    7814447        gzip stream 7753114 + FDT 61333 (0xd00dfeed)
kernel_addr    0x40080000
ramdisk_addr   0x45000000
tags_addr      0x44000000
page_size      2048
cmdline        "bootopt=64S3,32N2,64N2"
```

Repack recipe (hardware description unchanged, so the stock DTB is reused):

```bash
gzip -n -9 -c arch/arm64/boot/Image > Image.gz
cat Image.gz stock.dtb > kernel.gz-dtb
# then rebuild the boot image reusing the stock header verbatim
```

Result is ~9.5 MiB against a 16 MiB `boot` partition. Note the uncompressed
kernel is ~19 MiB, so it **must** be gzipped — raw would not fit.

## Partition notes

Non-A/B GPT, 33 partitions. `recovery` is `mmcblk0p1` at `0x8000`, `boot` is
`mmcblk0p24` at `0xd600000`, both `0x1000000`. Preloader lives in the eMMC boot
region (`mmcblk0boot0`), outside the GPT.

**Never write** `nvram` `nvdata` `proinfo` `protect1` `protect2` — per-unit RF
calibration and IMEI, unrecoverable. The port needs none of them.

Keep `recovery` intact: it is the escape hatch for every bad `boot`. Restore with

```bash
adb shell "busybox dd if=/tmp/boot.img of=/dev/block/mmcblk0p24 bs=1048576"
```

Note Android's toolbox `dd` rejects `bs=1M` ("illegal number") — use busybox, or
a byte count. And `adb exec-out "su -c '...'"` returns 0 bytes under SuperSU;
stream from recovery (already root) or stage to a file and `adb pull`.

## Status

- [x] Full verified backup (31 partitions, sha256 host-vs-block-device)
- [x] Matching kernel source identified and built
- [x] Kernel patched for Linux userspace, boots, hardware intact
- [ ] Alpine rootfs on `userdata` + initramfs handoff
- [ ] X11 on fbdev (mind the 736 stride), touch via evdev
- [ ] WiFi — needs `6620_launcher`/`wmt_loader` to power the combo chip; these
      are Android/bionic binaries, the main unsolved risk
- [ ] Audio — ALSA card works, needs the mixer routing from
      `/system/etc/audio_device_15131.xml`

Never expected to work: cellular, camera, GPU/video acceleration. Standby battery
life will be poor — MTK deep-idle is driven by Android's power HAL plus the
`pcm_deepidle_*`/`pcm_suspend_*` firmware blobs.
