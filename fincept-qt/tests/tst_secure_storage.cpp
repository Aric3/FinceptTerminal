#include "storage/secure/MacKeychain.h"
#include "storage/secure/SecureStorage.h"
#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <openssl/evp.h>

namespace {
QHash<QString, QByteArray> fake_keys;
bool deny_add = false, deny_read = false;
int additions = 0;
QByteArray encrypt(const QByteArray& key, const QByteArray& iv, const QByteArray& plain, QByteArray& tag) {
    auto* ctx = EVP_CIPHER_CTX_new();
    QByteArray result(plain.size() + 16, '\0');
    int count = 0, last = 0;
    bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.constData()),
                                 reinterpret_cast<const unsigned char*>(iv.constData())) == 1 &&
              EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(result.data()), &count,
                                reinterpret_cast<const unsigned char*>(plain.constData()), plain.size()) == 1 &&
              EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(result.data()) + count, &last) == 1;
    tag.resize(16);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        qFatal("Fixture encryption failed");
    result.resize(count + last);
    return result;
}
QByteArray seed() {
    return QSysInfo::machineUniqueId() + '\x1f' + QSysInfo::productType().toUtf8() + '\x1f' +
           QByteArray("fincept-terminal/secure-storage/v1");
}
void insert_row(const QString& name, const QByteArray& ct, const QByteArray& iv, const QByteArray& tag) {
    auto result = fincept::Database::instance().execute(
        "INSERT INTO secure_credentials(key,ciphertext,iv,tag) VALUES(?,?,?,?)", {name, ct, iv, tag});
    if (result.is_err())
        qFatal("Fixture insert failed");
}
void old_store(bool wrapped = true, bool corrupt = false) {
    const QByteArray dek(32, 'D'), salt(16, 'S'), iv(12, 'I');
    QByteArray row_key = QCryptographicHash::hash(seed(), QCryptographicHash::Sha256);
    if (wrapped) {
        QByteArray kek(32, '\0'), tag;
        const auto material = seed();
        if (PKCS5_PBKDF2_HMAC(material.constData(), material.size(),
                              reinterpret_cast<const unsigned char*>(salt.constData()), salt.size(), 200000,
                              EVP_sha256(), 32, reinterpret_cast<unsigned char*>(kek.data())) != 1)
            qFatal("Fixture KDF failed");
        auto ct = encrypt(kek, iv, dek, tag);
        if (corrupt)
            tag[0] ^= 1;
        insert_row("__fincept_dek_v2_pbkdf2", salt + ct, iv, tag);
        row_key = dek;
    }
    QByteArray tag;
    const auto ct = encrypt(row_key, iv, "fake-api-key", tag);
    if (!wrapped && corrupt)
        tag[0] ^= 1;
    insert_row("sample", ct, iv, tag);
}
void close_database() {
    auto& db = fincept::Database::instance();
    db.close();
    db.raw_db() = QSqlDatabase();
    QSqlDatabase::removeDatabase("fincept_main");
}
int rows(const QString& name) {
    auto result = fincept::Database::instance().execute("SELECT count(*) FROM secure_credentials WHERE key=?", {name});
    if (result.is_err() || !result.value().next())
        return -1;
    return result.value().value(0).toInt();
}
QByteArray ciphertext(const QString& name) {
    auto result =
        fincept::Database::instance().execute("SELECT ciphertext FROM secure_credentials WHERE key=?", {name});
    if (result.is_err() || !result.value().next())
        return {};
    return result.value().value(0).toByteArray();
}
} // namespace
namespace fincept::mac_keychain {
Result<void> add(const QString& account, const QByteArray& key) {
    ++additions;
    if (deny_add || fake_keys.contains(account))
        return Result<void>::err("Fake Keychain denial");
    fake_keys.insert(account, key);
    return Result<void>::ok();
}
Result<QByteArray> read(const QString& account) {
    if (deny_read || !fake_keys.contains(account))
        return Result<QByteArray>::err("Fake Keychain locked/missing");
    return Result<QByteArray>::ok(fake_keys.value(account));
}
Result<void> remove(const QString& account) {
    fake_keys.remove(account);
    return Result<void>::ok();
}
} // namespace fincept::mac_keychain

class SecureStorageTest : public QObject {
    Q_OBJECT
    QTemporaryDir temp;
    int profile = 0;
    fincept::SecureStorage& storage = fincept::SecureStorage::instance();
  private slots:
    void init() {
        deny_add = deny_read = false;
        additions = 0;
        fake_keys.clear();
        QVERIFY(temp.isValid());
        auto& db = fincept::Database::instance();
        db.close();
        QVERIFY(db.open(temp.filePath(QString("profile-%1.db").arg(++profile))).is_ok());
        QVERIFY(db.exec("CREATE TABLE secure_credentials (key TEXT PRIMARY KEY, ciphertext BLOB NOT NULL, "
                        "iv BLOB NOT NULL, tag BLOB NOT NULL, updated_at TEXT)")
                    .is_ok());
    }
    void cleanup() { close_database(); }
    void new_store_roundtrip() {
        QVERIFY(storage.initialize().is_ok());
        QCOMPARE(rows("__fincept_dek_v2_keychain"), 1);
        QCOMPARE(rows("__fincept_dek_v2_pbkdf2"), 0);
        QVERIFY(storage.store("sample", "fake-secret").is_ok());
        QVERIFY(storage.initialize().is_ok()); // Simulate startup: reload from Keychain.
        QCOMPARE(storage.retrieve("sample").value(), QString("fake-secret"));
        QVERIFY(storage.store("empty", "").is_ok());
        QCOMPARE(storage.retrieve("empty").value(), QString(""));
        QVERIFY(storage.store("__fincept_dek_v2_keychain", "overwrite").is_err());
        QVERIFY(storage.remove("sample").is_ok());
        QVERIFY(storage.retrieve("sample").is_err());
    }
    void pbkdf_migration_keeps_ciphertext() {
        old_store();
        const auto before = ciphertext("sample");
        QVERIFY(storage.initialize().is_ok());
        QCOMPARE(ciphertext("sample"), before);
        QCOMPARE(rows("__fincept_dek_v2_pbkdf2"), 0);
        QCOMPARE(storage.retrieve("sample").value(), QString("fake-api-key"));
        QVERIFY(storage.initialize().is_ok());
        QCOMPARE(additions, 1);
    }
    void legacy_migration_roundtrip() {
        old_store(false);
        const auto before = ciphertext("sample");
        QVERIFY(storage.initialize().is_ok());
        QVERIFY(ciphertext("sample") != before);
        QCOMPARE(storage.retrieve("sample").value(), QString("fake-api-key"));
    }
    void denial_keeps_old_key_and_allows_retry() {
        old_store();
        const auto wrapper = ciphertext("__fincept_dek_v2_pbkdf2");
        deny_add = true;
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(ciphertext("__fincept_dek_v2_pbkdf2"), wrapper);
        QCOMPARE(rows("__fincept_dek_v2_keychain"), 0);
        deny_add = false;
        QVERIFY(storage.initialize().is_ok());
        QCOMPARE(storage.retrieve("sample").value(), QString("fake-api-key"));
    }
    void readback_denial_rolls_back() {
        old_store();
        deny_read = true;
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(rows("__fincept_dek_v2_pbkdf2"), 1);
        QCOMPARE(rows("__fincept_dek_v2_keychain"), 0);
        QVERIFY(fake_keys.isEmpty());
    }
    void db_failure_rolls_back() {
        old_store();
        const auto before = ciphertext("sample");
        QVERIFY(fincept::Database::instance()
                    .exec("CREATE TRIGGER reject_marker BEFORE INSERT ON secure_credentials "
                          "WHEN new.key='__fincept_dek_v2_keychain' BEGIN SELECT RAISE(ABORT,'test failure'); END")
                    .is_ok());
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(ciphertext("sample"), before);
        QCOMPARE(rows("__fincept_dek_v2_pbkdf2"), 1);
        QVERIFY(fake_keys.isEmpty());
    }
    void db_delete_failure_restores_wrapper() {
        old_store();
        const auto wrapper = ciphertext("__fincept_dek_v2_pbkdf2");
        QVERIFY(fincept::Database::instance()
                    .exec("CREATE TRIGGER reject_delete BEFORE DELETE ON secure_credentials "
                          "WHEN old.key='__fincept_dek_v2_pbkdf2' BEGIN SELECT RAISE(ABORT,'test failure'); END")
                    .is_ok());
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(ciphertext("__fincept_dek_v2_pbkdf2"), wrapper);
        QCOMPARE(rows("__fincept_dek_v2_keychain"), 0);
        QVERIFY(fake_keys.isEmpty());
    }
    void new_store_denial_does_not_create_machine_wrapper() {
        deny_add = true;
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(rows("__fincept_dek_v2_pbkdf2"), 0);
        QCOMPARE(rows("__fincept_dek_v2_keychain"), 0);
        QVERIFY(storage.store("sample", "dummy").is_err());
        deny_add = false;
        QVERIFY(storage.initialize().is_ok());
    }
    void corrupt_wrapped_key_is_never_replaced() {
        old_store(true, true);
        const auto before = ciphertext("__fincept_dek_v2_pbkdf2");
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(ciphertext("__fincept_dek_v2_pbkdf2"), before);
        QCOMPARE(additions, 0);
    }
    void corrupt_legacy_row_aborts_migration() {
        old_store(false, true);
        const auto before = ciphertext("sample");
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(ciphertext("sample"), before);
        QCOMPARE(rows("__fincept_dek_v2_keychain"), 0);
        QVERIFY(fake_keys.isEmpty());
    }
    void missing_or_locked_keychain_never_falls_back() {
        QVERIFY(storage.initialize().is_ok());
        QVERIFY(storage.store("sample", "dummy").is_ok());
        const auto marker = ciphertext("__fincept_dek_v2_keychain");
        deny_read = true;
        QVERIFY(storage.initialize().is_err());
        QVERIFY(storage.retrieve("sample").is_err());
        QCOMPARE(ciphertext("__fincept_dek_v2_keychain"), marker);
        QCOMPARE(additions, 1);
        deny_read = false;
        fake_keys.clear();
        QVERIFY(storage.initialize().is_err());
        QCOMPARE(additions, 1);
    }
    void profiles_have_distinct_keys() {
        QVERIFY(storage.initialize().is_ok());
        const auto first = ciphertext("__fincept_dek_v2_keychain");
        QVERIFY(storage.store("sample", "profile-one").is_ok());
        auto& db = fincept::Database::instance();
        close_database();
        QVERIFY(db.open(temp.filePath("second.db")).is_ok());
        QVERIFY(db.exec("CREATE TABLE secure_credentials (key TEXT PRIMARY KEY, ciphertext BLOB NOT NULL, "
                        "iv BLOB NOT NULL, tag BLOB NOT NULL, updated_at TEXT)")
                    .is_ok());
        QVERIFY(storage.initialize().is_ok());
        QVERIFY(ciphertext("__fincept_dek_v2_keychain") != first);
        QVERIFY(storage.retrieve("sample").is_err());
        QCOMPARE(fake_keys.size(), 2);
    }
};
QTEST_GUILESS_MAIN(SecureStorageTest)
#include "tst_secure_storage.moc"
