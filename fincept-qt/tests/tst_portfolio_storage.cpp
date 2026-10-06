#include "services/portfolio/PortfolioTransfer.h"
#include "storage/repositories/PortfolioRepository.h"
#include "storage/repositories/WatchlistRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sync/CloudSyncSettings.h"

#include <QDate>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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

    void snapshot_round_trip() {
        using namespace fincept::portfolio;
        auto& repo = fincept::PortfolioRepository::instance();
        auto source = repo.create_portfolio("手动组合", "Fixture", "USD", "No synthesized trades", "private-broker");
        QVERIFY(source.is_ok());
        QVERIFY(repo.add_asset(source.value(), "AAPL", 7.125, 123.456789, "2026-01-02", "Technology", "AAPL", "NASDAQ")
                    .is_ok());
        QVERIFY(repo.add_asset(source.value(), "MSFT", 0, 0, "2026-01-03").is_ok());
        QVERIFY(
            repo.add_transaction(source.value(), "AAPL", "BUY", 10, 100, "2026-01-02", "before manual edit").is_ok());
        QVERIFY(repo.add_transaction(source.value(), "AAPL", "SELL", 2, 120, "2026-01-03").is_ok());
        QVERIFY(repo.add_transaction(source.value(), "AAPL", "DIVIDEND", 8, 1, "2026-01-04", "cash dividend").is_ok());
        QVERIFY(repo.add_transaction(source.value(), "AAPL", "SPLIT", 2, 0, "2026-01-05", "history only").is_ok());
        auto exported = export_portfolio(source.value());
        QVERIFY(exported.is_ok());
        QCOMPARE(exported.value()["format_version"].toString(), QString("2.0"));
        QVERIFY(!exported.value().contains("broker_account_id"));
        QFile file(directory_.filePath("round-trip.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(exported.value()).toJson();
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto imported = import_portfolio(QJsonDocument::fromJson(file.readAll()).object(), ImportMode::New);
        QVERIFY2(imported.is_ok(), imported.is_err() ? imported.error().c_str() : "");
        QCOMPARE(imported.value().transactions_replayed, 4);
        QVERIFY(imported.value().portfolio_id != source.value());
        auto restored = export_portfolio(imported.value().portfolio_id);
        QVERIFY(restored.is_ok());
        QCOMPARE(restored.value()["holdings"], exported.value()["holdings"]);
        QCOMPARE(restored.value()["transactions"], exported.value()["transactions"]);
        QCOMPARE(restored.value()["description"], exported.value()["description"]);
        QCOMPARE(repo.get_portfolio(imported.value().portfolio_id).value().broker_account_id, QString());
    }

    void empty_and_holdings_only_round_trip() {
        using namespace fincept::portfolio;
        auto& repo = fincept::PortfolioRepository::instance();
        auto id = repo.create_portfolio("Empty", "Fixture", "USD");
        QVERIFY(id.is_ok());
        auto empty = export_portfolio(id.value());
        QVERIFY(empty.is_ok());
        QVERIFY(import_portfolio(empty.value(), ImportMode::New).is_ok());
        QVERIFY(repo.add_asset(id.value(), "AAPL", 0.125, 150).is_ok());
        auto snapshot = export_portfolio(id.value());
        QVERIFY(snapshot.is_ok());
        auto restored = import_portfolio(snapshot.value(), ImportMode::New);
        QVERIFY(restored.is_ok());
        QCOMPARE(repo.get_assets(restored.value().portfolio_id).value().first().quantity, 0.125);
        QVERIFY(repo.get_transactions(restored.value().portfolio_id).value().isEmpty());
        QVERIFY(export_portfolio("missing").is_err());
    }

    void legacy_replay_and_additive_merge() {
        using namespace fincept::portfolio;
        auto& repo = fincept::PortfolioRepository::instance();
        QJsonObject buy{
            {"date", "2026-01-02"}, {"symbol", " aapl "}, {"type", "BUY"}, {"quantity", 10}, {"price", 100}};
        QJsonObject sell{{"date", "2026-01-03"}, {"symbol", "AAPL"}, {"type", "SELL"}, {"quantity", 4}, {"price", 120}};
        QJsonObject root{{"portfolio_name", "Legacy"},
                         {"transactions", QJsonArray{sell, buy}},
                         {"holdings", QJsonArray{QJsonObject{{"symbol", "AAPL"}, {"sector", "Technology"}}}}};
        auto imported = import_portfolio(root, ImportMode::New);
        QVERIFY2(imported.is_ok(), imported.is_err() ? imported.error().c_str() : "");
        const auto id = imported.value().portfolio_id;
        auto assets = repo.get_assets(id);
        QCOMPARE(assets.value().first().quantity, 6.0);
        QCOMPARE(assets.value().first().avg_buy_price, 100.0);
        QCOMPARE(assets.value().first().sector, QString("Technology"));
        auto snapshot = export_portfolio(id);
        QVERIFY(snapshot.is_ok());
        auto target = repo.create_portfolio("Target", "Fixture", "USD");
        QVERIFY(target.is_ok());
        QVERIFY(repo.add_asset(target.value(), "AAPL", 4, 150).is_ok());
        QVERIFY(import_portfolio(snapshot.value(), ImportMode::Merge, target.value()).is_ok());
        auto combined = repo.get_assets(target.value());
        QCOMPARE(combined.value().first().quantity, 10.0);
        QCOMPARE(combined.value().first().avg_buy_price, 120.0);
        QCOMPARE(repo.get_transactions(target.value()).value().size(), 2);
        snapshot.value()["currency"] = "EUR";
        QVERIFY(import_portfolio(snapshot.value(), ImportMode::Merge, target.value()).is_err());
        QCOMPARE(repo.get_assets(target.value()).value().first().quantity, 10.0);
    }

    void fractional_legacy_sell_preserves_small_balance() {
        using namespace fincept::portfolio;
        auto& repo = fincept::PortfolioRepository::instance();
        QJsonObject buy{
            {"date", "2026-01-02"}, {"symbol", "BTC-USD"}, {"type", "BUY"}, {"quantity", 2e-10}, {"price", 100}};
        QJsonObject sell{
            {"date", "2026-01-02"}, {"symbol", "BTC-USD"}, {"type", "SELL"}, {"quantity", 1e-10}, {"price", 120}};
        QJsonObject root{{"portfolio_name", "Fractional"}, {"transactions", QJsonArray{sell, buy}}};
        auto imported = import_portfolio(root, ImportMode::New);
        QVERIFY(imported.is_ok());
        QCOMPARE(repo.get_assets(imported.value().portfolio_id).value().first().quantity, 1e-10);
        sell["quantity"] = 1.1e-10;
        root["transactions"] = QJsonArray{sell};
        QVERIFY(import_portfolio(root, ImportMode::Merge, imported.value().portfolio_id).is_err());
        QCOMPARE(repo.get_assets(imported.value().portfolio_id).value().first().quantity, 1e-10);
    }

    void import_failure_rolls_back_new_and_merge() {
        using namespace fincept::portfolio;
        auto& repo = fincept::PortfolioRepository::instance();
        auto source = repo.create_portfolio("Source", "Fixture", "USD");
        QVERIFY(source.is_ok());
        QVERIFY(repo.add_asset(source.value(), "AAPL", 2, 100).is_ok());
        QVERIFY(repo.add_transaction(source.value(), "AAPL", "BUY", 2, 100, "2026-01-02").is_ok());
        auto exported = export_portfolio(source.value());
        QVERIFY(exported.is_ok());
        const auto before = repo.list_portfolios().value().size();
        // Fail AFTER portfolio/holdings writes, using real SQLite constraints.
        QVERIFY(fincept::Database::instance()
                    .exec("CREATE TRIGGER reject_import BEFORE INSERT ON portfolio_transactions "
                          "BEGIN SELECT RAISE(ABORT, 'fixture failure'); END")
                    .is_ok());
        QVERIFY(import_portfolio(exported.value(), ImportMode::New).is_err());
        QCOMPARE(repo.list_portfolios().value().size(), before);
        QVERIFY(import_portfolio(exported.value(), ImportMode::Merge, source.value()).is_err());
        QCOMPARE(repo.get_assets(source.value()).value().first().quantity, 2.0);
        QCOMPARE(repo.get_transactions(source.value()).value().size(), 1);
        QVERIFY(fincept::Database::instance().exec("DROP TRIGGER reject_import").is_ok());
        // Also roll back a valid BUY followed by an impossible SELL.
        auto legacy = exported.value();
        legacy["format_version"] = "1.0";
        auto txns = legacy["transactions"].toArray();
        auto sell = txns.first().toObject();
        sell["type"] = "SELL";
        sell["quantity"] = 999;
        sell["date"] = "2026-01-03";
        txns.append(sell);
        legacy["transactions"] = txns;
        QVERIFY(import_portfolio(legacy, ImportMode::Merge, source.value()).is_err());
        QCOMPARE(repo.get_assets(source.value()).value().first().quantity, 2.0);
        QCOMPARE(repo.get_transactions(source.value()).value().size(), 1);
    }

    void invalid_documents_leave_database_unchanged() {
        using namespace fincept::portfolio;
        auto& repo = fincept::PortfolioRepository::instance();
        QJsonObject holding{
            {"symbol", "AAPL"}, {"quantity", 1}, {"avg_buy_price", 100}, {"first_purchase_date", "2026-01-02"}};
        QJsonObject valid{{"format_version", "2.0"},
                          {"portfolio_name", "Fixture"},
                          {"holdings", QJsonArray{holding}},
                          {"transactions", QJsonArray{}}};
        QVector<QJsonObject> invalid;
        auto bad = valid;
        bad["format_version"] = "3.0";
        invalid.append(bad);
        bad = valid;
        bad["format_version"] = 2;
        invalid.append(bad);
        bad = valid;
        bad["currency"] = "123";
        invalid.append(bad);
        bad = valid;
        bad["holdings"] = QJsonArray{holding, holding};
        invalid.append(bad);
        holding["quantity"] = -1;
        bad = valid;
        bad["holdings"] = QJsonArray{holding};
        invalid.append(bad);
        holding["quantity"] = 1;
        holding["first_purchase_date"] = "2026-02-30";
        bad = valid;
        bad["holdings"] = QJsonArray{holding};
        invalid.append(bad);
        holding["first_purchase_date"] = "2026-01-02";
        holding["quantity"] = 1e308;
        holding["avg_buy_price"] = 1e308;
        bad = valid;
        bad["holdings"] = QJsonArray{holding};
        invalid.append(bad);
        for (const auto& document : invalid) {
            QVERIFY(import_portfolio(document, ImportMode::New).is_err());
            QVERIFY(repo.list_portfolios().value().isEmpty());
        }
        QJsonObject split{{"date", "2026-01-02"}, {"symbol", "AAPL"}, {"type", "SPLIT"}, {"quantity", 2}, {"price", 0}};
        bad = valid;
        bad["format_version"] = "1.0";
        bad["transactions"] = QJsonArray{split};
        QVERIFY(import_portfolio(bad, ImportMode::New).is_err());
        QVERIFY(repo.list_portfolios().value().isEmpty());
    }

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
