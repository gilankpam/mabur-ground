# Releases, snapshots and test builds

The GS images are built in this repo (`firmware/`) by
`.github/workflows/firmware.yml`, for four boards: `radxa_zero3`,
`runcam_wifilink`, `emax_wyvern-link` (all RK3566) and `orangepi_zero2w`
(Allwinner H618). One mabur-ground commit gives all four images; each
image's `maburgs` and `maburplay` are built from that commit, against the
mabur commit it pins at `third_party/mabur`. Flashing and upgrading a board:
[`gs-image.md`](gs-image.md).

## Three levels

| | built on | lands in | who can download | kept |
|---|---|---|---|---|
| **Snapshot** | a push to master touching `gs/`, `firmware/`, the mabur pin, `tools/release.py`, `tools/maburtop.py` or the workflow | the `snapshot` pre-release, replaced each time | anyone | until the next build |
| **Release** | a pushed `v*` tag | a permanent GitHub release, marked Latest | anyone | forever |
| **Test build** | the `build-images` label put on a PR (or a manual run, `gh workflow run firmware.yml --ref <branch>`) | the run's artifacts, listed by one comment on the PR (edited per run) | anyone logged in to GitHub | 14 days |

PRs do not build images by themselves: four boards are hours of Buildroot.
Label a PR `build-images` when it needs images on the bench; each time the
label is put on is one build of the PR's head (remove it and put it back to
build again). The label works on a PR that adds or changes the workflow
itself; a manual run only works once the workflow is on master. A newer run cancels
the older one of the same branch; a tag build is never cancelled.

Files, where `<version>` is the tag, `snapshot-<sha8>` or `pr<N>-<sha8>`:

- `mabur-ground-<version>-<board>_sdcard.img`: the whole disk.
- `mabur-ground-<version>-<board>_boot.scr`: the SD-card flasher script.
- `mabur-ground-<version>-<board>_u-boot.bin`: the bootloader.
- `mabur-ground-<version>-<board>_emmc_bootloader.img`: the bootloader as an
  eMMC image, for Maskrom flashing (RK3566 boards only).
- `mabur-ground-<version>-<board>.tar.gz`: the `sysupgrade` package (rootfs,
  plus U-Boot on the RK3566 boards).

A release also carries every file under its unversioned name
(`<board>.tar.gz`, `<board>_sdcard.img`, …): an image's `sysupgrade` URL is
`https://github.com/gilankpam/mabur-ground/releases/latest/download/<board>.tar.gz`.
Snapshots are pre-releases and never Latest, so an online upgrade only ever
takes a release.

## Version rule

GS releases are `v0.MINOR.PATCH`. **MINOR is the drone release minor the GS
pairs with**: GS `v0.M.x` works with drone `v0.M.x` (mabur's
[`docs/releases.md`](https://github.com/gilankpam/mabur/blob/master/docs/releases.md)
moves MINOR when the drone↔GS contract changes: `RC_VERSION`, a `CAP_*` bit,
the drone parameter table, a removed drone config key).

`tools/release.py check <tag>` refuses a tag unless:

- the pin is on mabur master (the pin rule, below);
- mabur has a `v0.MINOR.*` release, and the contract at the pin equals the
  contract of the newest one (read by mabur's own `tools/release.py`);
- the tag is above every earlier GS tag.

So a contract change releases mabur `v0.(M+1).0` first, then GS
`v0.(M+1).0`. A mabur patch release needs no GS release. A GS-only change is
the next GS patch. The check compares contracts, not commits: the pin need
not be the tagged mabur commit. A removed drone config key leaves no
constant for the check to see; whoever tags reads mabur's PRs since its last
tag. A published release is never re-tagged or deleted; a mistake is fixed
by the next number.

## Pin rule

`third_party/mabur` is a git submodule: one mabur commit. A change that
needs both repos goes:

1. On the dev box, both branches side by side; the host build takes
   `-DMABUR_DIR=$PWD/../mabur`, the image build `MABUR_DIR=../mabur`.
2. The mabur-ground PR pins the mabur PR branch's head, so its CI and test
   build use it; mabur's `ground-downstream` job is red meanwhile.
3. mabur merges first; the mabur-ground PR moves the pin to the merge commit
   on mabur master, then merges.

A PR may pin a mabur branch commit. Master and `v*` tags may not:
`tools/release.py pin-check` fails a master build whose pin is not on mabur
master, and `check` fails such a tag.

## Cutting a release

```sh
git tag v0.4.0 <master-sha>
python3 tools/release.py check v0.4.0        # the same check the workflow runs
git push origin v0.4.0
```

`check` needs mabur's history in the submodule:
`git -C third_party/mabur fetch --tags --force origin '+refs/heads/master:refs/remotes/origin/master'`.
If the check fails on GitHub: `git push origin :refs/tags/v0.4.0`, delete
the local tag, and fix what it names (usually: tag mabur first).

The notes come from `tools/release.py notes`: the drone release the pin's
contract pairs with, the mabur-ground and mabur commits, per board the
devourer commit, Buildroot version, kernel source and its sha256, and each
file's md5.

## What goes into an image

- **Buildroot** 2025.08.1, downloaded by `firmware/build.sh`.
- **maburgs, maburplay, maburtop, maburcal** and their bundled configs and
  init scripts from this tree (`firmware/package/mabur`, Buildroot `local`
  site), against **mabur** at the pin (or `MABUR_DIR`).
- **devourer** master, resolved once per workflow run.
- **Kernels**: `radxa/kernel` at the commit the RK3566 base config pins; on
  the Orange Pi an Armbian-patched 6.18.35 source tarball hosted on this
  repo's `opi-zero2w-kernel-6.18.35` pre-release
  (`firmware/scripts/prepare-opi-artifacts.sh` regenerates it).
- **Rockchip MPP** at the commit `tools/build-arm64.sh` pins (`MPP_REF`),
  with `gs/player/mpp-patches/`; **FFmpeg** (H618) from jernejsk's
  v4l2-request branch with `gs/player/ffmpeg-patches/`. One copy of each
  patch serves both builds; `firmware/tests/mpp-ref.test.sh` keeps the two
  MPP refs equal.
- Board files under `firmware/board/`; per-board defaults edited onto the
  shipped `maburplay.toml` by `firmware/board/common/post-build-script.sh`.

The three RK3566 boards share `firmware/configs/rk3566_common.fragment`;
each `<board>.fragment` sets only its overlay list and its factory-reset /
gadget pins. The H618 has its own `orangepi_zero2w_defconfig`.

## Building locally

```sh
firmware/build.sh radxa_zero3                                      # on a FHS host
echo 'firmware/build.sh radxa_zero3' | nix-shell firmware/shell.nix   # NixOS
MABUR_DIR=$PWD/../mabur firmware/build.sh radxa_zero3              # against a sibling mabur
firmware/build.sh radxa_zero3 menuconfig                           # any Buildroot target
```

On NixOS `nix-shell --run` does nothing under this `buildFHSEnv` and the
wrapper's exit status is always 0: pipe the command in, and log `$?` from
inside it. The output is `firmware/output/<board>/`, the images in its
`images/` with `build-info.txt` naming what went in. Every full build
re-syncs `package/mabur` from the tree (`mabur-dirclean`), so a rebuild
always carries the current code; check `images/` mtimes and the target's
`/etc/os-release` `VERSION` before handing an image over. The board's
Buildroot `host/` is the SDK `tools/build-arm64.sh` stages 6 and 7 build
against.
