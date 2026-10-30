#include "data_protection.h"

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <Security/Security.h>
#include <QMutex>
#include <QMutexLocker>
#include <cstring>
#include <limits>

namespace {
constexpr char Envelope[] = "POM1";
void wipe(QByteArray &bytes) {
    volatile char *data = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) data[i] = 0;
}

struct Secret {
    QByteArray bytes;
    ~Secret() { wipe(bytes); }
};

QByteArray readKey(CFDictionaryRef query, OSStatus *result = nullptr) {
    CFTypeRef value = nullptr;
    const auto status = SecItemCopyMatching(query, &value);
    if (result) *result = status;
    if (status != errSecSuccess || !value) return {};
    QByteArray key;
    if (CFGetTypeID(value) == CFDataGetTypeID()) {
        auto data = static_cast<CFDataRef>(value);
        if (CFDataGetLength(data) == 64)
            key = QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), 64);
    }
    CFRelease(value);
    return key;
}

QByteArray localKey(bool create) {
    static QMutex mutex;
    static Secret cached;
    QMutexLocker lock(&mutex);
    if (cached.bytes.size() == 64) return cached.bytes;
    auto query = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("com.pasteorbit.PasteOrbit"));
    CFDictionarySetValue(query, kSecAttrAccount, CFSTR("history-key-v1"));
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    OSStatus lookup = errSecSuccess;
    cached.bytes = readKey(query, &lookup);
    if (lookup == errSecItemNotFound && create) {
        Secret generated;
        generated.bytes.resize(64);
        if (SecRandomCopyBytes(kSecRandomDefault, 64, generated.bytes.data()) == errSecSuccess) {
            auto attributes = CFDictionaryCreateMutableCopy(nullptr, 0, query);
            CFDictionaryRemoveValue(attributes, kSecReturnData);
            CFDictionaryRemoveValue(attributes, kSecMatchLimit);
            auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(generated.bytes.constData()), 64);
            CFDictionarySetValue(attributes, kSecValueData, data);
            CFDictionarySetValue(attributes, kSecAttrLabel, CFSTR("PasteOrbit history encryption key"));
            // 钥匙串记录只创建一次；并发进程竞争时重新读取，不覆盖已有密钥。
            const auto status = SecItemAdd(attributes, nullptr);
            if (status == errSecSuccess) cached.bytes = generated.bytes;
            else if (status == errSecDuplicateItem) cached.bytes = readKey(query);
            CFRelease(data);
            CFRelease(attributes);
        }
    }
    CFRelease(query);
    return cached.bytes;
}

QByteArray tag(const QByteArray &key, const QByteArray &data) {
    QByteArray result(32, Qt::Uninitialized);
    CCHmac(kCCHmacAlgSHA256, key.constData() + 32, 32,
           data.constData(), static_cast<size_t>(data.size()), result.data());
    return result;
}

bool equalTag(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return difference == 0;
}
}

namespace DataProtection {
std::optional<QByteArray> protect(const QByteArray &plain) {
    if (plain.size() > std::numeric_limits<qsizetype>::max() - 68) return std::nullopt;
    Secret key{localKey(true)};
    if (key.bytes.size() != 64) return std::nullopt;
    QByteArray iv(16, Qt::Uninitialized);
    if (SecRandomCopyBytes(kSecRandomDefault, 16, iv.data()) != errSecSuccess) return std::nullopt;
    QByteArray encrypted(plain.size() + 16, Qt::Uninitialized);
    size_t written = 0;
    if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.bytes.constData(), 32,
                iv.constData(), plain.constData(), static_cast<size_t>(plain.size()),
                encrypted.data(), static_cast<size_t>(encrypted.size()), &written) != kCCSuccess)
        return std::nullopt;
    encrypted.resize(static_cast<qsizetype>(written));
    QByteArray result = QByteArray(Envelope, 4) + iv + encrypted;
    result.append(tag(key.bytes, result));
    return result;
}

std::optional<QByteArray> unprotect(const QByteArray &cipher) {
    if (cipher.size() < 68 || !cipher.startsWith(QByteArray(Envelope, 4)) || (cipher.size() - 52) % 16)
        return std::nullopt;
    Secret key{localKey(false)};
    if (key.bytes.size() != 64) return std::nullopt;
    // 先恒时校验认证码，再解密；系统密钥不可用或记录被修改时拒绝读取。
    const auto authenticated = cipher.first(cipher.size() - 32);
    if (!equalTag(tag(key.bytes, authenticated), cipher.last(32))) return std::nullopt;
    const qsizetype length = cipher.size() - 52;
    QByteArray plain(length, Qt::Uninitialized);
    size_t written = 0;
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.bytes.constData(), 32,
                cipher.constData() + 4, cipher.constData() + 20, static_cast<size_t>(length),
                plain.data(), static_cast<size_t>(plain.size()), &written) != kCCSuccess) {
        wipe(plain);
        return std::nullopt;
    }
    plain.resize(static_cast<qsizetype>(written));
    return plain;
}
}
