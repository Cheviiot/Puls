#!/usr/bin/env bash
set -euo pipefail

# Qt's Android TLS plugin loads the _3 suffix. Build the exact verified source
# for Bionic; the device's private BoringSSL is not a compatible substitute.
# Source: https://doc.qt.io/qt-6.12/android-openssl-support.html
openssl_version=3.5.9
openssl_sha256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
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
readelf=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
for abi in arm64-v8a x86_64; do
    case "$abi" in
        arm64-v8a) target=puls-android-arm64 ;;
        x86_64) target=puls-android-x86_64 ;;
    esac
    build=$output/build-$abi
    mkdir -p "$build" "$output/$abi"
    (
        cd "$build"
        "$source_dir/Configure" --config="$script_dir/../openssl.cnf" \
            shared no-tests no-legacy "$target" \
            -D__ANDROID_API__=28 -Wl,-z,max-page-size=16384
        ionice -c 3 nice -n 19 make -j2 build_libs
        cp libcrypto_3.so libssl_3.so "$output/$abi/"
    )
    # Verify the final ELF before Qt/Gradle packaging. Do not edit the linked
    # ELF with patchelf: added load segments may lose Android page alignment.
    for library in crypto ssl; do
        "$readelf" --program-headers --dynamic "$output/$abi/lib${library}_3.so"
        "$readelf" --dynamic "$output/$abi/lib${library}_3.so" \
            | grep -Fq "Library soname: [lib${library}_3.so]"
    done
    "$readelf" --dynamic "$output/$abi/libssl_3.so" \
        | grep -Fq 'Shared library: [libcrypto_3.so]'
    python3 -B - "$script_dir" "$output/$abi" "$abi" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
from verify_apk import verify_elf
machine = {"arm64-v8a": 183, "x86_64": 62}[sys.argv[3]]
for name in ("libcrypto_3.so", "libssl_3.so"):
    library = Path(sys.argv[2]) / name
    with library.open("rb") as source:
        verify_elf(source, str(library), machine)
PY
done
