# The GS image: flashing, upgrading, on the board

The image for the RunCam WiFiLink, the Emax Wyvern-Link, the Radxa ZERO 3
and the Orange Pi Zero 2W. Where the files come from and how they are built:
[`releases.md`](releases.md). Daemon layout and swapping single binaries:
[`deploy.md`](deploy.md).

## Flashing

### Easy Flash: eMMC from an SD card

Writes the whole image to eMMC by booting from an SD card; no drivers or
tools needed, and it works from older SBC-GS images.

1. Format an SD card FAT32.
2. Copy `<board>_sdcard.img` and `<board>_boot.scr` from a release to its
   root, and rename the second to `boot.scr`.
3. Insert it and power on. U-Boot runs `boot.scr`, writes the image to eMMC
   (a few minutes), removes both files from the card and reboots into the
   new system.

The board needs a working U-Boot on eMMC. If the eMMC is blank or U-Boot is
broken, flash the bootloader through Maskrom first (below).

### Boards without eMMC

Write `<board>_sdcard.img` to an SD card with any image writer
([etcher](https://etcher.balena.io/)).

### dd from a running system

The whole disk, from a system booted off SD (erases everything on eMMC):

    dd if=/tmp/<board>_sdcard.img of=/dev/mmcblk0 status=progress

The bootloader only, keeping the rest of the disk. RK3566 boards, at
32 KiB:

    dd if=/tmp/<board>_u-boot.bin of=/dev/mmcblk0 seek=64 status=progress

The Orange Pi boots from SD; its SPL + U-Boot sit at 256 KiB (the 8 KiB
slot is inside the GPT), so on the card in a reader:

    dd if=orangepi_zero2w_u-boot.bin of=/dev/<sd-card> bs=1k seek=256 conv=fsync

### Bootloader via Maskrom (RK3566)

Flash `<board>_emmc_bootloader.img` to the eMMC with RKDevTool or
rkdeveloptool; this erases the eMMC. See
[recovering a RunCam VRX from a bad flash](https://docs.openipc.org/hardware/runcam/vrx/recoverbadflash/).

## Upgrading

An upgrade installs a `<board>.tar.gz` package over the running system: the
rootfs on its partition and, on the RK3566 boards, U-Boot. The partition
table, the overlay, `CONFIG` and the recordings stay. It runs only when asked:
a package left on the SD card or the `DVR` partition does nothing by itself.

### From the menu

- **Firmware > Ground station > Update from file** lists this board's packages on the media
  (`/media/dvr`: the SD card if one is inserted, else the `DVR` partition),
  named `<board>.tar.gz` or `mabur-ground-<version>-<board>.tar.gz`. Copy one
  there by `scp`, gadget mode or a card reader.
- **Firmware > Ground station > Check online** asks GitHub for the latest release (Stable) or
  the `snapshot` pre-release built from master (Snapshot).

Either way the menu shows the package's version and changelog, asks before
installing, then shows the progress until the reboot. The rows and keys are
in [`gs-menu.md`](gs-menu.md).

Online needs the GS on a network (`wifi.toml` in client mode) and a synced
clock: GitHub's certificate is verified, and the boards have no
battery-backed clock. With no default route (hotspot mode, the default)
the check refuses with "No network — set wifi.toml to client mode" before
it looks at the clock. The image syncs the clock by NTP once it has a
network; until then, while the clock is still before the image's own
build time, the check refuses with "Clock not synced yet — try again in a
minute".

### Over ssh

    gs-upgrade install /media/dvr/<board>.tar.gz
    gs-upgrade install --online stable        # or snapshot

The same phases in the foreground, with progress on the terminal and a
question before services stop (`-y` skips it). This is the upgrade path on
a board whose default config wires no menu buttons.

### What happens

`gs-upgrade` downloads (online only), unpacks into `/tmp/gs-upgrade` (it
needs 2.5 times the package in free RAM), checks the md5 sums and that the
package's `VERSION`, when it has one, names this board, then stops maburgs,
cron and the syslogs (and maburplay from ssh; from the menu only its
respawn loop, so it keeps drawing), writes, and reboots. The package file is
kept. From the menu the write waits up to 5 s for maburplay to finish any
recording and lock itself in memory (its `ui-ready` mark), then goes ahead
either way; once started it does not depend on maburplay.

- A failure before the write leaves the GS running as it was. From the
  menu, ◀ cancels until services start to stop (download, unpack, verify).
- If maburgs fails to stop, the upgrade gives up and starts again what it
  stopped; from the menu, maburplay gets its respawn loop back. crond or a
  syslog daemon failing to stop (it was already gone) does not block it.
- A failure during the write reports "Write failed — reflash from SD card"
  and does not reboot: flash the image again (above).
- One run at a time: a second `start` or `install` answers "An update is
  already running". The lock (`/tmp/gs-upgrade/lock`) holds the run's pid;
  a run killed before it could drop the lock leaves a pid that is gone,
  and the next run takes the lock over.
- The reboot is sysrq's; should that be refused, `reboot -f` follows.

Files installed by hand on the overlay (`/overlay/root`, such as a branch
`maburplay` in `/usr/local/bin`) keep shadowing the new image afterwards.

`gs-upgrade` never writes a bootloader on the Orange Pi, whatever the
package carries (an old image repo package still has `u-boot.bin` in it):
the bootloader write is at 32 KiB, the Rockchip offset, which on the H618
runs over the SPL at 256 KiB and leaves the board unbootable. The Orange
Pi's own package carries no `u-boot.bin`; its bootloader is updated only by
hand, as above.

### From an older image

An image from before `gs-upgrade` has `sysupgrade` instead, which installs
the same package: leave `<board>.tar.gz` on a FAT SD card or the `DVR`
partition and reboot (those images flash it at boot), or run
`sysupgrade --archive=<file>`. A board flashed before this repo built the
images has an upgrade URL that points elsewhere: upgrade it once with
`sysupgrade --url=https://github.com/gilankpam/mabur-ground/releases/latest/download/<board>.tar.gz`,
or reflash.

## Configuration: the CONFIG partition

The settings live on a 64 MB FAT32 `CONFIG` partition, created on first
boot:

| File | What it configures |
|---|---|
| `wifi.toml` | onboard WiFi (`wlan0`): hotspot, joining an existing network, or off |
| `maburgs.toml` | `maburgs`: radio, link, stats outputs |
| `maburplay.toml` | `maburplay`: decoder, screen mode, DVR, OSD, buttons |

It is FAT32 with a Microsoft basic-data partition type, so the SD card can be
edited in any computer. On the board they are `/config/maburgs.toml`,
`/config/maburplay.toml` and `/config/wifi.toml`; `S96maburgs` and
`S97maburplay` start the daemons on the `/config` copy. Reboot, or restart `S96maburgs` /
`S97maburplay` / `S39wifi`, to apply.

The pairing key goes on the same partition: the image's default
`maburgs.toml` sets `key_file = "/config/mabur.key"`, so copy the drone's
key there as `mabur.key` (with no file, `maburgs` uses the built-in default
key). A `maburgs.toml` seeded by an older image keeps the `key_file` it
was written with.

`wifi.toml` runs a hotspot called `OpenIPC GS` by default (password
`12345678`, channel 6, the board at `10.18.0.1`). `mode = "client"` with
`[client]` filled in joins a router instead; if no address arrives within
`fallback_timeout` seconds the hotspot comes up anyway. Invalid values are
replaced by their defaults and logged to the console.

Deleting a file restores the board's default on the next boot; the defaults
are kept in `/usr/share/config-defaults/`.

A board upgraded in place to this image from one without `CONFIG`
keeps its old partition table, where `DVR` fills the disk. There is no room
for `CONFIG`, so the files live in `/config` on the overlay (edit them over
ssh), and the boot log says `config: no room for a CONFIG partition`. Flash
the full image to get the partition.

## Factory reset

Hold the factory-reset button (`Right` on the WiFiLink) during boot, or run
`firstboot`. This wipes the overlay **and** `CONFIG`: every setting goes back
to the board's default.

## Gadget mode

Hold the gadget button (`Left` on the WiFiLink) during boot. On the OTG port
the board then offers:

- the DVR (the SD card if one is inserted, else the `DVR` partition);
- the `CONFIG` partition, as a second drive (not on a board without one);
- a serial console;
- a network link, the board at `192.168.5.1`.

On the RunCam WiFiLink this is the port labelled `Type-C`; the one labelled
`OTG` is host-only. Gadget mode stops `maburgs` and `maburplay` so the PC can
write the drives safely: no video until you leave it. Eject the drives on
the PC, then reboot.

## Dual boot with older RadxaOS images

U-Boot names the DTB differently from older RadxaOS-based images (e.g.
Ruby). Copy the right one into the Ruby partition once:

    mkdir -p /mnt/ruby
    mount /dev/mmcblk1p3 /mnt/ruby
    cp /boot/rockchip/rk3566-radxa-zero-3w-aic8800ds2.dtb /mnt/ruby/usr/lib/linux-image-5.10.160-34-rk356x/rockchip/rk3566-radxa-zero-3w.dtb
    reboot
