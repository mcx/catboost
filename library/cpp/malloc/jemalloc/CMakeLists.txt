# This file was generated by the YaTool build system (https://github.com/yandex/yatool),
# from a source YaTool build configuration provided in ya.make files.
#
# If the repository supports both CMake and ya build configurations, please modify both of them.
#
# If only CMake build configuration is supported then modify only CMake files and note that only
# simple modifications are allowed like adding source-files to targets or adding simple properties
# like target_include_directories. These modifications will be ported to original ya.make files
# by maintainers. Any complex modifications which can't be easily ported back to the ya build
# system may be rejected.
#
# Please refer to the build instructions in the repository for more information about manual
# changes in this file.

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" AND NOT HAVE_CUDA)
  include(CMakeLists.linux-aarch64.txt)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" AND HAVE_CUDA)
  include(CMakeLists.linux-aarch64-cuda.txt)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  include(CMakeLists.darwin-x86_64.txt)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  include(CMakeLists.darwin-arm64.txt)
elseif (ANDROID AND CMAKE_ANDROID_ARCH STREQUAL "arm")
  include(CMakeLists.android-arm.txt)
elseif (ANDROID AND CMAKE_ANDROID_ARCH STREQUAL "arm64")
  include(CMakeLists.android-arm64.txt)
elseif (ANDROID AND CMAKE_ANDROID_ARCH STREQUAL "x86")
  include(CMakeLists.android-x86.txt)
elseif (ANDROID AND CMAKE_ANDROID_ARCH STREQUAL "x86_64")
  include(CMakeLists.android-x86_64.txt)
endif()

