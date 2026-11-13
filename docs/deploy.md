# Deploying the GS

How a deploy works for the pair — flag days, the config-load crash loop,
the clean stop/edit/swap/start sequence, what both install scripts do with
an existing config, pairing, and the flag-day checklist — is mabur's
[`docs/deploy.md`](https://github.com/gilankpam/mabur/blob/master/docs/deploy.md). This page is the GS side:
what `gs/bundle/install.sh` does beyond that, where things live on each GS
board, and flashing the GS image.

## The install script

`gs/bundle/install.sh root@<gs>` builds (unless a prebuilt binary is
passed as the second argument), stops `S96maburgs`, copies the binary and
the init script, leaves an existing `/etc/maburgs.toml` alone, and starts
the service. It also keeps
`/usr/local/bin/maburgs.prev` and installs `tools/maburtop.py` and
`gs/bundle/maburcal` to `/usr/bin`. Without a binary argument it reads the
board's `/proc/device-tree/model` and ships that board's glibc build
(`out/arm64/maburgs` on a Radxa, `out/arm64/maburgs-opi` on an Orange Pi),
and it waits for maburgs to exit after `S96maburgs stop`: stop returns
before the daemon is gone, and copying over a running binary fails with
"Text file busy". A hand swap needs the same wait
(`while pidof maburgs; do sleep 1; done`). There is no installer for `maburplay`
(see "Device facts").

## Device facts

**GS.**

- `maburgs` and `maburplay` live in `/usr/local/bin`; `maburtop` and
  `maburcal` in `/usr/bin` (the GS shell's default PATH lacks
  `/usr/local/bin`); the OSD fonts and splash in `/usr/local/share/mabur/`
  (staged in `out/arm64/` by `tools/build-arm64.sh`). The remembered
  channel is `/etc/maburgs.channel`.
- Never load the 8812eu kernel module on the GS cards; `S96maburgs`
  unloads it on start.
- `maburgs` is glibc-DYNAMIC, built per board against that image's
  Buildroot sysroot (`tools/build-arm64.sh` stage 6 → `out/arm64/maburgs`,
  stage 7 → `out/arm64/maburgs-opi`). NEEDED: `libusb-1.0.so.0` plus
  libstdc++/libm/libgcc_s/libc and the loader, all on both images. The
  image repo's own package builds it the same way. `out/arm64/maburgs-static`
  is the musl-static build of the same daemon, a rollback that runs on any
  board but costs ~2 ms of GS latency per frame
  ([`docs/findings/2026-10-15-gs-musl-latency.md`](https://github.com/gilankpam/mabur/blob/master/docs/findings/2026-10-15-gs-musl-latency.md)).
- **`/etc/maburplay.toml` is a symlink to `/config/maburplay.toml`** (the
  vfat partition). Back up and edit the **target**, not the link: BusyBox
  `sed -i` on the symlink replaces it with a regular overlay file, after
  which the `/config` copy goes silently dead, and `cp -a` on the link
  backs up only the link. Because it is vfat, run `sync` after a hand
  edit before power-cycling the GS: the write can otherwise sit in the
  page cache and a power-cycle before it lands drops the edit (seen on
  the settings-menu's `[input.*]` tables below).
- **`[input.*]` tables are per-unit wiring, not part of the shipped
  config.** The DVR button's pin and the settings menu's five
  (`docs/gs-menu.md`) depend on how that particular unit's GPIO header is
  wired, so the bundle ships the record button's table live and the
  menu's five commented out at `pin = 0`. Copy the real pin numbers by
  hand into `/config/maburplay.toml` on each unit (never ship them in the
  repo bundle as if they were universal), then `sync`.
- **`gs_osd.gfont` must ship together with `maburplay`.** The settings
  menu draws with that same GS font (`osd.gs.font`, `docs/observability.md`
  "GS link-status OSD"), and it carries the menu's symbols
  (`docs/gs-menu.md`): an older copy of the font on the GS renders the
  menu's glyphs as blanks rather than failing the font load outright, so
  a mismatch is easy to miss. Deploy the binary, `gs_osd.gfont`
  (`/usr/local/share/mabur/`) and the bundle TOML together.
- **Bench: running `tools/bench/mavparam.py` from the host** needs
  `maburgs`'s `[mavlink] bind` set to the GS's LAN address, not its
  loopback default ([`docs/mavlink.md`](https://github.com/gilankpam/mabur/blob/master/docs/mavlink.md)). `maburgs`'s config is
  `/etc/maburgs.toml` on the overlay (only `maburplay.toml` is the vfat
  symlink): back up `/etc/maburgs.toml`, edit it, restart `S96maburgs`.
- There is no `maburplay` installer: copy `out/arm64/maburplay.default.toml`
  (staged by `tools/build-arm64.sh`) to the config by hand on a fresh GS.
  With no config, `maburplay -c /etc/maburplay.toml` exits 2 and
  `S97maburplay` does not respawn.
- `maburplay` is glibc-DYNAMIC (Mesa EGL/GLESv2/GBM + librga for the
  burned-DVR colortrans stage, `docs/colortrans.md`). NEEDED:
  `librockchip_mpp.so.1`, `libdrm.so.2`, `libEGL.so.1`, `libGLESv2.so.2`,
  `libgbm.so.1`, `librga.so.2`, plus libstdc++/libm/libgcc_s/libc and the
  loader. Check the GS image carries them before swapping, e.g.
  `ssh root@<gs> 'ls /usr/lib/libEGL.so.1 /usr/lib/libGLESv2.so.2 /usr/lib/libgbm.so.1 /usr/lib/librga.so.2'`
  — miss it and the binary will not exec at all.
  `out/arm64/maburplay-static` is the musl-static build of the same player
  without the GPU stage, for an image that lacks Mesa.

**Orange Pi Zero 2W GS** (`docs/opi-zero2w.md`). Same layout and init
scripts as above; the daemon is `out/arm64/maburgs-opi` (installed as
`/usr/local/bin/maburgs`, `gs/bundle/install.sh` picks it by model) and
the player binary is `out/arm64/maburplay-opi`
(installed as `/usr/local/bin/maburplay`), glibc-dynamic against the OPi
image's libav: NEEDED `libavcodec.so.61`, `libavutil`, `libdrm.so.2`,
libstdc++. Its config must say `backend = "v4l2req"`, `dvr.mode = "raw"`
— a Radxa `maburplay.toml` exits 2. Check the target first:
`ssh $GS cat /proc/device-tree/model` must read `OrangePi Zero 2W`.

    ssh $GS '/etc/init.d/S97maburplay stop'
    scp -O out/arm64/maburplay-opi $GS:/usr/local/bin/maburplay
    ssh $GS '/etc/init.d/S97maburplay start'

maburplay needs mabur's FFmpeg patch in the board's libavcodec, so put the
patched libavcodec on before (or together with) a new maburplay: a new
maburplay against an unpatched libavcodec fails `init()` and the wrapper
respawns it forever at 2 s. An image built from the image repo carries the
patch already; to put a rebuilt libavcodec on a board by hand, copy it
onto the overlay, where it shadows the image's:

    scp -O ../sbc-groundstations-gilankpam/output/orangepi_zero2w_defconfig/target/usr/lib/libavcodec.so.61.19.100 $GS:/usr/lib/

and check `find /overlay/root/usr/lib -name 'libavcodec*'` after a later
image flash (a stale copy there shadows the new image's).

**CPE510 relay** (`radio.relays`, `docs/cpe510-relay.md`). The Radxa ZERO 3
has no Ethernet: the CPE needs a USB-Ethernet adapter on the GS. The CPE
serves DHCP on its LAN, 10.83.11.100-199, with **no router and no DNS**
(mabur-openwrt `90-mabur-lan`), so `relays` entries are numeric
`"ipv4:port"`. After a CPE `sysupgrade -n` its SSH host key changes:
`ssh-keygen -R 10.83.11.1`.

## GS image: p1 squashfs swap

The GS boots U-Boot → `/boot/Image` + `/boot/extlinux/extlinux.conf`, read
out of the squashfs on p1 (`/dev/mmcblk1p1`, 264160 KiB = 270,499,840
bytes; U-Boot `CONFIG_CMD_SQUASHFS`). The kernel command line is the
`append` line of that extlinux.conf (image repo:
`board/common/overlay/boot/extlinux/extlinux.conf`), so a kernel parameter
(`rk_vcodec.rkvdec2_stream=0`: the rkvdec2 driver is built in) means a new
squashfs. Everything else the GS keeps — `/etc` configs,
`/usr/local/bin/maburgs`/`maburplay` — lives on the overlayfs partition
(upper dir `/overlay/root`) and survives a p1 swap. A library copied into
the upper dir's `usr/` shadows the image's: check
`find /overlay/root/usr -name 'librockchip_mpp*'` is empty after a swap.

Below, `$GS` is the ground station's ssh target (`root@<gs-ip>`).

Back up p1 first:
`ssh $GS 'dd if=/dev/mmcblk1p1 bs=1M' > p1.orig.img`
(270,499,840 bytes); rollback is the same sequence with that file. The
SD-card image stays the last resort if p1 does not boot.

Flash any image `IMG` (from the image repo's `output/`):

    IMG=<squashfs>; SZ=$(stat -c %s $IMG); MD5=$(md5sum < $IMG | cut -d' ' -f1)
    scp -O $IMG $GS:/tmp/p1.new.img
    ssh $GS 'md5sum /tmp/p1.new.img'                 # == $MD5
    ssh $GS '/etc/init.d/S97maburplay stop'
    ssh $GS '/etc/init.d/S96maburgs stop'
    ssh $GS 'pidof maburplay maburgs || echo none'   # none
    timeout 60 ssh $GS 'dd if=/tmp/p1.new.img of=/dev/mmcblk1p1 bs=1M conv=fsync && sync && echo written && echo 1 > /proc/sys/kernel/sysrq && echo s > /proc/sysrq-trigger && echo b > /proc/sysrq-trigger' || true
    # once it is back up (~25 s): the partition holds the image
    ssh $GS "head -c $SZ /dev/mmcblk1p1 | md5sum"   # == $MD5

Write and reboot in **one** ssh command, and reboot through sysrq only:
once p1 is rewritten, nothing on the old lower filesystem can be exec'd
except what is still in the page cache — BusyBox `reboot` cannot, and after
`echo 3 > /proc/sys/vm/drop_caches` neither can `head`/`md5sum` nor sshd
itself (every new ssh connection is reset). If that happens anyway,
power-cycle the GS: the `sync` has already landed the image. Verify the
write after the boot, as above; to check it before rebooting, the
read-back needs a static binary in `/tmp` and must run in the same ssh
command as the `dd`.

Verify after boot: `uname -v`, `cat /proc/cmdline`,
`cat /sys/module/rk_vcodec/parameters/rkvdec2_stream` (`Y` on the
stream-mode image), `dmesg | grep "link mode off"`.
