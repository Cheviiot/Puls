# Android C++ package

The `C++ Android` workflow builds a single release APK for `arm64-v8a` and
`x86_64`, signs it with the existing application identity, verifies it, and
launches the signed Qt GUI in an API 36 x86_64 emulator. The application ID is
`io.github.cheviiot.puls`; the only requested permission is `INTERNET`.

The pinned release environment follows the [Qt 6.12 prerequisites](https://doc.qt.io/qt-6.12/android-configure-dev-environment.html):

| Component | Version |
| --- | --- |
| Qt | 6.12.0, including Qt WebSockets and matching host tools |
| Android NDK | r27c, `27.2.12479018` |
| JDK | 21 |
| Android compile/target SDK | 36 |
| Android minimum SDK | 28 |
| Android build tools | 36.0.0 |
| Gradle / Android Gradle Plugin | 9.5.1 / 9.2.1, supplied by Qt |
| OpenSSL | 3.5.9, built from its SHA-256-verified official source |

The exact [Qt 6.12.0 Gradle template](https://github.com/qt/qtbase/blob/v6.12.0/src/android/templates/build.gradle)
and [Gradle wrapper](https://github.com/qt/qtbase/blob/v6.12.0/src/3rdparty/gradle/gradle/wrapper/gradle-wrapper.properties)
supply Gradle dependencies. No custom Gradle project is maintained here.
Qt 6.8.3 is superseded for release packaging by the patched Qt 6.12 line.

The two Qt kits must be adjacent directories named `android_arm64_v8a` and
`android_x86_64`, as documented for [CMake multi-ABI builds](https://doc.qt.io/qt-6.12/android-building-projects-from-commandline.html).
`QT_ANDROID_MULTI_ABI_FORWARD_VARS` forwards the application version and
OpenSSL runtime location into the additional ABI build.

Qt's Android TLS plugin requires separately packaged OpenSSL libraries.
`ci/build_openssl.sh` uses the same NDK as Qt to build both ABIs and installs
`libcrypto_3.so`, `libssl_3.so`, and the upstream license under the selected
build directory. Both the application and OpenSSL use 16 KiB ELF alignment;
the APK verifier checks all packaged native libraries and ZIP alignment.
The [Qt OpenSSL documentation](https://doc.qt.io/qt-6.12/android-openssl-support.html)
explains the `_3` suffix and why Android's private TLS libraries cannot be used.

Signing is restricted to trusted repository jobs with the existing
`PULS_ANDROID_KEYSTORE_BASE64`, `PULS_ANDROID_KEYSTORE_PASSWORD`,
`PULS_ANDROID_KEY_ALIAS`, and `PULS_ANDROID_KEY_PASSWORD` secrets.
The decoded keystore exists only in a private build directory and is removed
after signing. Local and debug keystores are never created. APKs are CI
artifacts; this workflow does not publish a release.

`PULS_ANDROID_VERSION_CODE` follows the existing stable release formula
`major * 1000000 + minor * 1000 + patch`; `0.4.0-cpp-dev` uses `4000`.
The emulator proof covers installation, foreground launch, a screenshot, and
background/resume. It does not start public measurements or collect IP data;
actual Android measurement acceptance remains a separate check.
