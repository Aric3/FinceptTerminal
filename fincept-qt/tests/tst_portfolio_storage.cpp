#include "storage/repositories/PortfolioRepository.h"
#include "storage/repositories/WatchlistRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sync/CloudSyncSettings.h"

#include <QDate>
#include <QTemporaryDir>
#include <QtTest>

namespace fincept {
void register_migration_v001();
void register_migration_v006();
void register_migration_v019();
void register_migration_v022();
} // namespace fincept

class PortfolioStorageTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;
    QString path_;
    int fixture_ = 0;

  private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        fincept::register_migration_v001();
        fincept::register_migration_v006();
        fincept::register_migration_v019();
        fincept::register_migration_v022();
    }
    void init() {
        path_ = directory_.filePath(QString("fixture-%1.db").arg(++fixture_));
        QVERIFY(fincept::Database::instance().open(path_).is_ok());
        fincept::CloudSyncSettings::set_master_enabled(false);
    }
    void cleanup() { fincept::Database::instance().close(); }

    void holdings_merge_and_persist() {
        auto& repo = fincept::PortfolioRepository::instance();
        auto created = repo.create_portfolio("Acceptance", "Fixture", "USD", "Local only", "fixture-broker");
        QVERIFY(created.is_ok());
        const auto id = created.value();
        auto first = repo.add_asset(id, "aapl", 10, 100, "2026-01-02", "Technology", "AAPL", "NASDAQ");
        QVERIFY(first.is_ok());
        auto second = repo.add_asset(id, "AAPL", 5, 160);
        QVERIFY(second.is_ok());
        QCOMPARE(second.value(), first.value());
        auto assets = repo.get_assets(id);
        QVERIFY(assets.is_ok());
        QCOMPARE(assets.value().size(), 1);
        QCOMPARE(assets.value().first().quantity, 15.0);
        QCOMPARE(assets.value().first().avg_buy_price, 120.0);
        QCOMPARE(assets.value().first().sector, QString("Technology"));
        QCOMPARE(assets.value().first().broker_symbol, QString("AAPL"));
        QCOMPARE(assets.value().first().exchange, QString("NASDAQ"));
        QCOMPARE(assets.value().first().first_purchase_date, QString("2026-01-02"));
        QVERIFY(repo.update_portfolio(id, "Renamed", "Fixture", "USD", "Updated").is_ok());
        fincept::Database::instance().close();
        QVERIFY(fincept::Database::instance().open(path_).is_ok());
        auto saved = repo.get_portfolio(id);
        QVERIFY(saved.is_ok());
        QCOMPARE(saved.value().name, QString("Renamed"));
        QCOMPARE(saved.value().broker_account_id, QString("fixture-broker"));
        auto reloaded = repo.get_assets(id);
        QVERIFY(reloaded.is_ok());
        QCOMPARE(reloaded.value().size(), 1);
        QCOMPARE(reloaded.value().first().avg_buy_price, 120.0);
    }

    void transactions_snapshots_and_cascade() {
        auto& repo = fincept::PortfolioRepository::instance();
        auto created = repo.create_portfolio("Cascade", "Fixture", "USD");
        QVERIFY(created.is_ok());
        const auto id = created.value();
        QVERIFY(repo.add_asset(id, "AAPL", 2, 100).is_ok());
        auto transaction = repo.add_transaction(id, "aapl", "BUY", 2, 100, "2026-01-02");
        QVERIFY(transaction.is_ok());
        QVERIFY(repo.update_transaction(transaction.value(), 3, 120, "2026-01-03", "Edited").is_ok());
        auto transactions = repo.get_symbol_transactions(id, "aapl");
        QVERIFY(transactions.is_ok());
        QCOMPARE(transactions.value().size(), 1);
        QCOMPARE(transactions.value().first().total_value, 360.0);
        QVERIFY(repo.add_transaction(id, "AAPL", "INVALID", 2, 100, "2026-01-02").is_err());
        const auto today = QDate::currentDate().toString(Qt::ISODate);
        QVERIFY(repo.save_snapshot(id, 240, 200, 40, 20, today).is_ok());
        QVERIFY(repo.save_snapshot(id, 260, 200, 60, 30, today).is_ok());
        auto snapshots = repo.get_snapshots(id);
        QVERIFY(snapshots.is_ok());
        QCOMPARE(snapshots.value().size(), 1);
        QCOMPARE(snapshots.value().first().total_value, 260.0);
        QVERIFY(repo.delete_portfolio(id).is_ok());
        QVERIFY(repo.get_portfolio(id).is_err());
        auto assets = repo.get_assets(id);
        auto remaining = repo.get_transactions(id);
        auto history = repo.get_snapshots(id);
        QVERIFY(assets.is_ok());
        QVERIFY(remaining.is_ok());
        QVERIFY(history.is_ok());
        QVERIFY(assets.value().isEmpty());
        QVERIFY(remaining.value().isEmpty());
        QVERIFY(history.value().isEmpty());
        QVERIFY(repo.add_asset(id, "MSFT", 1, 100).is_err());
    }

    void watchlist_duplicates_persistence_and_cascade() {
        auto& repo = fincept::WatchlistRepository::instance();
        auto created = repo.create("Fixture");
        QVERIFY(created.is_ok());
        auto watchlist = created.value();
        QVERIFY(repo.add_stock(watchlist.id, "aapl", "Apple", "NASDAQ").is_ok());
        QVERIFY(repo.add_stock(watchlist.id, "AAPL").is_ok());
        auto stocks = repo.get_stocks(watchlist.id);
        QVERIFY(stocks.is_ok());
        QCOMPARE(stocks.value().size(), 1);
        QCOMPARE(stocks.value().first().symbol, QString("AAPL"));
        watchlist.name = "Renamed";
        QVERIFY(repo.update(watchlist).is_ok());
        fincept::Database::instance().close();
        QVERIFY(fincept::Database::instance().open(path_).is_ok());
        auto reloaded = repo.get(watchlist.id);
        QVERIFY(reloaded.is_ok());
        QCOMPARE(reloaded.value().name, QString("Renamed"));
        auto persisted = repo.get_stocks(watchlist.id);
        QVERIFY(persisted.is_ok());
        QCOMPARE(persisted.value().size(), 1);
        QVERIFY(repo.remove(watchlist.id).is_ok());
        auto remaining = repo.get_stocks(watchlist.id);
        QVERIFY(remaining.is_ok());
        QVERIFY(remaining.value().isEmpty());
        QVERIFY(repo.add_stock(watchlist.id, "MSFT").is_err());
    }
};

QTEST_GUILESS_MAIN(PortfolioStorageTest)
#include "tst_portfolio_storage.moc"
