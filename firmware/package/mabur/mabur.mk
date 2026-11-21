################################################################################
#
# mabur
#
################################################################################

# Built from the mabur-ground tree this package sits in (firmware/build.sh
# passes MABUR_GS_SRC_DIR), so an image is always the maburgs/maburplay of the
# commit it was built from. Buildroot rsyncs that tree into its build dir; the
# exclusions keep out this build's own Buildroot tree and outputs, local build
# outputs, the docs, the pinned mabur's own devourer submodule and firmware
# build (devourer is its own package), machine-local agent state and link keys.
# mabur itself comes from MABUR_GS_MABUR_DIR: the third_party/mabur pin, or a
# sibling checkout when a change spans both repos.
ifeq ($(MABUR_GS_SRC_DIR),)
$(error mabur: MABUR_GS_SRC_DIR is not set (build through firmware/build.sh))
endif
MABUR_SITE = $(MABUR_GS_SRC_DIR)
MABUR_SITE_METHOD = local
MABUR_VERSION = local
MABUR_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude /firmware/buildroot --exclude /firmware/output \
	--exclude '/build*' --exclude /out --exclude /toolchain --exclude /logs \
	--exclude /docs --exclude /web/build-wasm \
	--exclude /third_party/mabur/third_party/devourer \
	--exclude /third_party/mabur/firmware/openipc \
	--exclude /.claude --exclude /.superpowers --exclude '/*.key'
MABUR_SUPPORTS_IN_SOURCE_BUILD = NO
MABUR_INSTALL_STAGING = NO
MABUR_INSTALL_TARGET = YES

# devourer supplies the source tree mabur's CMake add_subdirectory()s; the rest
# are real link/staging dependencies. The decoder choice (Config.in) picks
# the rest: Rockchip MPP (rockchip-mpp, plus mesa3d + librga for the GPU
# colortrans stage; rockchip-mpp and libdrm have to be staged before the
# pre-configure hook below runs) or V4L2 request (ffmpeg-v4l2request, whose
# libavcodec/libavutil mabur finds through pkg-config).
#
# No libgpiod: maburplay's record button talks to <linux/gpio.h> ioctls
# directly (gs/player/src/rec_button.cpp), deliberately, so mabur links nothing
# for it. The boards still enable libgpiod-tools for `gpioinfo`, which is how
# you find the line name for a given header pin.
MABUR_DEPENDENCIES = devourer libusb libdrm

ifeq ($(BR2_PACKAGE_MABUR_DECODER_V4L2REQ),y)
MABUR_DEPENDENCIES += ffmpeg-v4l2request
MABUR_DECODER_OPTS = -DMABUR_PLAYER_DECODER=v4l2req
else
MABUR_DEPENDENCIES += rockchip-mpp mesa3d librga
MABUR_PRE_CONFIGURE_HOOKS += MABUR_STAGE_LIBS
MABUR_DECODER_OPTS = \
	-DMABUR_PLAYER_DECODER=mpp \
	-DMABUR_PLAYER_GPU=ON \
	-DMABUR_MPP_ROOT=$(@D)/br-libs \
	-DMABUR_DRM_ROOT=$(@D)/br-libs
endif

# MPP decoder only. gs/player/CMakeLists.txt links two libraries by absolute
# path:
#
#   ${MABUR_MPP_ROOT}/lib/librockchip_mpp.a
#   ${MABUR_DRM_ROOT}/lib/libdrm.a
#
# That shape is inherited from this repo's musl-static cross build
# (tools/build-arm64.sh), where both really are static archives staged under
# toolchain/. Buildroot stages them differently: rockchip-mpp's CMake does
# install a genuine librockchip_mpp.a next to the shared object, but libdrm is
# a Meson package built shared-only, so libdrm.a does not exist in staging.
#
# Rather than patch gs/player or turn on BR2_SHARED_STATIC_LIBS for the whole
# build, stage one small prefix that satisfies both cache variables. Both .a
# entries are symlinks to the *shared* objects: ld identifies an input file by
# its contents, not its extension, so it links dynamically and records
# libdrm.so.2 / librockchip_mpp.so.1 as DT_NEEDED. Verify with
# `readelf -d .../usr/local/bin/maburplay`.
#
# Pointing the mpp entry at the shared object rather than the real
# librockchip_mpp.a that rockchip-mpp does stage is deliberate. Buildroot
# installs librockchip_mpp.so.0 (~8.9 MB) to the target no matter what, and
# with pixelpilot gone maburplay is its only possible consumer -- static
# linking would leave that 8.9 MB on the rootfs with nothing referencing it
# AND carry a second copy of the used objects inside the binary. (The staged
# .a itself never reaches the image; target-finalize deletes *.a.)
#
# The header side needs no shim. mabur includes <rockchip/rk_mpi.h> and staging
# has usr/include/rockchip/; it includes <xf86drm.h> and <drm_fourcc.h>, and it
# already adds both ${ROOT}/include and ${ROOT}/include/libdrm, which staging
# provides at exactly those two levels.
#
# Delete this hook if gs/player ever learns to link -lrockchip_mpp -ldrm from the
# sysroot directly.
define MABUR_STAGE_LIBS
	mkdir -p $(@D)/br-libs/lib
	ln -sfn $(STAGING_DIR)/usr/include $(@D)/br-libs/include
	ln -sfn $(STAGING_DIR)/usr/lib/librockchip_mpp.so \
		$(@D)/br-libs/lib/librockchip_mpp.a
	ln -sfn $(STAGING_DIR)/usr/lib/libdrm.so $(@D)/br-libs/lib/libdrm.a
endef

# Mirrors tools/build-arm64.sh, the reference cross build for this target.
# maburd is SigmaStar/armv7 and never built here; the host test suite needs
# GoogleTest; the bench tools are not shipped, so only the two daemons are
# built (MABUR_MAKE_OPTS).
#
# The per-chip devourer gates are listed exhaustively on purpose: every
# DEVOURER_<chip> option defaults to ON, so a chip added upstream would opt
# itself into this build on the next hash bump and silently inflate the binary.
# maburplay's record button speaks the v2 GPIO uAPI (Linux 5.10+), which the
# aarch64 external toolchains ship no headers for -- see the long note in
# package/mabur/gpio_v2_compat.h. Force-include the guarded fallback so
# rec_button.cpp compiles; it is inert once the toolchain catches up.
# Prepending $(TARGET_CFLAGS)/$(TARGET_CXXFLAGS) is required, not optional:
# Buildroot's toolchainfile.cmake sets the flags only `if(NOT DEFINED
# CMAKE_C_FLAGS)`, so passing -DCMAKE_C_FLAGS at all takes ownership of them.
# This is the override path that file documents in its own comments.
MABUR_GPIO_COMPAT_FLAG = -include $(MABUR_PKGDIR)/gpio_v2_compat.h

# -DBUILD_SHARED_LIBS=OFF is load-bearing, not tidiness. Buildroot's
# cmake-package passes BUILD_SHARED_LIBS=ON (anything but a BR2_STATIC_LIBS
# build), and devourer's `add_library(devourer ...)` names neither STATIC nor
# SHARED, so it honours that and produces an unversioned libdevourer.so.
# package/devourer installs nothing to the target by design -- it exists only
# to put the source on disk -- so maburgs then died at startup with
#   error while loading shared libraries: libdevourer.so
# Every library mabur and mabur-ground declare is explicitly STATIC, so forcing
# this off only affects devourer, and it links it into maburgs exactly as
# tools/build-arm64.sh does. Our -D comes after Buildroot's on the command line, so it
# wins. Re-verify with `readelf -d` after any devourer bump.
#
# MABUR_DECODER_OPTS (above): on the MPP decoder, -DMABUR_PLAYER_GPU=ON is the
# burned-DVR colortrans stage (gs/player/src/frame_colortrans.cpp), which links
# EGL/GLESv2/gbm/rga from staging (docs/colortrans.md). The V4L2-request
# decoder has no encoder, so no burned DVR and no GPU stage.
MABUR_MAKE_OPTS = maburgs maburplay

MABUR_CONF_OPTS = \
	-DBUILD_SHARED_LIBS=OFF \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) $(MABUR_GPIO_COMPAT_FLAG)" \
	-DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) $(MABUR_GPIO_COMPAT_FLAG)" \
	-DMABUR_DIR=$(MABUR_GS_MABUR_DIR) \
	-DGROUND_BUILD_TESTS=OFF \
	-DGROUND_BUILD_MABURD=OFF \
	-DMABUR_BUILD_LINKBENCH=OFF \
	-DMABUR_PLAYER_HW=ON \
	$(MABUR_DECODER_OPTS) \
	-DDEVOURER_DIR=$(DEVOURER_SRCDIR) \
	-DDEVOURER_LOG_MAX_LEVEL=WARN \
	-DDEVOURER_JAGUAR1=OFF \
	-DDEVOURER_8814=OFF \
	-DDEVOURER_JAGUAR2_8822B=OFF \
	-DDEVOURER_JAGUAR2_8821C=OFF \
	-DDEVOURER_JAGUAR3_8822C=OFF \
	-DDEVOURER_JAGUAR3_8822E=ON \
	-DDEVOURER_8733B=OFF \
	-DDEVOURER_KESTREL_8852B=OFF \
	-DDEVOURER_KESTREL_8852C=OFF

# mabur-ground declares no install() rules -- its deploy scripts copy
# artifacts by hand -- so the layout is spelled out here.
#
# The two .default.toml files do NOT go to /etc any more. /etc is on the
# read-only squashfs (writable only through the ext4 overlay, which Windows
# cannot read), and the point of the config partition is that these two files
# are editable with the card in a laptop. So they are installed as *defaults*
# under /usr/share/config-defaults, /etc/init.d/S00config copies any missing
# default onto the FAT32 CONFIG partition on first boot, and the daemons read
# them straight from /config (S96maburgs and S97maburplay prefer
# /config/<file> over /etc/<file>). The pairing key sits beside them: the installed
# maburgs default names /config/mabur.key, so the key is dropped onto the
# same partition. The bundle files keep /etc for install.sh hosts.
#
# Both files are otherwise installed verbatim, and nothing overrides them
# wholesale any more. A board that needs a different default edits the
# installed file in board/common/post-build-script.sh instead -- see the note
# above MABUR_INSTALL_INIT_SYSV.
#
# The three assets under /usr/local/share/mabur are runtime files, not linked-in
# blobs: maburplay.toml names font_btfl.mfont and gs_osd.gfont by path, and
# splash.bin is hardcoded in splash_image.h with no config key.
#
# maburtop and maburcal go to /usr/bin, not /usr/local/bin: the GS shell's
# default PATH does not include /usr/local/bin. Both import only the standard
# library, so python3 + python3-curses is the whole requirement -- maburcal
# needs no curses at all, it is argparse/socket only.
#
# maburcal is the TX-power wall calibration kit's entire operator UI, and it
# has to be on the box: `start`/`status`/`abort` speak UDP to CalControl in
# maburgs, which binds 127.0.0.1:8400 only, so there is no calibrating this
# board from anywhere but a shell on it. (`maburcal report <cal.log>` is
# offline and would run on a laptop, but shipping half the tool is worse than
# shipping it.)
define MABUR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(MABUR_BUILDDIR)/gs/maburgs \
		$(TARGET_DIR)/usr/local/bin/maburgs
	$(INSTALL) -D -m 0755 $(MABUR_BUILDDIR)/gs/player/maburplay \
		$(TARGET_DIR)/usr/local/bin/maburplay

	$(INSTALL) -D -m 0644 $(@D)/gs/player/bundle/font_btfl.mfont \
		$(TARGET_DIR)/usr/local/share/mabur/font_btfl.mfont
	$(INSTALL) -D -m 0644 $(@D)/gs/player/bundle/gs_osd.gfont \
		$(TARGET_DIR)/usr/local/share/mabur/gs_osd.gfont
	$(INSTALL) -D -m 0644 $(@D)/gs/player/bundle/splash.bin \
		$(TARGET_DIR)/usr/local/share/mabur/splash.bin

	$(INSTALL) -D -m 0755 $(@D)/tools/maburtop.py \
		$(TARGET_DIR)/usr/bin/maburtop
	$(INSTALL) -D -m 0755 $(@D)/gs/bundle/maburcal \
		$(TARGET_DIR)/usr/bin/maburcal

	$(INSTALL) -D -m 0644 $(@D)/gs/bundle/maburgs.default.toml \
		$(TARGET_DIR)/usr/share/config-defaults/maburgs.toml
	$(INSTALL) -D -m 0644 $(@D)/gs/player/bundle/maburplay.default.toml \
		$(TARGET_DIR)/usr/share/config-defaults/maburplay.toml
	sed -i 's|"/etc/mabur.key"|"/config/mabur.key"|' \
		$(TARGET_DIR)/usr/share/config-defaults/maburgs.toml
	grep -q '^key_file *= *"/config/mabur.key"' \
		$(TARGET_DIR)/usr/share/config-defaults/maburgs.toml
endef

# Both wrappers are this repo's bundled files, installed unmodified.
# S96maburgs' start does `rmmod 8812eu` so devourer can claim the cards over
# libusb; that is a no-op here because the mabur boards drop the Realtek kernel
# drivers entirely, and it is kept for hosts that do load the driver.
#
# Per-board maburplay defaults are sed'd onto the installed file by
# board/common/post-build-script.sh, which runs after both this install and the
# rootfs overlays. The boards used to ship a whole forked copy of
# maburplay.default.toml under board/*/overlay/usr/share/config-defaults/,
# and that copy went stale the moment gs/player added a key: [colortrans] = true shipped disabled on every image after
# the GPU colortrans stage landed. Patch the differing lines, never the file.
define MABUR_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(@D)/gs/bundle/S96maburgs \
		$(TARGET_DIR)/etc/init.d/S96maburgs
	$(INSTALL) -D -m 0755 $(@D)/gs/player/bundle/S97maburplay \
		$(TARGET_DIR)/etc/init.d/S97maburplay
endef

$(eval $(cmake-package))
