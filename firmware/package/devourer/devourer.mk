################################################################################
#
# devourer
#
################################################################################

# The commit the mabur being built pins as its third_party/devourer;
# firmware/build.sh reads it and passes it in the environment. Override with
# `make DEVOURER_VERSION=<sha-or-tag> mabur-rebuild`.
ifeq ($(DEVOURER_VERSION),)
$(error devourer: DEVOURER_VERSION is not set (build through firmware/build.sh))
endif
DEVOURER_SITE = https://github.com/gilankpam/devourer.git
DEVOURER_SITE_METHOD = git
DEVOURER_LICENSE = GPL-2.0
DEVOURER_INSTALL_STAGING = NO
DEVOURER_INSTALL_TARGET = NO

# Nothing is compiled here. mabur's top-level CMakeLists.txt pulls devourer in
# with add_subdirectory(${DEVOURER_DIR} ... EXCLUDE_FROM_ALL), so what mabur
# needs is the *source tree on disk* at its own configure time. This package
# exists to make Buildroot fetch and extract it -- and to define DEVOURER_DIR,
# which the package infrastructure sets to $(BUILD_DIR)/devourer-$(VERSION),
# exactly the cache variable mabur's CMake expects. See package/mabur/mabur.mk.
#
# libusb is still a dependency: devourer's CMake does
# pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0), and that runs
# during *mabur's* configure step, so libusb has to be staged by then.
DEVOURER_DEPENDENCIES = libusb

DEVOURER_CONFIGURE_CMDS =
DEVOURER_BUILD_CMDS =
DEVOURER_INSTALL_TARGET_CMDS =

$(eval $(generic-package))
