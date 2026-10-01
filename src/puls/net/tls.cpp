#include "puls/net/tls.hpp"

#include "puls/core/text.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>

#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#endif

namespace puls::net {

namespace {

// Adds the first PEM certificate of every regular file in directory; text
// after the certificate, which Android appends, is ignored. Returns the
// number of certificates added.
int add_certificate_directory(X509_STORE* store, const std::filesystem::path& directory) {
    int added = 0;
    std::error_code error;
    std::filesystem::directory_iterator entry(directory, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        std::error_code status_error;
        if (!entry->is_regular_file(status_error)) {
            continue;
        }
        // OpenSSL opens UTF-8 file names on Windows as well.
        const std::u8string name = entry->path().u8string();
        BIO* bio = BIO_new_file(reinterpret_cast<const char*>(name.c_str()), "r");
        if (bio == nullptr) {
            continue;
        }
        if (X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
            X509_STORE_add_cert(store, certificate);
            X509_free(certificate);
            ++added;
        }
        BIO_free(bio);
    }
    ERR_clear_error();
    return added;
}

#if defined(_WIN32) || defined(__APPLE__)
void add_certificate(X509_STORE* store, const unsigned char* data, long size) {
    X509* certificate = d2i_X509(nullptr, &data, size);
    if (certificate != nullptr) {
        X509_STORE_add_cert(store, certificate);
        X509_free(certificate);
    }
}
#endif

#if defined(_WIN32)

void load_system_roots(SSL_CTX*, X509_STORE* store) {
    for (const wchar_t* name : {L"ROOT", L"CA"}) {
        HCERTSTORE system_store = CertOpenSystemStoreW(0, name);
        if (system_store == nullptr) {
            continue;
        }
        PCCERT_CONTEXT certificate = nullptr;
        while ((certificate = CertEnumCertificatesInStore(system_store, certificate)) != nullptr) {
            // The CA store holds intermediates; only self-signed ones are
            // trust anchors.
            if (std::wstring_view(name) == L"CA" &&
                !CertCompareCertificateName(X509_ASN_ENCODING, &certificate->pCertInfo->Subject,
                                            &certificate->pCertInfo->Issuer)) {
                continue;
            }
            add_certificate(store, certificate->pbCertEncoded,
                            static_cast<long>(certificate->cbCertEncoded));
        }
        CertCloseStore(system_store, 0);
    }
}

#elif defined(__APPLE__)

void add_security_certificate(X509_STORE* store, SecCertificateRef certificate) {
    CFDataRef data = SecCertificateCopyData(certificate);
    if (data == nullptr) {
        return;
    }
    add_certificate(store, CFDataGetBytePtr(data), static_cast<long>(CFDataGetLength(data)));
    CFRelease(data);
}

// Admin and user trust settings may also distrust certificates, so only
// entries that are trusted as roots are imported.
bool trusted_as_root(SecCertificateRef certificate, SecTrustSettingsDomain domain) {
    CFArrayRef settings = nullptr;
    if (SecTrustSettingsCopyTrustSettings(certificate, domain, &settings) != errSecSuccess ||
        settings == nullptr) {
        return false;
    }
    bool trusted = CFArrayGetCount(settings) == 0;
    for (CFIndex index = 0; !trusted && index < CFArrayGetCount(settings); ++index) {
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(settings, index));
        auto result =
            static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kSecTrustSettingsResult));
        SInt32 value = kSecTrustSettingsResultTrustRoot;
        if (result != nullptr) {
            CFNumberGetValue(result, kCFNumberSInt32Type, &value);
        }
        trusted = value == kSecTrustSettingsResultTrustRoot ||
                  value == kSecTrustSettingsResultTrustAsRoot;
    }
    CFRelease(settings);
    return trusted;
}

void load_system_roots(SSL_CTX*, X509_STORE* store) {
    CFArrayRef anchors = nullptr;
    if (SecTrustCopyAnchorCertificates(&anchors) == errSecSuccess && anchors != nullptr) {
        for (CFIndex index = 0; index < CFArrayGetCount(anchors); ++index) {
            add_security_certificate(store, static_cast<SecCertificateRef>(const_cast<void*>(
                                                CFArrayGetValueAtIndex(anchors, index))));
        }
        CFRelease(anchors);
    }
    for (const SecTrustSettingsDomain domain :
         {kSecTrustSettingsDomainAdmin, kSecTrustSettingsDomainUser}) {
        CFArrayRef certificates = nullptr;
        if (SecTrustSettingsCopyCertificates(domain, &certificates) != errSecSuccess ||
            certificates == nullptr) {
            continue;
        }
        for (CFIndex index = 0; index < CFArrayGetCount(certificates); ++index) {
            auto certificate = static_cast<SecCertificateRef>(
                const_cast<void*>(CFArrayGetValueAtIndex(certificates, index)));
            if (trusted_as_root(certificate, domain)) {
                add_security_certificate(store, certificate);
            }
        }
        CFRelease(certificates);
    }
}

#elif defined(__ANDROID__)

// The updatable store of Android 14 and newer, then the system store.
constexpr std::string_view certificate_directories[] = {
    "/apex/com.android.conscrypt/cacerts",
    "/system/etc/security/cacerts",
};

// Android names the files by the subject hash of OpenSSL 0.9.8, which the
// hashed directory lookup of OpenSSL 3 does not find, so every certificate
// is added to the store. The updatable store replaces the system one.
void load_system_roots(SSL_CTX*, X509_STORE* store) {
    for (const std::string_view directory : certificate_directories) {
        if (add_certificate_directory(store, std::filesystem::path(directory)) > 0) {
            return;
        }
    }
}

#else

// The same locations Go's crypto/x509 uses on Linux and BSD.
constexpr std::string_view certificate_files[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",
    "/etc/pki/tls/cacert.pem",
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    "/etc/ssl/cert.pem",
    "/usr/local/etc/ssl/cert.pem",
    "/usr/local/share/certs/ca-root-nss.crt",
};
constexpr std::string_view certificate_directories[] = {
    "/etc/ssl/certs",
    "/etc/pki/tls/certs",
};

bool is_regular_file(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool is_directory(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

void load_system_roots(SSL_CTX* context, X509_STORE*) {
    const char* file_override = std::getenv("SSL_CERT_FILE");
    const char* directory_override = std::getenv("SSL_CERT_DIR");

    std::vector<std::string> files;
    if (file_override != nullptr && *file_override != '\0') {
        files.emplace_back(file_override);
    } else {
        files.assign(std::begin(certificate_files), std::end(certificate_files));
    }
    for (const auto& file : files) {
        if (is_regular_file(file) &&
            SSL_CTX_load_verify_locations(context, file.c_str(), nullptr) == 1) {
            break;
        }
    }

    std::vector<std::string> directories;
    if (directory_override != nullptr && *directory_override != '\0') {
        std::string_view remaining = directory_override;
        while (!remaining.empty()) {
            const auto colon = remaining.find(':');
            directories.emplace_back(remaining.substr(0, colon));
            remaining =
                colon == std::string_view::npos ? std::string_view() : remaining.substr(colon + 1);
        }
    } else {
        directories.assign(std::begin(certificate_directories), std::end(certificate_directories));
    }
    for (const auto& directory : directories) {
        if (is_directory(directory)) {
            SSL_CTX_load_verify_locations(context, nullptr, directory.c_str());
        }
    }
    // Distribution OpenSSL builds know their own store; vcpkg builds point to
    // their install prefix, which is harmless when it does not exist.
    SSL_CTX_set_default_verify_paths(context);
}

#endif

Error add_pem_roots(X509_STORE* store, const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return Error::make("не удалось прочитать корневой сертификат");
    }
    int added = 0;
    while (X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        X509_STORE_add_cert(store, certificate);
        X509_free(certificate);
        ++added;
    }
    BIO_free(bio);
    ERR_clear_error();
    if (added == 0) {
        return Error::make("не удалось прочитать корневой сертификат");
    }
    return {};
}

} // namespace

TlsContext::TlsContext() : context_(boost::asio::ssl::context::tls_client) {}

Result<std::shared_ptr<TlsContext>> TlsContext::create(const TlsOptions& options) {
    std::shared_ptr<TlsContext> result;
    try {
        result = std::shared_ptr<TlsContext>(new TlsContext());
    } catch (const std::exception& error) {
        return Error::make(std::string("инициализация TLS: ") + error.what());
    }
    SSL_CTX* native = result->context_.native_handle();
    SSL_CTX_set_min_proto_version(native, TLS1_2_VERSION);
    SSL_CTX_set_options(native, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_verify(native, SSL_VERIFY_PEER, nullptr);
    X509_STORE* store = SSL_CTX_get_cert_store(native);
    if (options.use_system_roots) {
        load_system_roots(native, store);
    }
    for (const auto& pem : options.extra_root_certificates) {
        if (Error error = add_pem_roots(store, pem)) {
            return error;
        }
    }
    for (const auto& directory : options.root_certificate_directories) {
        if (add_certificate_directory(store, directory) == 0) {
            const std::u8string name = directory.u8string();
            return Error::make("не удалось прочитать корневые сертификаты из " +
                               text::quote(std::string(name.begin(), name.end())));
        }
    }
    ERR_clear_error();
    return result;
}

std::shared_ptr<TlsContext> TlsContext::system() {
    static const std::shared_ptr<TlsContext> context = [] {
        auto created = create(TlsOptions{});
        if (!created) {
            throw std::runtime_error(created.error().message());
        }
        return std::move(created).value();
    }();
    return context;
}

} // namespace puls::net
