#pragma once
#include "core/result/Result.h"

#include <QString>

namespace fincept {

/// Local credential store backed by SQLite + AES-256-GCM.
///
/// Each profile stores AES-256-GCM ciphertext with a random data key protected
/// by macOS login Keychain, Windows DPAPI, or machine-derived PBKDF2 elsewhere.
/// macOS stores only a random Keychain account ID in SQLite. Machine wrappers
/// migrate transactionally without rewriting credential ciphertext. Access
/// denial never falls back to weaker protection. The unwrapped key is cached
/// until exit; locking the OS does not evict an already loaded key. This does
/// not protect process memory from malware with the user's privileges.
class SecureStorage {
  public:
    static SecureStorage& instance();

    // Must succeed before authentication/PIN state is loaded at startup.
    Result<void> initialize();
    Result<void> store(const QString& key, const QString& value);
    Result<QString> retrieve(const QString& key);
    Result<void> remove(const QString& key);

  private:
    SecureStorage() = default;
};

} // namespace fincept
