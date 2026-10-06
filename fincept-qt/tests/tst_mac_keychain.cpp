#include "storage/secure/MacKeychain.h"
#include "storage/secure/SecureStorage.h"
#include "storage/sqlite/Database.h"

#include <QTemporaryDir>
#include <QtTest>

// Opt-in integration test: only a temporary DB and newly created dummy key.
// Never runs the application, accesses its profiles, or reads existing secrets.
class MacKeychainTest : public QObject {
    Q_OBJECT
  private slots:
    void temporary_database_roundtrip() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        auto& db = fincept::Database::instance();
        QVERIFY(db.open(temp.filePath("dummy.db")).is_ok());
        QVERIFY(db.exec("CREATE TABLE secure_credentials(key TEXT PRIMARY KEY, ciphertext BLOB NOT NULL, "
                        "iv BLOB NOT NULL, tag BLOB NOT NULL, updated_at TEXT)")
                    .is_ok());
        auto& store = fincept::SecureStorage::instance();
        const auto initialized = store.initialize();
        QVERIFY2(initialized.is_ok(), initialized.is_err() ? initialized.error().c_str() : "");

        QString account;
        {
            auto row = db.execute("SELECT ciphertext FROM secure_credentials WHERE key='__fincept_dek_v2_keychain'");
            QVERIFY(row.is_ok());
            QVERIFY(row.value().next());
            account = QString::fromUtf8(row.value().value(0).toByteArray());
        }
        QVERIFY(!account.isEmpty());
        struct Cleanup {
            QString account;
            bool removed = false;
            ~Cleanup() {
                if (!removed)
                    (void)fincept::mac_keychain::remove(account);
                fincept::Database::instance().close();
            }
        } cleanup{account};

        QVERIFY(store.store("test-only", "public-dummy-value").is_ok());
        QVERIFY(store.initialize().is_ok()); // Reload DEK through the real OS API.
        const auto value = store.retrieve("test-only");
        QVERIFY(value.is_ok());
        QCOMPARE(value.value(), QString("public-dummy-value"));
        QVERIFY(fincept::mac_keychain::add(account, QByteArray(32, 'X')).is_err());
        QVERIFY(store.initialize().is_ok());
        QCOMPARE(store.retrieve("test-only").value(), QString("public-dummy-value"));
        QVERIFY(fincept::mac_keychain::remove(account).is_ok());
        cleanup.removed = true;
        QVERIFY(store.initialize().is_err()); // Missing OS key must fail closed.
    }
};
QTEST_GUILESS_MAIN(MacKeychainTest)
#include "tst_mac_keychain.moc"
