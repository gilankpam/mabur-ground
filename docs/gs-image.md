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

The bootloader only, keeping the rest of the eMMC (RK3566 boards only; the
Orange Pi's SPL sits elsewhere):

    dd if=/tmp/<board>_u-boot.bin of=/dev/mmcblk0 seek=64 status=progress

### Bootloader via Maskrom (RK3566)

Flash `<board>_emmc_bootloader.img` to the eMMC with RKDevTool or
rkdeveloptool; this erases the eMMC. See
[recovering a RunCam VRX from a bad flash](https://docs.openipc.org/hardware/runcam/vrx/recoverbadflash/).

## Upgrading

A flashed board updates itself from `<board>.tar.gz`:

- copy it to a FAT-formatted SD card, or to the `DVR` partition (SD or eMMC,
  by `scp` or gadget mode), and reboot; or
- `sysupgrade -u -r` online: it fetches the latest release's
  `<board>.tar.gz`. `sysupgrade --help` lists the rest.

A board flashed before this repo built the images has an upgrade URL that
points elsewhere: upgrade it once with
`sysupgrade --url=https://github.com/gilankpam/mabur-ground/releases/latest/download/<board>.tar.gz`,
or reflash.

The Orange Pi's package carries no `u-boot.bin`, and `sysupgrade` leaves the
bootloader alone when the package has none: the H618's SPL lives at
256 KiB, and the generic bootloader write (at 32 KiB, the Rockchip offset)
would make it unbootable.

## Configuration: the CONFIG partition

The settings live on a 64 MB FAT32 `CONFIG` partition, created on first
boot:

| File | What it configures |
|---|---|
| `wifi.toml` | onboard WiFi (`wlan0`): hotspot, joining an existing network, or off |
| `maburgs.toml` | `maburgs`: radio, link, stats outputs |
| `maburplay.toml` | `maburplay`: decoder, screen mode, DVR, OSD, buttons |

It is FAT32 with a Microsoft basic-data partition type, so the SD card can be
edited in any computer. On the board the same files are `/etc/maburgs.toml`
and `/etc/maburplay.toml` (symlinks into `/config`) and
`/config/wifi.toml`. Reboot, or restart `S96maburgs` / `S97maburplay` /
`S39wifi`, to apply.

`wifi.toml` runs a hotspot called `OpenIPC GS` by default (password
`12345678`, channel 6, the board at `10.18.0.1`). `mode = "client"` with
`[client]` filled in joins a router instead; if no address arrives within
`fallback_timeout` seconds the hotspot comes up anyway. Invalid values are
replaced by their defaults and logged to the console.

Deleting a file restores the board's default on the next boot; the defaults
are kept in `/usr/share/config-defaults/`.

A board that reached this image by `sysupgrade` from one without `CONFIG`
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
