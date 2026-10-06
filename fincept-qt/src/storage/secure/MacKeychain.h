#pragma once
#include "core/result/Result.h"

#include <QByteArray>
#include <QString>

namespace fincept::mac_keychain {
// A random account identifies one database key; add never overwrites an item.
Result<void> add(const QString& account, const QByteArray& key);
Result<QByteArray> read(const QString& account);
Result<void> remove(const QString& account);
} // namespace fincept::mac_keychain
