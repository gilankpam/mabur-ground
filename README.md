# mabur-ground

The ground station of [mabur](https://github.com/gilankpam/mabur), an
RTP-free FPV video link. `maburgs` receives the drone's bodies on
RTL88xx USB cards through [devourer](https://github.com/gilankpam/devourer),
FEC-decodes them, drives the link (bitrate ladder, channel selection and
hops, NACK repair, TX-power calibration) and publishes whole access units
to a shm AU ring. `maburplay` (`gs/player/`) consumes the ring: MPP
hardware decode straight to DRM/KMS, the OSD, and the fMP4 DVR. `web/` is
the same receive + control core in the browser (WebUSB + WASM), as a
single-card GS or a passive spotter, served from this repo's GitHub Pages.

The drone (`maburd`), the wire formats and the FEC live in mabur, pinned
here as the submodule `third_party/mabur`; drone and GS are released as a
matched pair. How the link behaves (ladder, calibration, NACK, phase lock,
slices, pairing, deploy) is documented in mabur's `docs/`; the pages here
cover the GS itself.

## Host build + test

Clone with the mabur submodule, and devourer as a sibling checkout:

```sh
git clone https://github.com/gilankpam/mabur-ground.git
git -C mabur-ground submodule update --init third_party/mabur
git clone https://github.com/gilankpam/devourer.git
```

Requires `pkg-config` and `libusb1`; on NixOS wrap commands in
`nix-shell -p pkg-config libusb1 --run "..."`:

```sh
cmake -S . -B build -DDEVOURER_DIR=$PWD/../devourer
cmake --build build -j
ctest --test-dir build -R 'test_|gs_e2e|gs_au_e2e|player_e2e|web_au_parity'
```

A host build also builds `maburd` from the pinned mabur: the end-to-end
tests feed maburgs from `maburd --dry-run`. To build against a local mabur
checkout instead of the submodule (a change that spans both repos), add
`-DMABUR_DIR=$PWD/../mabur`.

## Cross build (aarch64 GS)

`tools/build-arm64.sh` builds `maburgs`, `maburplay`, `linkbench-rx`,
`nhm-reset-probe` and `encosd` into `out/arm64/`, including the glibc
builds for the Radxa and the Orange Pi Zero 2W. It honours `MABUR_DIR`.
Run it outside any `nix-shell` wrapper.

## MSP DisplayPort OSD (ground-side)

Set `msp.enable=true` on the drone (`mabur.toml`) and GS (`maburgs.toml`);
`msp.symbol_size`/`msp.window` must match on both ends. The drone taps the
flight controller's MSP DisplayPort UART (`msp.serial`, default `/dev/ttyS2`)
and forwards full-screen keyframe snapshots at `msp.update_rate_hz` (default
1 Hz) as their own FEC-protected air stream. `maburgs` reassembles the
snapshots and re-emits the MSP bytes as loopback UDP datagrams to `msp.out`
(default `127.0.0.1:14560`) — it renders no pixels itself. `maburplay`
receives that feed on `osd.port`, rasterizes the character grid with a
runtime glyph atlas and shows it on a DRM overlay plane above the decoded
video. Neither msposd nor PixelPilot is involved any more (both they and
the old `msp.render = "shm"` shared-memory path are gone). One-way display
only — see `docs/superpowers/specs/2026-07-15-msp-displayport-osd-design.md`
for the drone-side-burn-in and GS→FC-menu future doors.

Player-side config lives in `maburplay.toml` under `osd`: `enable`, `port`,
`font` (path to a `.mfont` atlas), `scale` (`sharp` | `fill`) and `stale_ms`
(blank the overlay after this much silence; keep it at several multiples of
the drone's `msp.update_rate_hz` period, default 5000 ms).

Two deploy-time invariants, neither of which a daemon can check for itself:

- **`osd.port` (maburplay) must equal `msp.out.port` (maburgs).** They are
  separate processes with separate config files. On a mismatch the OSD stays
  empty; maburplay logs a one-shot warning naming the bound port ~10 s after
  start.
- **The `.mfont` atlas named by `osd.font` must exist on the GS**, by
  default `/usr/local/share/mabur/font_btfl.mfont`. The generated Betaflight
  36x54 atlas is committed as `gs/player/bundle/font_btfl.mfont` and staged
  into `out/arm64/` by `tools/build-arm64.sh`; push it alongside the
  binaries. Regenerate it from an msposd checkout next to this repo with:

  ```
  tools/msp/gen_font.py ../msposd/fonts/original/betaflight/font.png \
      gs/player/bundle/font_btfl.mfont
  ```

  If the file is missing, maburplay logs `osd disabled -- cannot open ...`
  and runs video-only.

## GS link-status OSD (player-side)

A second, independent overlay drawn by maburplay from the maburgs stats
sideport: two rows of icon-tagged figures along the bottom edge (channel,
mcs, airtime, per-card RSSI and SNR and drone temperature above; bitrate,
resolution as `720p`/`1080p`, fps, jitter, latency and the pre/post-FEC
loss pair below),
plus the DVR recording clock alone in the top-right corner, so the centre
of frame stays clear. `docs/observability.md` has the details. It shares the MSP overlay's plane and
surface (MSP draws first, then the GS fields repaint over any collision, so
GS pixels always win), and it is burned into the DVR alongside the MSP grid.

Config lives in `maburplay.toml` under `osd.gs`: `enable` (default false),
`port` (the sideport fan-out destination maburgs sends to — see `stats.out`
in `maburgs.toml`), `font` (path to a `.gfont` atlas) and `stale_ms` (dim
every link-derived field after this much silence; the player-measured fields
ignore it, being current by construction).

- **The `.gfont` atlas named by `osd.gs.font` must exist on the GS**, by
  default `/usr/local/share/mabur/gs_osd.gfont`. It is committed as
  `gs/player/bundle/gs_osd.gfont` and staged into `out/arm64/` by
  `tools/build-arm64.sh`; push it alongside the binaries, exactly like the
  `.mfont`. Both fonts are mmapped by a running maburplay, so stop the
  player (`/etc/init.d/S97maburplay stop`) before overwriting either — an
  in-place `scp` over a live mapping is how you get a SIGBUS instead of a
  new font.
- The asset bakes **30 pixel sizes** (eight base sizes at ×⅔, ×1, ×4/3 and
  ×2); the bar takes the largest one whose worst-case rows fit the screen,
  with no runtime scaling anywhere. That is what makes it ~15 MB — a mask
  atlas, two bytes per pixel, at every size.
  Its characters are baked from JetBrains Mono Medium and the bar's item
  icons from JetBrains Mono **Nerd Font Mono** Medium (Material Design
  icons in the same cell). Regenerate it (both fonts are dev-host
  dependencies only; the asset is committed so a deploy needs no font
  toolchain) with:

  ```
  nix-shell -p python3Packages.freetype-py --run \
    "tools/msp/gen_gsfont.py \
       <jetbrains-mono>/share/fonts/truetype/JetBrainsMono-Medium.ttf \
       <nerd-fonts.jetbrains-mono>/share/fonts/truetype/NerdFonts/JetBrainsMono/JetBrainsMonoNerdFontMono-Medium.ttf \
       gs/player/bundle/gs_osd.gfont --sizes <the 30-value list>"
  ```

  The exact size list and expected byte count are in `gen_gsfont.py`'s
  header; `tests/test_gs_asset.cpp` is the gate that the committed asset
  still bakes exactly those sizes and that the layout holds against real
  JetBrains Mono metrics at all four resolutions.

**When the overlay is on screen.** It appears with the first decoded frame
and stays there for the rest of the session, including through video loss.
The CRTC-active latch it is gated on (`drm_presenter.cpp`, set once on the
first modeset and never cleared) is deliberately *not* the "video is
healthy" flag: an AU-ring discontinuity, a decoder wedge, the decode
watchdog dropping every queued frame and recreating the backend all re-arm a
modeset without deactivating the CRTC, and the OSD keeps committing on its
own throughout — which is exactly when on-screen link telemetry is worth
having. The one dead window is between GS power-on and the first decoded
frame, where the CRTC is not up and an OSD-only commit would have nothing to
display on.
