# CLAUDE.md

Guidance for Claude Code when working in this repository.

**Never create git worktrees** (no `.claude/worktrees/*`, no
`git worktree add`), even when a skill or plan-execution flow suggests one.
Branch in the main checkout instead. Split checkouts drift here — a PR
merges on GitHub while the main checkout sits behind, and merged work looks
missing.

**Code review:** do NOT run Claude Code's built-in `/review` /
`code-review` skill (nor `/code-review ultra`) on mabur work — too
expensive for diffs that are gated on tests plus an inline read. This
does NOT cover the superpowers workflows: `subagent-driven-development`'s
per-task implementer + spec/quality reviewer subagents are wanted and
should be used when a plan is executed that way.

mabur-ground is the ground station of mabur, an RTP-free FPV video link.
`maburgs` (aarch64) receives, FEC-decodes, drives the link (ladder,
channel, NACK, calibration) and publishes whole access units to a shm AU
ring; `maburplay` (`gs/player/`) consumes the ring — MPP hardware decode
straight to DRM/KMS, plus the fMP4 DVR on /media/dvr. `web/` is the same
receive + control core in the browser (WebUSB + WASM): a single-card GS or
a passive spotter.

The drone (`maburd`), the shared wire formats and FEC (`common/`) live in
**mabur** (`gilankpam/mabur`), pinned here as the submodule
`third_party/mabur`. CMake reads it through `MABUR_DIR`; for a change that
spans both repos point it at the sibling checkout:
`-DMABUR_DIR=$PWD/../mabur`. A wire change is a mabur PR first; the
mabur-ground PR that follows it bumps the submodule, and each links the
other. mabur's CI builds this repo's master against every mabur PR.

## Where to look — read the doc, don't guess

This file is deliberately short. The detail lives in `docs/` here and in
mabur's `docs/` (`../mabur/docs/` on the dev box); load the one page the
task needs rather than carrying all of it.

Doc rules:

- **`docs/*.md` are reference pages: they describe the code at HEAD.** A
  change that alters behaviour, a default, a config key, a wire field or a
  log format fixes the page in the same commit. Present tense; no bench
  narratives, dated asides or SUPERSEDED chains — those go to findings.
- **The link-behaviour pages live in mabur** even where the code is here
  (ladder, calibration session, NACK tracker, phase lock, slice salvage):
  they are what any GS implementation reads. A PR here that changes
  behaviour one of them describes comes with a mabur docs PR, and each
  links the other.
- **`docs/findings/YYYY-MM-DD-topic.md` are dated records** (spikes,
  benches, flights, handovers). Everything recorded before the split is in
  mabur's `docs/findings/` (indexed in its `README.md`); new GS records go
  in this repo's `docs/findings/`. They are snapshots and are never edited
  to match later code; each carries a one-line `**Status:**` pointing at
  the reference page that owns the topic now.
- **Code comments say what the code does and why — no dates, no
  spec/plan citations.** Never cite `docs/superpowers/` specs or plans,
  `.superpowers/` reports, "spec section N", "Task 11", "fix round 2" or
  review ids: those files are machine-local, so state the rationale in the
  comment itself. No date stamps ("since 2026-09-16", "bench 2026-07-13"):
  a comment is true at HEAD or it is wrong; history belongs in git and
  `docs/findings/`. Code that still reads old recordings names the old
  format by its version marker, not its date. Links to committed docs
  (including dated `docs/findings/` paths) are fine.
- **No bench or host specifics in code, docs or commits.** Device IPs and
  ssh logins (drone, GS, spare boards), credentials, MACs, and this
  machine's paths/users/NICs stay out of the repo — they live only in
  Claude's memory (`device-addresses`). Write "the drone", "the GS",
  `root@<gs-ip>` or `$GS`; bench scripts take targets from the
  environment (`DRONE=`, `GS=`) with no default. Product addresses the
  software itself ships with (the CPE relay's firmware-fixed `10.83.11.1`,
  loopback ports) and synthetic test values are not bench specifics.

| If the task touches… | Read |
|---|---|
| the stats sideport, maburtop, the debug-log session directory (ctl/probe/au/flight/lat), ausniff, capture tools, player OSD/DVR/record button | `docs/observability.md` |
| colortrans (CRTC 3D LUT, OSD inverse, burned-DVR GPU stage), the Buildroot glibc build of maburplay | `docs/colortrans.md` |
| auto channel selection over a shared channel set, remembered channel, boot hop, pinned mode, which knob drives which piece (search/measure/relocate/verdict/hop), scan.log | `docs/channel-select.md` |
| in-flight channel hop, verdict, dwells, hop_restore | `docs/inflight-channel-hop.md` |
| the shared channel/hop wiring (`ChannelCore`), its seams into maburgs (maburgs and the web GS both drive it), the test fakes | `gs/src/channel_core.h` (header comment) + `docs/channel-select.md` "Where the code lives" |
| live GS settings (set/get on 8402/8403), writing the GS TOMLs from a running daemon, gsparam.py | `docs/gs-live-config.md` |
| the goggles settings menu (buttons, rows, what each row sets, `[input.<button>]`, `[menu]`) | `docs/gs-menu.md` |
| the browser GS / spotter (web/), WebUSB+WASM build, LinkHealthAssembler sharing, channel set form, CHANNEL line, spotter follower | `docs/web-gs.md` |
| the Orange Pi Zero 2W GS (H618): the v4l2req decoder (V4l2ReqBackend, libav v4l2request → Cedrus), MABUR_PLAYER_DECODER, build stage 7, what the board cannot do | `docs/opi-zero2w.md` |
| the CPE510 relay card (mabur-relay protocol v4, maburgs RemoteCard + radio.relays, web GS relay mode, relay TX, interference sweep/hop on a relay) | `docs/cpe510-relay.md` |
| ladder rungs, promote/demote, s3 probes, fade, attribution, RCF drain, the drone's encoder verbs + IDR pacing, the disarmed low-power mode | `../mabur/docs/link-adaptation.md` |
| the GS install script, where things live on the Radxa / OPi / CPE510, the GS image p1 swap | `docs/deploy.md` |
| flag days, the config-load crash loop, the clean deploy sequence, pairing, the flag-day checklist | `../mabur/docs/deploy.md` |
| comparing recordings, metric scales, removed sideport keys | `../mabur/docs/data-provenance.md` |
| calibrating TX-power walls and capacity / efficiency tables, maburcal, cal.log, the MAVLink cal lane | `../mabur/docs/calibration.md`, `../mabur/docs/mavlink.md` |
| camera modes per `venc.fps`, the GS `screen_mode` / fps mismatch warning / letterboxing, the burned recorder at 100 fps | `../mabur/docs/camera-modes.md` |
| airtime/serialization math, FEC overhead, the encoder bitrate formula | `../mabur/docs/airtime-model.md` |
| half-duplex timing, the slotter, why RCFs get lost | `../mabur/docs/tx-rx-timing.md` |
| 40 MHz (HT40): per-rung width, pairs, the boot scan's pair pick, the derived receiver width | `../mabur/docs/bw40.md` |
| the VTX onboard SD recorder, the RCF rec byte, OSD REC field | `../mabur/docs/vtx-recorder.md` |
| pairing / the link key file, KEY MISMATCH, auth_reject | `../mabur/docs/link-pairing.md` |
| the software NACK: T_NACK, the retx ring, the air bucket, feclog 3 | `../mabur/docs/fec-nack.md` |
| H.265 row slices: slice salvage (SliceAssembler, skip-slice fills, aulog 5) and streamed decode (AU ring v4 open slots, StreamFeeder, FeedLoop, the MPP patches in `gs/player/mpp-patches/`) | `../mabur/docs/slices.md` |
| the phase lock (PhaseLock, the regulator's margin and placement switch, `display.phase_lock`, reg.log `lock=` fields) | `../mabur/docs/phase-lock.md` |
| MAVLink tunnel, the GS MAVLink UDP endpoint and its trust boundary | `../mabur/docs/mavlink.md` |
| drone config parameters over MAVLink (`ParamClient`, the web GS VTX tab) | `../mabur/docs/vtx-config-mavlink.md`, `docs/web-gs.md` |
| a specific past investigation, or why something is the way it is | `../mabur/docs/findings/README.md` (dated index) |
| radio/PHY below mabur | `../devourer/CLAUDE.md` |

Design specs and plans live in `docs/superpowers/` — **gitignored**, so
they are on this machine only and anything that must survive belongs in a
committed `docs/` page instead. Never cite them from code comments.

## Compatibility policy

mabur has none: one drone, one GS, one operator, and both ends always run
the latest build. The full policy, and the three things it does not
excuse (deploy is never atomic, config-before-binary, recordings outlive
the code), are in mabur's `CLAUDE.md`. The part that lands here: the
sideport schema and the debug-log session layout are NOT additive-only,
and their in-repo consumers — `tools/maburtop.py`, `tools/flightreport.py`
(which reads both the sideport jsonl and the session directory's log
files), `gs/player`'s OSD — are fixed **in the same commit** as the change.

## Build & test

The dev box keeps the repos as siblings: `../mabur`, `../mabur-ground`,
`../devourer`. NixOS host: wrap every cmake/ctest invocation in
`nix-shell -p pkg-config libusb1 --run "..."`. Configure with
`-DDEVOURER_DIR=$PWD/../devourer` when the default `../devourer` doesn't
resolve. A host build also builds `maburd` from `MABUR_DIR` (the e2e
tests feed maburgs from `maburd --dry-run`). Host suite:
`ctest --test-dir build -R 'test_|gs_e2e|gs_au_e2e|player_e2e|web_au_parity'`.
Cross-build: `tools/build-arm64.sh` (maburgs, maburplay, linkbench-rx,
nhm-reset-probe, encosd; honours `MABUR_DIR`). It must run bare, never
inside that `nix-shell` wrapper — Nix's pkg-config wrapper overrides
`PKG_CONFIG_LIBDIR` and breaks its stage 6 glibc build.
Never load the 8812eu kernel module on the GS cards.

**The standing regression gate for any maburgs change is
`tools/bench/ausniff.py`**, which reads the AU ring from outside the
daemon — the sideport is maburgs self-reporting, so gating a maburgs
change on the sideport is circular. Host-side the equivalent is
`ctest -R 'gs_e2e|gs_au_e2e|player_e2e'`. Any change touching the
AirBalancer, venc, UEP overhead, or the bitrate policy additionally
gates on `tools/bench/aucadence.py` (base−enh completion offset vs the
recorded per-rung baseline). Details in `docs/observability.md`.
