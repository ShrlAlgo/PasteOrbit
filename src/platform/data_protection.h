#pragma once

#include <QByteArray>
#include <optional>

namespace DataProtection {
// 本机当前用户的数据保护；失败返回 nullopt，不降级为明文存储。
std::optional<QByteArray> protect(const QByteArray &plain);
std::optional<QByteArray> unprotect(const QByteArray &cipher);
}
