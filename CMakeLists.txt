cmake_minimum_required(VERSION 3.16)
project(mabur_ground CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# mabur (the drone, common/ wire formats and FEC) is the submodule at
# third_party/mabur. For a change that spans both repos, point MABUR_DIR at
# a sibling checkout instead: -DMABUR_DIR=$PWD/../mabur.
set(MABUR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mabur" CACHE PATH "mabur checkout")
set(DEVOURER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../devourer" CACHE PATH "devourer checkout")
option(GROUND_BUILD_TESTS "Build the GS unit and e2e tests" ON)
# The e2e tests feed maburgs from a host maburd --dry-run, so a host build
# builds maburd too; the aarch64 cross-build has no use for it.
if(CMAKE_CROSSCOMPILING)
  set(_ground_maburd_default OFF)
else()
  set(_ground_maburd_default ON)
endif()
option(GROUND_BUILD_MABURD "Build maburd from MABUR_DIR (host e2e tests)" ${_ground_maburd_default})

if(NOT EXISTS "${MABUR_DIR}/common/CMakeLists.txt")
  message(FATAL_ERROR "no mabur checkout at MABUR_DIR=${MABUR_DIR}: "
    "run `git submodule update --init third_party/mabur` or pass -DMABUR_DIR")
endif()

# mabur's own options, as normal variables: its cmake_minimum_required makes
# CMP0077 NEW, so its option() calls take these values.
set(MABUR_BUILD_TESTS OFF)
if(NOT DEFINED MABUR_BUILD_LINKBENCH)
  set(MABUR_BUILD_LINKBENCH ON)
endif()
set(MABUR_BUILD_DRONE ${GROUND_BUILD_MABURD})
add_subdirectory(${MABUR_DIR} ${CMAKE_BINARY_DIR}/mabur)
# mabur adds devourer only when it builds maburd.
if(NOT TARGET devourer)
  add_subdirectory(${DEVOURER_DIR} ${CMAKE_BINARY_DIR}/devourer EXCLUDE_FROM_ALL)
endif()

add_subdirectory(gs)
add_subdirectory(gs/player)
add_subdirectory(web)
add_subdirectory(bench/linkbench-rx)
add_subdirectory(bench/nhmreset)
# encosd needs mabur_player_core + the MPP root, so it can only exist after
# gs/player has been added and only in the MPP-decoder MABUR_PLAYER_HW
# (aarch64) build -- its own CMakeLists returns early otherwise.
# v4l2req_streamcheck likewise needs gs/player's decoder choice; it returns
# early except on the V4L2-request build.
add_subdirectory(bench/encosd)
add_subdirectory(bench/v4l2req_streamcheck)
if(GROUND_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
