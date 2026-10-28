#!/usr/bin/env bash
set -euo pipefail

# Qt's Android TLS plugin loads the _3 suffix. Build the exact verified source
# for Bionic; the device's private BoringSSL is not a compatible substitute.
# Source: https://doc.qt.io/qt-6.12/android-openssl-support.html
openssl_version=3.5.9
openssl_sha256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
output=${1:?usage: build_openssl.sh OUTPUT_DIRECTORY}
: "${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT must point to the Qt-compatible NDK}"
mkdir -p "$output"
output=$(cd "$output" && pwd)
source_dir=$output/source
archive=$output/openssl-$openssl_version.tar.gz
mkdir -p "$source_dir"
curl --fail-with-body --location --silent --show-error \
    "https://github.com/openssl/openssl/releases/download/openssl-$openssl_version/openssl-$openssl_version.tar.gz" \
    --output "$archive"
printf '%s  %s\n' "$openssl_sha256" "$archive" | sha256sum --check --strict
tar -xzf "$archive" --strip-components=1 -C "$source_dir"
cp "$source_dir/LICENSE.txt" "$output/LICENSE.txt"
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
for abi in arm64-v8a x86_64; do
    case "$abi" in
        arm64-v8a) target=android-arm64 ;;
        x86_64) target=android-x86_64 ;;
    esac
    build=$output/build-$abi
    mkdir -p "$build" "$output/$abi"
    (
        cd "$build"
        "$source_dir/Configure" shared no-tests no-legacy "$target" \
            -D__ANDROID_API__=28 -Wl,-z,max-page-size=16384
        ionice -c 3 nice -n 19 make -j2 SHLIB_VERSION_NUMBER= build_libs
        cp libcrypto.so "$output/$abi/libcrypto_3.so"
        cp libssl.so "$output/$abi/libssl_3.so"
    )
    patchelf --page-size 16384 --set-soname libcrypto_3.so "$output/$abi/libcrypto_3.so"
    patchelf --page-size 16384 --set-soname libssl_3.so "$output/$abi/libssl_3.so"
    # OpenSSL 3 normally links libcrypto.so.3; handle the unversioned form too.
    for needed in libcrypto.so.3 libcrypto.so; do
        if patchelf --print-needed "$output/$abi/libssl_3.so" | grep -Fxq "$needed"; then
            patchelf --page-size 16384 --replace-needed "$needed" libcrypto_3.so "$output/$abi/libssl_3.so"
        fi
    done
done
