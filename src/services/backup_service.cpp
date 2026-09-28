#include "backup_service.h"

#include "history_store.h"
#include "localization.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

namespace {
constexpr char BackupMagic[] = "PasteOrbit.Backup";
constexpr qint32 BackupVersion = 1;
constexpr qint64 AuthenticationTagLength = 32;
constexpr ULONG CryptoChunkSize = 64 * 1024;

QString localized(const QString &key) {
    return AppLocalization::get(key);
}

bool readExactly(QIODevice &input, char *destination, qint64 length) {
    qint64 total = 0;
    while (total < length) {
        const qint64 read = input.read(destination + total, length - total);
        if (read <= 0) return false;
        total += read;
    }
    return true;
}

QByteArray readHeaderBytes(QIODevice &input, qint64 length) {
    if (length < 0 || length > 4096) return {};
    QByteArray bytes(static_cast<qsizetype>(length), Qt::Uninitialized);
    return readExactly(input, bytes.data(), length) ? bytes : QByteArray{};
}

bool writeExactly(QIODevice &output, const char *data, qint64 length) {
    qint64 total = 0;
    while (total < length) {
        const qint64 written = output.write(data + total, length - total);
        if (written <= 0) return false;
        total += written;
    }
    return true;
}

template <typename T>
QByteArray littleEndian(T value) {
    const T encoded = qToLittleEndian(value);
    return QByteArray(reinterpret_cast<const char *>(&encoded), sizeof(T));
}

template <typename T>
T readLittleEndian(const char *data) {
    return qFromLittleEndian<T>(reinterpret_cast<const uchar *>(data));
}

class SecureBytes final {
public:
    explicit SecureBytes(qsizetype size = 0) : bytes(size, Qt::Uninitialized) {}
    ~SecureBytes() {
        if (!bytes.isEmpty()) SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
    }
    QByteArray bytes;
};

class HmacSha256 final {
public:
    ~HmacSha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
        if (!object_.isEmpty()) SecureZeroMemory(object_.data(), static_cast<SIZE_T>(object_.size()));
    }

    bool initialize(const char *key, qsizetype keyLength) {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr,
                                        BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
        ULONG objectLength = 0;
        ULONG returned = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                              sizeof(objectLength), &returned, 0) < 0) return false;
        object_.resize(objectLength);
        return BCryptCreateHash(algorithm_, &hash_, reinterpret_cast<PUCHAR>(object_.data()), objectLength,
                                reinterpret_cast<PUCHAR>(const_cast<char *>(key)),
                                static_cast<ULONG>(keyLength), 0) >= 0;
    }

    bool add(const char *data, qint64 length) {
        while (length > 0) {
            const ULONG count = static_cast<ULONG>(qMin<qint64>(length, std::numeric_limits<ULONG>::max()));
            if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char *>(data)), count, 0) < 0) return false;
            data += count;
            length -= count;
        }
        return true;
    }

    QByteArray finish() {
        QByteArray result(32, Qt::Uninitialized);
        if (!hash_ || BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(result.data()),
                                       static_cast<ULONG>(result.size()), 0) < 0) return {};
        return result;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    QByteArray object_;
};

class AesCbc final {
public:
    ~AesCbc() {
        if (key_) BCryptDestroyKey(key_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
        if (!keyObject_.isEmpty()) SecureZeroMemory(keyObject_.data(), static_cast<SIZE_T>(keyObject_.size()));
    }

    bool initialize(const char *key, const QByteArray &iv) {
        if (iv.size() != 16 || BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
        if (BCryptSetProperty(algorithm_, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_CBC)),
                              sizeof(BCRYPT_CHAIN_MODE_CBC), 0) < 0) return false;
        ULONG objectLength = 0;
        ULONG returned = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                              sizeof(objectLength), &returned, 0) < 0) return false;
        keyObject_.resize(objectLength);
        iv_ = iv;
        return BCryptGenerateSymmetricKey(algorithm_, &key_, reinterpret_cast<PUCHAR>(keyObject_.data()),
                                          objectLength, reinterpret_cast<PUCHAR>(const_cast<char *>(key)), 32, 0) >= 0;
    }

    bool transform(const QByteArray &input, QByteArray &output, bool encrypt) {
        if (!key_ || input.isEmpty() || input.size() % 16 != 0) return false;
        output.resize(input.size());
        ULONG written = 0;
        const auto status = encrypt
            ? BCryptEncrypt(key_, reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData())),
                            static_cast<ULONG>(input.size()), nullptr, reinterpret_cast<PUCHAR>(iv_.data()),
                            static_cast<ULONG>(iv_.size()), reinterpret_cast<PUCHAR>(output.data()),
                            static_cast<ULONG>(output.size()), &written, 0)
            : BCryptDecrypt(key_, reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData())),
                            static_cast<ULONG>(input.size()), nullptr, reinterpret_cast<PUCHAR>(iv_.data()),
                            static_cast<ULONG>(iv_.size()), reinterpret_cast<PUCHAR>(output.data()),
                            static_cast<ULONG>(output.size()), &written, 0);
        if (status < 0) return false;
        output.resize(written);
        return true;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
    QByteArray keyObject_;
    QByteArray iv_;
};

QByteArray protectKeys(const QByteArray &keys) {
    DATA_BLOB input{static_cast<DWORD>(keys.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(keys.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    QByteArray result(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return result;
}

QByteArray unprotectKeys(const QByteArray &protectedKeys) {
    DATA_BLOB input{static_cast<DWORD>(protectedKeys.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(protectedKeys.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    QByteArray result(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return result;
}

bool writeAuthenticated(QIODevice &output, HmacSha256 &hmac, const char *data, qint64 length) {
    return hmac.add(data, length) && writeExactly(output, data, length);
}

bool copyEncrypted(QFile &source, class CipherWriter &cipher, qint64 length);

class CipherWriter final {
public:
    CipherWriter(QIODevice &output, HmacSha256 &hmac, AesCbc &cipher)
        : output_(output), hmac_(hmac), cipher_(cipher) {}

    bool add(const char *data, qint64 length) {
        while (length > 0) {
            const qsizetype count = static_cast<qsizetype>(qMin<qint64>(length, 64 * 1024));
            pending_.append(data, count);
            data += count;
            length -= count;
            while (pending_.size() >= static_cast<qsizetype>(CryptoChunkSize)) {
                if (!encrypt(static_cast<qsizetype>(CryptoChunkSize))) return false;
            }
        }
        return true;
    }

    bool finish() {
        const int padding = 16 - (pending_.size() % 16);
        pending_.append(QByteArray(padding, static_cast<char>(padding)));
        while (!pending_.isEmpty()) {
            const qsizetype count = qMin<qsizetype>(pending_.size(), CryptoChunkSize);
            if (!encrypt(count)) return false;
        }
        return true;
    }

private:
    bool encrypt(qsizetype count) {
        QByteArray plain = pending_.left(count);
        QByteArray encrypted;
        if (!cipher_.transform(plain, encrypted, true)
            || !writeAuthenticated(output_, hmac_, encrypted.constData(), encrypted.size())) return false;
        pending_.remove(0, count);
        return true;
    }

    QIODevice &output_;
    HmacSha256 &hmac_;
    AesCbc &cipher_;
    QByteArray pending_;
};

bool copyEncrypted(QFile &source, CipherWriter &cipher, qint64 length) {
    QByteArray buffer(static_cast<qsizetype>(CryptoChunkSize), Qt::Uninitialized);
    while (length > 0) {
        const qint64 count = qMin<qint64>(length, buffer.size());
        if (!readExactly(source, buffer.data(), count) || !cipher.add(buffer.constData(), count)) return false;
        length -= count;
    }
    return true;
}

bool copyExactly(QIODevice &source, QIODevice &destination, qint64 length) {
    QByteArray buffer(static_cast<qsizetype>(CryptoChunkSize), Qt::Uninitialized);
    while (length > 0) {
        const qint64 count = qMin<qint64>(length, buffer.size());
        if (!readExactly(source, buffer.data(), count) || !writeExactly(destination, buffer.constData(), count)) return false;
        length -= count;
    }
    return true;
}

bool decryptPayload(QFile &input, qint64 encryptedLength, AesCbc &cipher, QFile &payload) {
    if (encryptedLength <= 0 || encryptedLength % 16 != 0) return false;
    QByteArray heldBlock;
    while (encryptedLength > 0) {
        const qint64 count = qMin<qint64>(encryptedLength, CryptoChunkSize);
        QByteArray encrypted(static_cast<qsizetype>(count), Qt::Uninitialized);
        if (!readExactly(input, encrypted.data(), count)) return false;
        QByteArray plain;
        if (!cipher.transform(encrypted, plain, false)) return false;
        if (!heldBlock.isEmpty() && !writeExactly(payload, heldBlock.constData(), heldBlock.size())) return false;
        const bool lastChunk = count == encryptedLength;
        if (lastChunk) {
            if (plain.size() < 16) return false;
            const char *lastBlock = plain.constData() + plain.size() - 16;
            const int padding = static_cast<uchar>(lastBlock[15]);
            if (padding < 1 || padding > 16) return false;
            for (int i = 16 - padding; i < 16; ++i) if (lastBlock[i] != padding) return false;
            if (plain.size() > 16 && !writeExactly(payload, plain.constData(), plain.size() - 16)) return false;
            if (padding < 16 && !writeExactly(payload, lastBlock, 16 - padding)) return false;
        } else {
            if (plain.size() > 16 && !writeExactly(payload, plain.constData(), plain.size() - 16)) return false;
            heldBlock = plain.right(16);
        }
        encryptedLength -= count;
    }
    return payload.flush();
}

bool extractPayload(const QString &payloadPath, const QString &databasePath,
                    const QString &settingsPath, bool *hasSettings) {
    QFile payload(payloadPath);
    if (!payload.open(QIODevice::ReadOnly)) return false;
    std::array<char, 16> lengths{};
    if (!readExactly(payload, lengths.data(), lengths.size())) return false;
    const qint64 databaseLength = readLittleEndian<qint64>(lengths.data());
    const qint64 settingsLength = readLittleEndian<qint64>(lengths.data() + 8);
    const qint64 remaining = payload.size() - payload.pos();
    if (databaseLength <= 0 || settingsLength < 0 || databaseLength > remaining
        || settingsLength != remaining - databaseLength) return false;

    QFile database(databasePath);
    if (!database.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !copyExactly(payload, database, databaseLength) || !database.flush()) return false;
    if (settingsLength > 0) {
        QFile settings(settingsPath);
        if (!settings.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || !copyExactly(payload, settings, settingsLength) || !settings.flush()) return false;
        *hasSettings = true;
    } else *hasSettings = false;
    return true;
}

bool replaceFile(const QString &source, const QString &destination) {
    return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                       reinterpret_cast<LPCWSTR>(destination.utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool copyFile(const QString &source, const QString &destination) {
    return CopyFileW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()), FALSE) != FALSE;
}

QString replaceLocalData(const QString &temporaryDatabase, const QString &temporarySettings,
                         bool hasSettings, const QString &databasePath, const QString &settingsPath) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString databaseRecovery = databasePath + QStringLiteral(".before-restore-") + stamp;
    const QString settingsRecovery = settingsPath + QStringLiteral(".before-restore-") + stamp;
    const bool hadDatabase = QFileInfo::exists(databasePath);
    const bool hadSettings = QFileInfo::exists(settingsPath);
    if (hadDatabase && !copyFile(databasePath, databaseRecovery)) return localized(QStringLiteral("RestoreFailed"));
    if (hadSettings && !copyFile(settingsPath, settingsRecovery)) return localized(QStringLiteral("RestoreFailed"));

    // 当前数据库已完成 checkpoint，先移除旧 WAL，避免它被新数据库误用。
    for (const QString &suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QString sidecar = databasePath + suffix;
        if (QFileInfo::exists(sidecar) && !QFile::remove(sidecar))
            return localized(QStringLiteral("RestoreFailed"));
    }

    if (!replaceFile(temporaryDatabase, databasePath)) return localized(QStringLiteral("RestoreFailed"));
    if (hasSettings && !replaceFile(temporarySettings, settingsPath)) {
        if (hadDatabase) copyFile(databaseRecovery, databasePath);
        if (hadSettings) copyFile(settingsRecovery, settingsPath);
        return localized(QStringLiteral("RestoreFailed"));
    }
    return {};
}

bool secureEquals(const QByteArray &left, const QByteArray &right) {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return difference == 0;
}
}

QString BackupService::exportToFile(const QString &databasePath, const QString &settingsPath,
                                    const QString &destinationPath) {
    if (!QFileInfo::exists(databasePath)) return localized(QStringLiteral("HistoryDatabaseNotFound"));
    const QString databaseAbsolute = QFileInfo(databasePath).absoluteFilePath();
    const QString settingsAbsolute = QFileInfo(settingsPath).absoluteFilePath();
    const QString destinationAbsolute = QFileInfo(destinationPath).absoluteFilePath();
    if (destinationAbsolute.compare(databaseAbsolute, Qt::CaseInsensitive) == 0
        || destinationAbsolute.compare(settingsAbsolute, Qt::CaseInsensitive) == 0)
        return localized(QStringLiteral("InvalidBackupFile"));

    const QString checkpointError = HistoryStore(databasePath).compact(false);
    if (!checkpointError.isEmpty()) return checkpointError;
    QFile database(databasePath);
    QFile settings(settingsPath);
    if (!database.open(QIODevice::ReadOnly)) return database.errorString();
    const bool hasSettings = QFileInfo::exists(settingsPath);
    if (hasSettings && !settings.open(QIODevice::ReadOnly)) return settings.errorString();

    SecureBytes encryptionKey(32);
    SecureBytes authenticationKey(32);
    SecureBytes keyMaterial(64);
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(encryptionKey.bytes.data()), 32,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0
        || BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(authenticationKey.bytes.data()), 32,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return localized(QStringLiteral("BackupExportFailed"));
    std::memcpy(keyMaterial.bytes.data(), encryptionKey.bytes.constData(), 32);
    std::memcpy(keyMaterial.bytes.data() + 32, authenticationKey.bytes.constData(), 32);
    const QByteArray protectedKeys = protectKeys(keyMaterial.bytes);
    if (protectedKeys.isEmpty() || protectedKeys.size() > 4096) return localized(QStringLiteral("BackupExportFailed"));

    QByteArray iv(16, Qt::Uninitialized);
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return localized(QStringLiteral("BackupExportFailed"));
    HmacSha256 hmac;
    AesCbc aes;
    if (!hmac.initialize(authenticationKey.bytes.constData(), authenticationKey.bytes.size())
        || !aes.initialize(encryptionKey.bytes.constData(), iv)) return localized(QStringLiteral("BackupExportFailed"));

    QSaveFile output(destinationAbsolute);
    if (!output.open(QIODevice::WriteOnly)) return output.errorString();
    QByteArray header = littleEndian<qint32>(static_cast<qint32>(sizeof(BackupMagic) - 1));
    header.append(BackupMagic, sizeof(BackupMagic) - 1);
    header.append(littleEndian<qint32>(BackupVersion));
    header.append(littleEndian<qint32>(protectedKeys.size()));
    header.append(protectedKeys);
    if (!writeAuthenticated(output, hmac, header.constData(), header.size())
        || !writeAuthenticated(output, hmac, iv.constData(), iv.size())) {
        output.cancelWriting();
        return localized(QStringLiteral("BackupExportFailed"));
    }

    CipherWriter cipher(output, hmac, aes);
    QByteArray payloadLengths = littleEndian<qint64>(database.size());
    payloadLengths.append(littleEndian<qint64>(hasSettings ? settings.size() : 0));
    const bool copied = cipher.add(payloadLengths.constData(), payloadLengths.size())
        && copyEncrypted(database, cipher, database.size())
        && (!hasSettings || copyEncrypted(settings, cipher, settings.size()))
        && cipher.finish();
    if (!copied) {
        output.cancelWriting();
        return localized(QStringLiteral("BackupExportFailed"));
    }
    const QByteArray tag = hmac.finish();
    if (tag.size() != AuthenticationTagLength || !writeExactly(output, tag.constData(), tag.size())
        || !output.commit()) {
        output.cancelWriting();
        return localized(QStringLiteral("BackupExportFailed"));
    }
    return {};
}

QString BackupService::restoreFromFile(const QString &sourcePath, const QString &databasePath,
                                       const QString &settingsPath) {
    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly)) return input.errorString();
    std::array<char, 4> field{};
    if (!readExactly(input, field.data(), field.size())) return localized(QStringLiteral("InvalidBackupFile"));
    const qint32 magicLength = readLittleEndian<qint32>(field.data());
    if (magicLength != static_cast<qint32>(sizeof(BackupMagic) - 1)) return localized(QStringLiteral("InvalidBackupFile"));
    const QByteArray magic = readHeaderBytes(input, magicLength);
    if (magic != QByteArray(BackupMagic, sizeof(BackupMagic) - 1)) return localized(QStringLiteral("InvalidBackupFile"));
    if (!readExactly(input, field.data(), field.size())) return localized(QStringLiteral("BackupFileIncomplete"));
    if (readLittleEndian<qint32>(field.data()) != BackupVersion) return localized(QStringLiteral("UnsupportedBackupVersion"));
    if (!readExactly(input, field.data(), field.size())) return localized(QStringLiteral("BackupFileIncomplete"));
    const qint32 protectedLength = readLittleEndian<qint32>(field.data());
    if (protectedLength <= 0 || protectedLength > 4096) return localized(QStringLiteral("InvalidBackupKeyInfo"));
    const QByteArray protectedKeys = readHeaderBytes(input, protectedLength);
    if (protectedKeys.size() != protectedLength) return localized(QStringLiteral("BackupFileIncomplete"));
    SecureBytes keyMaterial;
    keyMaterial.bytes = unprotectKeys(protectedKeys);
    if (keyMaterial.bytes.size() != 64) return localized(QStringLiteral("InvalidBackupKeyLength"));
    const QByteArray iv = readHeaderBytes(input, 16);
    if (iv.size() != 16) return localized(QStringLiteral("InvalidBackupInitializationVector"));

    const qint64 authenticatedLength = input.size() - AuthenticationTagLength;
    const qint64 payloadOffset = input.pos();
    const qint64 encryptedLength = authenticatedLength - payloadOffset;
    if (authenticatedLength <= payloadOffset || encryptedLength <= 0) return localized(QStringLiteral("BackupContentEmpty"));
    if (encryptedLength % 16 != 0) return localized(QStringLiteral("BackupFileIncomplete"));

    HmacSha256 hmac;
    if (!hmac.initialize(keyMaterial.bytes.constData() + 32, 32) || !input.seek(0))
        return localized(QStringLiteral("BackupCorrupted"));
    QByteArray buffer(static_cast<qsizetype>(CryptoChunkSize), Qt::Uninitialized);
    qint64 remaining = authenticatedLength;
    while (remaining > 0) {
        const qint64 count = qMin<qint64>(remaining, buffer.size());
        if (!readExactly(input, buffer.data(), count) || !hmac.add(buffer.constData(), count))
            return localized(QStringLiteral("BackupFileIncomplete"));
        remaining -= count;
    }
    const QByteArray expectedTag = hmac.finish();
    if (!input.seek(authenticatedLength)) return localized(QStringLiteral("BackupFileIncomplete"));
    const QByteArray storedTag = readHeaderBytes(input, AuthenticationTagLength);
    if (expectedTag.size() != AuthenticationTagLength || !secureEquals(storedTag, expectedTag))
        return localized(QStringLiteral("BackupCorrupted"));

    const QString dataDirectory = QFileInfo(databasePath).absolutePath();
    if (!QDir().mkpath(dataDirectory)) return localized(QStringLiteral("BackupExportFailed"));
    QTemporaryDir temporaryDirectory(QDir(dataDirectory).filePath(QStringLiteral("pasteorbit-restore-XXXXXX")));
    if (!temporaryDirectory.isValid()) return localized(QStringLiteral("BackupExportFailed"));
    const QString payloadPath = temporaryDirectory.filePath(QStringLiteral("payload.bin"));
    const QString temporaryDatabase = temporaryDirectory.filePath(QStringLiteral("history.db"));
    const QString temporarySettings = temporaryDirectory.filePath(QStringLiteral("settings.json"));
    if (!input.seek(payloadOffset)) return localized(QStringLiteral("BackupFileIncomplete"));
    QFile payload(payloadPath);
    if (!payload.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return payload.errorString();
    AesCbc aes;
    if (!aes.initialize(keyMaterial.bytes.constData(), iv)
        || !decryptPayload(input, encryptedLength, aes, payload)) return localized(QStringLiteral("BackupCorrupted"));
    payload.close();

    bool hasSettings = false;
    if (!extractPayload(payloadPath, temporaryDatabase, temporarySettings, &hasSettings))
        return localized(QStringLiteral("InvalidBackupContentLength"));
    if (!HistoryStore::isCurrentSchema(temporaryDatabase))
        return localized(QStringLiteral("UnsupportedBackupDatabaseVersion"));
    const QString checkpointError = HistoryStore(databasePath).compact(true);
    if (!checkpointError.isEmpty()) return checkpointError;
    return replaceLocalData(temporaryDatabase, temporarySettings, hasSettings, databasePath, settingsPath);
}
