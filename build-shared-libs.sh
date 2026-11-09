#!/bin/sh

set -e

if [ -z "$ANDROID_NDK_HOME" ]; then
    echo 'You need to set the ANDROID_NDK_HOME environment variable to point to your Android NDK.'
    exit 1
fi

ABI_NAMES=${ABI_NAMES:-arm64-v8a}
ANDROID_API_LEVEL=${ANDROID_API_LEVEL:-26}
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}

for abi in ${ABI_NAMES}; do
    cmake -B build/${abi} \
      -S . \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_ANDROID_ARCH_ABI=${abi} \
      -DANDROID_PLATFORM=${ANDROID_API_LEVEL} \
      -DANDROID_USE_LEGACY_TOOLCHAIN_FILE=OFF \
      -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
      -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake
    ninja -C build/${abi} install
    ./collect-licenses.sh build/${abi} project/app/src/main/assets/licenses
done
