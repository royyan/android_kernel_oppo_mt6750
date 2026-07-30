# OPPO A59s (`a59`) — Linux port notes

Running a minimal Linux (Alpine + X11 on fbdev) on the OPPO A59s, keeping the
vendor 3.10 kernel so the hardware keeps working.

**Mainline is not a route on this SoC.** Upstream `mt6755.dtsi` contains only
CPUs, timer, GIC, sysirq and two UARTs — no clocks, pinctrl, MMC, USB, display or
GPU. The only community effort
([mtk-mainline/mt6755](https://gitlab.com/mtk-mainline/mt6755/linux)) has been
dead since 2023-07 and targets the Nokia 3.1.

## Status

| Component | State |
|---|---|
| Alpine 3.23.5 aarch64 on `userdata` | **working**, boots unattended |
| Kernel 3.10.72 `a59_linux_defconfig` | **working** |
| Xorg + `xf86-video-fbdev` + twm + xterm | **working**, visible on panel |
| Touchscreen (evdev) | **working** |
| root SSH over USB (CDC-ECM) | **working**, key auth |
| Display refresh (`fbpan`) | **working** (required, see below) |
| Backlight | **working** |
| ALSA card + mixer | card present, PCM runs clean, **no audible output** |
| WiFi | **not working** — CONSYS cold power-on fails |
| Cellular, camera, GPU accel | not attempted / not possible |

Footprint: ~54 MB RAM used, 3.8 GB free of 3.87 GB. Android on the same device
had 1.8 GB free.

## Device facts

| | |
|---|---|
| SoC | MT6750 (Helio P10 family), 8× Cortex-A53, Mali-T860MP2 |
| MTK project | `oppo6750_15131` |
| Panel | `oppo_hx8394_truly_cpt_hd720_dsi_vdo`, 720×1280 DSI video |
| Framebuffer | `mtkfb`, 32bpp, `line_length` 2944 (**stride 736 px, not 720**), `smem_len` 11304960 = 3 frames |
| DRM/KMS | **none** — no `/sys/class/drm`, no `/dev/dri` |
| Audio | ALSA card 0 `mtsndcard`, 26 PCMs, 86 mixer controls; **NXP TFA9890** smart amp |
| WiFi | on-die CONSYS + `mt-wifi` driver, `cfg80211` present |
| Touch | `mtk-tpd` on `event2` |
| Backlight | `/sys/class/leds/lcd-backlight`, max 255 (**not** `/sys/class/backlight`) |
| UDC | `musb-hdrc` |
| userdata | `mmcblk0p32`, major:minor **259:0** (minors spill past p31) |

## Kernel

Source: `oppo6750_15131_defconfig` from a tree whose `Makefile` reports exactly
3.10.72 and which contains the matching panel driver — verified against the
running stock kernel.

`a59_linux_defconfig` diff from stock:

| Option | Stock | Here | Why |
|---|---|---|---|
| `ANDROID_PARANOID_NETWORK` | y | **n** | gates sockets on Android's `AID_INET` group; a normal distro gets no networking |
| `VT`, `VT_CONSOLE` | not set | **y** | no `/dev/tty0` otherwise |
| `FRAMEBUFFER_CONSOLE` | not set | **y** | console on the panel |
| `DEVTMPFS`, `DEVTMPFS_MOUNT` | not set | **y** | non-Android init expects `/dev` populated |
| `FHANDLE` | not set | **y** | cheap, some tooling wants it |
| `VGA_CONSOLE` | y | **n** | see below |
| `USB_G_ANDROID` | y | **n** | frees the UDC for `g_ether` |
| `USB_ETH` | not set | **y** | CDC-ECM (RNDIS/EEM off — macOS supports ECM) |

`VGA_CONSOLE` is the trap: enabling `VT` pulls in `vgacon`, which references
`screen_info`, a symbol that does not exist on arm64:

```
drivers/video/console/vgacon.c:1338: relocation truncated to fit:
  R_AARCH64_ADR_PREL_PG_HI21 against undefined symbol `screen_info'
```

`u_ether.c` also needs a one-line fix: `gether_connect()` does
`kzalloc(sizeof(struct rndis_packet_msg_type))` but never includes `rndis.h`.
It only compiled because `ether.c` includes `rndis.h` under
`CONFIG_USB_ETH_RNDIS` *before* including `u_ether.c`. With RNDIS off the type is
incomplete. Add `#include "rndis.h"`.

### Build

Host: aarch64 Linux. Toolchain: AOSP `aarch64-linux-android-4.9`, branch
`android10-dev` (it was deleted from `main`).

```bash
export ARCH=arm64 SUBARCH=arm64
export CROSS_COMPILE=/path/toolchain-4.9/bin/aarch64-linux-android-
make a59_linux_defconfig
make -j"$(nproc)" HOSTCFLAGS="-Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -fcommon" Image
```

Host-side gotchas, each of which cost real time:

1. **`-fcommon` is mandatory.** Host gcc ≥10 defaults to `-fno-common`, breaking
   the bundled `dtc`: `multiple definition of 'yylloc'`.
2. **`aarch64-linux-android-gcc` is a python2 wrapper.** Symlink it to the real
   ELF `aarch64-linux-android-gcc-4.9.x`.
3. **Never build on a case-insensitive filesystem.** The tree has colliding names
   (`net/netfilter/xt_HL.c` vs `xt_hl.c`); macOS silently clobbers one of each
   pair and `git status` shows phantom modifications.
4. **Never build over a network filesystem** — throughput collapsed to ~1 object
   per 90 s. A macOS-shared (virtiofs) path additionally hits
   `Too many open files` under `-j8` even with `ulimit -n` at 1048576, because
   the limit lives on the host side of the share. Build on local disk.

## Boot image

Plain Android format — **no MTK 512-byte headers** on kernel or ramdisk (many
MT6750 devices have them; this one does not). The kernel payload is
`gzip(Image)` with a **DTB appended after the gzip stream**:

```
kernel_addr  0x40080000    ramdisk_addr 0x45000000    tags_addr 0x44000000
page_size    2048          cmdline "bootopt=64S3,32N2,64N2"
stock kernel 7814447 = gzip 7753114 + FDT 61333 (0xd00dfeed)
```

Repack (hardware unchanged, so the stock DTB is reused):

```bash
gzip -n -9 -c arch/arm64/boot/Image > Image.gz
cat Image.gz stock.dtb > kernel.gz-dtb
# then rebuild reusing the stock header verbatim
```

The uncompressed kernel is ~19 MiB against a 16 MiB `boot`, so gzip is not
optional. Result is ~8.5 MiB.

Appended cmdline: `selinux=0 loglevel=8 console=tty0`. The `console=tty0` matters
— LK passes `console=tty0 console=ttyMT0,921600n1`, and the **last** `console=`
wins as `/dev/console`. Without it, `/dev/console` is the UART and all userspace
output is invisible.

## eMMC write protection

**`boot` cannot be written from Alpine or Android** — MTK's kernel guards the
bootloader/boot partitions by name:

```
mmcblk0: error -30 transferring data, sector 457692   (-EROFS)
```

`dd` reports success and a plausible byte count at ~243 KB/s while writing
**nothing**. Always verify by decoding the header, never by trusting `dd`.
Writes to `reserve4` and data partitions work fine. Flash `boot` from recovery,
where it runs at ~70 MB/s.

## Alpine userspace

### Why Alpine/OpenRC and not Debian/systemd

The vendor kernel exposes only two cgroup controllers (`cpu`, `cpuacct`) — no
`memory`, `devices`, `freezer`, `pids`, no unified hierarchy. systemd will not
boot without further kernel work. (OpenRC is installed but unused; busybox init
is fewer moving parts during bring-up.)

### initramfs

`switch_root NEWROOT` does `mount --move NEWROOT /`, and **only a mount point can
be moved**. `/mnt/alpine` is an ordinary subdirectory of the ext4 at `/mnt`, so
the move failed, `switch_root` exited, and because it was `exec`ed PID 1 died —
kernel sat on the Tux logo with no message. Fix: bind-mount it first.

```sh
mount --bind /mnt/alpine /newroot
mount --move /dev /newroot/dev   # and /proc, /sys
exec switch_root /newroot /sbin/init
```

Also: `CONFIG_DEVTMPFS_MOUNT` does **not** apply to an initramfs root — the
kernel only auto-mounts devtmpfs for a real root. The initramfs must mount its
own `/dev`. And never `exec >/dev/tty0` unguarded in PID 1; if `/dev` is not
populated the redirect fails and the shell exits, killing init.

### Display — `fbpan` is mandatory

`mtkfb` only scans out on `FBIOPAN_DISPLAY`. Plain writes into the mmap'd
framebuffer never reach the LCD, so fbcon and `xf86-video-fbdev` both render into
a buffer nobody presents. A ~40-line daemon fixes it:

- re-read `FBIOGET_VSCREENINFO` each iteration and pan to the **current**
  `yoffset`. Pinning it to 0 fights fbcon (which scrolls by panning) and shows a
  blank region — the screen just goes black.
- `FBIO_WAITFORVSYNC` is unsupported (`EINVAL`). `FBIOBLANK`, `FBIOPAN_DISPLAY`
  and `FBIOPUT_VSCREENINFO` all work.
- `FBIOPUT_VSCREENINFO` with `FB_ACTIVATE_FORCE` clears the buffer.

Backlight comes up at **0** — a lit-but-black panel reads as a dead phone.
Nothing in Alpine sets it; Android's lights HAL did.

### Xorg

fbdev only; no GL (`swrast_dri.so` absent, no Mesa installed). Two chained traps:

1. **A missing keyboard driver silently kills the touchscreen.** X asks for the
   `kbd` driver for its implicit `<default keyboard>`; Alpine dropped
   `xf86-input-keyboard` and `libinput` was not installed. When the core keyboard
   fails to get a driver, X **aborts the rest of input setup**, so the touch
   device is parsed and never opened. Fix: declare the core keyboard explicitly
   as `evdev` on the hardware keys (`event1`).
2. `Option "Name"` in an evdev `InputDevice` section is a **match filter** and
   silently rejects the device. Drop it; keep only `Option "Device"`.

Drop the explicit `Modes "720x1280"` line — it does not match and X logs
`mode "720x1280" not found` before falling back to the native mode.

Fonts: only `font-misc-misc`/75dpi/100dpi are installed, so `xterm -fa`
(fontconfig, scalable) fails with "cannot match normal font". Use `-fn fixed`.

### USB networking

`g_ether` CDC-ECM. **MTK's `musb-hdrc` never asserts the UDC pullup and does no
cable-insert detection**, so the gadget is configured but the host never
enumerates it — replugging does not help. Force it:

```sh
echo connect > /sys/class/udc/*/soft_connect
```

IPv4 is unreliable: the phone answers ARP it initiates but not ARP the host
initiates, so the host's neighbour entry goes stale. **IPv6 link-local works
consistently** — prefer it. The gadget MAC is random per boot, so the link-local
address changes; discover with `ping6 ff02::1%<iface>`. Pinning
`g_ether.dev_addr=`/`host_addr=` on the cmdline would make both stable.

Note dropbear has no sftp-server, so `scp` fails — pipe instead:
`ssh host 'cat > /path' < file`.

## Running Android binaries on Alpine

Android's bionic binaries run **natively** — same kernel, no libhybris, no
emulation. They only need their interpreter and libs at the baked-in paths.
For `wmt_loader` + `6620_launcher` the closure is 6 libs, 1.3 MB:

```
/system/bin/linker64
/system/lib64/{libc,libdl,libcutils,liblog,libm,libstdc++}.so
```

`wmt_loader` then correctly creates `/dev/stpwmt`, `/dev/wmtWifi`, `/dev/stpbt`,
`/dev/stpgps`. This technique should also serve the audio HAL.

## Audio — TFA9890 is the blocker

ALSA is fully alive: card 0 `mtsndcard`, 26 PCMs, 30 codec drivers bound, 86
mixer controls, playback opens and runs with no xruns. **No audible output.**

- PCM constraint: buffer maxes at 12288 frames, so `speaker-test` defaults fail.
  Use `--period-size=3072 --buffer-size=12288`.
- **Gain enums are listed loudest-first**: `Lineout_PGAL_GAIN` item 0 = `8Db`,
  item 19 = `-40Db`. Setting "max index" mutes it. This wasted a test round.
- Every output switch accepts and holds `1`
  (`Speaker_Amp_Switch`, `Audio_Amp_L/R_Switch`, `Ext_Speaker_Amp_Switch`,
  `Receiver_Speaker_Switch`, `Headset_Speaker_Amp_Switch`, `Audio_HP_Switch`,
  `Ext_HP_Switch`) — still silent at full-scale square wave, +8 dB.

Root cause: **NXP TFA9890 DSP smart amp**, muted until a firmware container is
pushed through its misc device by `audio.primary.mt6750.so`:

```
/dev/i2c0_tfa9890                    char 10:43, group audio
/sys/bus/i2c/drivers/i2c_TFA98XX
/sys/bus/platform/drivers/TFA98XX_DRV
/system/etc/smartpa_params/NHQ_0_0_TFA9890_1111.eq / .preset / _48000Hz.*
```

**Untried and cheap: headphones.** The headphone and receiver paths go through
the internal codec and bypass the TFA entirely.

## WiFi — CONSYS cold power-on fails

```
[WMT-DETECT][E] wmt_detect_chip_pwr_on: either PMU(-1) or RST(-1) or WIFI_EINT(-1) is not set
[WMT-CONSYS-HW][W] consys_co_clock_type: pmic_register_val=0x2380, co_clock_type=0, TCXO mode
vcn18: operation not allowed
[WMT-CONSYS-HW][E] Read CONSYS chipId(0x00000000)
[WMT-CORE][E] wmt_core_stp_init: no hif info!
[WMT-CORE][E] opfunc_pwr_on: wmt_core_stp_init fail (-1)
```

Note `hwPowerOn()` returns `true` even when `regulator_enable()` fails, so the
WMT stack proceeds believing power came up — the first symptom is a misleading
`-ENOENT`.

**Android comparison (same kernel, WiFi working):** writing 1 to `/dev/wmtWifi`
logs only `WMT turn on WIFI success!` — **no CONSYS power-on at all**. CONSYS is
brought up once early in boot and never released; stopping/restarting
`conn_launcher` does not release it. So Android never exercises the cold path
that fails for us, and its working cold sequence cannot be captured post-boot.

Android's WiFi userspace, from `init.project.rc` / `init.mt6755.rc`:

```
service wmtLoader     /system/bin/wmt_loader                               user root, oneshot
service conn_launcher /system/bin/6620_launcher -p /system/etc/firmware/   user system
mknod /dev/wmtWifi c 153 0
on property:wlan.driver.status=ok  -> write /dev/wmtWifi "1"
```

Our invocation is byte-identical to this.

**Ruled out:** kernel config (10-line USB-only diff); boot timing (fails
identically at 8.8 s from a sysinit run); firmware paths (both `/etc/firmware`
and `/system/etc/firmware` present); clock buffer (`is_pmic_clkbuf == false`, the
RF/BSI buffer is on — "PMIC clock buffer state (off)" merely means that path is
unused on this device, and `echo pmic ...` correctly returns `-EINVAL`).

**Open leads, in priority order:**
1. `either PMU(-1) or RST(-1) or WIFI_EINT(-1) is not set` at 4.2 s — GPIO/EINT
   resources unresolved. Earliest and most concrete error.
2. `/data/nvram/APCFG/APRDEB/WIFI` (514 B, MAC + RF calibration) — exists on
   userdata but Alpine never mounts `/data`. Bind it in and retest.
3. `nvram_agent_binder` is not running.
4. Android boots the modem (`ccci`/`md1`, `MD1_PWR_CON=0x112`); check whether it
   shares a rail or resource CONSYS needs.

## Partition notes

Non-A/B GPT, 33 partitions. `recovery` = `mmcblk0p1` @ `0x8000`,
`boot` = `mmcblk0p24` @ `0xd600000`, both `0x1000000`. Preloader lives in the
eMMC boot region (`mmcblk0boot0`), outside the GPT.

**Never write** `nvram` `nvdata` `proinfo` `protect1` `protect2` — per-unit RF
calibration and IMEI, unrecoverable. The port needs none of them.

Keep `recovery` intact: it is the escape hatch for every bad `boot`, and the only
place `boot` can be written.

```sh
adb shell "busybox dd if=/tmp/boot.img of=/dev/block/mmcblk0p24 bs=1048576"
```

Android's toolbox `dd` rejects `bs=1M` ("illegal number") — use busybox.
`adb exec-out "su -c '...'"` returns 0 bytes under SuperSU; stream from recovery
(already root), or stage to a file and `adb pull`.

## Debugging without a console

There is no serial console (would need teardown), the panel is unreadable until
`fbpan` runs, and Alpine has no adb. Two channels that work:

- **Raw partition log.** Write progress to `reserve4` (`mmcblk0p6`) at distinct
  offsets (initramfs 0, alpine-init 32 KiB, `a59-net` 64 KiB, `a59-wifi` 96 KiB)
  and read it from recovery. This is what actually located the `switch_root` bug.
- **Live `/proc/kmsg` stream** (needs root). `dmesg` after the fact is useless
  here: MTK logs every fork/exit at `KERN_DEBUG` and the ring buffer self-evicts
  in seconds — it only spans ~20–60 s, `pstore` is empty and `last_kmsg` is 0
  bytes. Stream it while triggering the event instead.

An `input_event` is **24 bytes** on aarch64; `dd bs=16` on `/dev/input/event*`
returns nothing and looks like dead hardware.
