################################################################################
#
# rockchip-mpp
#
################################################################################

# Pinned to MPP_REF in tools/build-arm64.sh (firmware/tests/mpp-ref.test.sh
# keeps the two equal), so the image's shared librockchip_mpp and the dev
# cross build are one decoder. The fpvOS rkvdec2 stream-mode patches are
# applied from gs/player/mpp-patches/, the one copy both builds use.
ROCKCHIP_MPP_VERSION = 14729dd578e570e5f00fd1dd2113f5429012d64b
ROCKCHIP_MPP_SITE = https://github.com/rockchip-linux/mpp.git
ROCKCHIP_MPP_SITE_METHOD = git
ROCKCHIP_MPP_INSTALL_STAGING = YES
ROCKCHIP_MPP_INSTALL_TARGET = YES

# BUILD_TEST=ON: the stream-mode bench runs mpi_dec_stream_test (added by
# 0001) on the GS. Size cost accepted.
ROCKCHIP_MPP_CONF_OPTS = \
	-DRKPLAT=ON \
	-DHAVE_DRM=ON \
	-DBUILD_TEST=ON

define ROCKCHIP_MPP_APPLY_GS_PATCHES
	$(APPLY_PATCHES) $(@D) $(BR2_EXTERNAL_MABUR_GS_PATH)/../gs/player/mpp-patches \*.patch
endef
ROCKCHIP_MPP_POST_PATCH_HOOKS += ROCKCHIP_MPP_APPLY_GS_PATCHES

$(eval $(cmake-package))
