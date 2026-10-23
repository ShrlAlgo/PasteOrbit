#include "data_protection.h"

#include <QScopeGuard>
#include <limits>
#include <windows.h>
#include <wincrypt.h>

namespace DataProtection {

std::optional<QByteArray> protect(const QByteArray &plain) {
    if (plain.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::nullopt;
    }
    const auto release = qScopeGuard([&] { LocalFree(output.pbData); });
    return QByteArray(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
}

std::optional<QByteArray> unprotect(const QByteArray &cipher) {
    if (cipher.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
    DATA_BLOB input{static_cast<DWORD>(cipher.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::nullopt;
    }
    const auto release = qScopeGuard([&] { LocalFree(output.pbData); });
    return QByteArray(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
}

}
