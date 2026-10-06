#include "services/portfolio/PortfolioTransfer.h"

#include "storage/repositories/PortfolioRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sync/SyncOutbox.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fincept::portfolio {
namespace {
class TransferTransaction {
  public:
    ~TransferTransaction() {
        if (active_)
            Database::instance().rollback();
    }
    Result<void> begin() {
        auto r = Database::instance().begin_transaction();
        active_ = r.is_ok();
        return r;
    }
    Result<void> commit() {
        auto r = Database::instance().commit();
        if (r.is_ok())
            active_ = false;
        return r;
    }

  private:
    bool active_ = false;
};
QString symbol_of(const QJsonObject& row) {
    return row.value("symbol").toString().trimmed().toUpper();
}
bool number(const QJsonObject& row, const char* key, bool positive) {
    const auto v = row.value(QLatin1String(key));
    return v.isDouble() && std::isfinite(v.toDouble()) && (positive ? v.toDouble() > 0 : v.toDouble() >= 0);
}
bool valid_date(const QString& date) {
    return (date.size() == 10 && QDate::fromString(date, Qt::ISODate).isValid()) ||
           QDateTime::fromString(date, Qt::ISODate).isValid() ||
           QDateTime::fromString(date, "yyyy-MM-dd HH:mm:ss").isValid();
}
} // namespace

Result<QJsonObject> export_portfolio(const QString& id) {
    TransferTransaction transaction;
    auto started = transaction.begin();
    if (started.is_err())
        return Result<QJsonObject>::err(started.error());
    auto& repo = PortfolioRepository::instance();
    auto portfolio = repo.get_portfolio(id);
    auto assets = repo.get_assets(id);
    auto history = repo.get_transactions(id, std::numeric_limits<int>::max());
    if (portfolio.is_err())
        return Result<QJsonObject>::err(portfolio.error());
    if (assets.is_err())
        return Result<QJsonObject>::err(assets.error());
    if (history.is_err())
        return Result<QJsonObject>::err(history.error());
    const auto& p = portfolio.value();
    QJsonObject root{{"format_version", "2.0"},
                     {"portfolio_name", p.name},
                     {"owner", p.owner},
                     {"currency", p.currency},
                     {"description", p.description},
                     {"export_date", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    QJsonArray holdings;
    for (const auto& a : assets.value())
        holdings.append(QJsonObject{{"symbol", a.symbol},
                                    {"quantity", a.quantity},
                                    {"avg_buy_price", a.avg_buy_price},
                                    {"first_purchase_date", a.first_purchase_date},
                                    {"sector", a.sector},
                                    {"broker_symbol", a.broker_symbol},
                                    {"exchange", a.exchange}});
    root["holdings"] = holdings;
    QJsonArray transactions;
    for (const auto& t : history.value())
        transactions.append(QJsonObject{{"date", t.transaction_date},
                                        {"symbol", t.symbol},
                                        {"type", t.transaction_type},
                                        {"quantity", t.quantity},
                                        {"price", t.price},
                                        {"total_value", t.total_value},
                                        {"notes", t.notes}});
    root["transactions"] = transactions;
    auto committed = transaction.commit();
    if (committed.is_err())
        return Result<QJsonObject>::err(committed.error());
    return Result<QJsonObject>::ok(root);
}

Result<ImportResult> import_portfolio(const QJsonObject& root, ImportMode mode, const QString& merge_target) {
    const auto fail = [](const QString& message) { return Result<ImportResult>::err(message.toStdString()); };
    if (root.contains("format_version") && !root.value("format_version").isString())
        return fail("Invalid portfolio format version");
    const QString version = root.value("format_version").toString("1.0");
    const bool snapshot = version == "2.0";
    if (!snapshot && version != "1.0")
        return fail("Unsupported portfolio format version");
    const QString name = root.value("portfolio_name").toString().trimmed();
    if (name.isEmpty() || !root.value("transactions").isArray())
        return fail("Expected portfolio_name and a transactions array");
    const QString currency = root.value("currency").toString("USD").trimmed().toUpper();
    if (currency.size() != 3 || std::any_of(currency.cbegin(), currency.cend(),
                                            [](QChar c) { return c < QLatin1Char('A') || c > QLatin1Char('Z'); }))
        return fail("Currency must be a three-letter code");
    if (snapshot && !root.value("holdings").isArray())
        return fail("Version 2 requires holdings");
    const auto holdings = root.value("holdings").toArray();
    const auto transactions = root.value("transactions").toArray();
    if (!snapshot && transactions.isEmpty())
        return fail("Version 1 requires transaction history");
    QHash<QString, QString> sector_hints;
    for (const auto& value : holdings) {
        const auto row = value.toObject();
        if (!row.value("sector").toString().isEmpty())
            sector_hints.insert(symbol_of(row), row.value("sector").toString());
    }
    QSet<QString> symbols;
    if (snapshot) {
        for (const auto& value : holdings) {
            const auto row = value.toObject();
            const auto symbol = symbol_of(row);
            if (!value.isObject() || symbol.isEmpty() || symbols.contains(symbol) || !number(row, "quantity", false) ||
                !number(row, "avg_buy_price", false) ||
                !std::isfinite(row.value("quantity").toDouble() * row.value("avg_buy_price").toDouble()) ||
                !valid_date(row.value("first_purchase_date").toString()))
                return fail("Invalid or duplicate holding");
            symbols.insert(symbol);
        }
    }
    QVector<QJsonObject> ordered;
    for (const auto& value : transactions) {
        const auto row = value.toObject();
        const auto type = row.value("type").toString();
        if (!value.isObject() || symbol_of(row).isEmpty() || !valid_date(row.value("date").toString()) ||
            !number(row, "quantity", type != "DIVIDEND") || !number(row, "price", false) ||
            !QStringList{"BUY", "SELL", "DIVIDEND", "SPLIT"}.contains(type))
            return fail("Invalid transaction");
        if (!snapshot && type == "SPLIT")
            return fail("Version 1 split replay is unsupported; import a version 2 snapshot");
        if (!std::isfinite(row.value("quantity").toDouble() * row.value("price").toDouble()))
            return fail("Transaction amount exceeds the supported range");
        ordered.append(row);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        const auto da = a.value("date").toString();
        const auto db = b.value("date").toString();
        if (da.left(10) != db.left(10))
            return da.left(10) < db.left(10);
        return a.value("type").toString() == "BUY" && b.value("type").toString() != "BUY";
    });
    auto& repo = PortfolioRepository::instance();
    TransferTransaction transaction;
    auto started = transaction.begin();
    if (started.is_err())
        return Result<ImportResult>::err(started.error());
    QString target = merge_target;
    ImportResult result;
    {
        // No queue notifications may escape before a successful commit.
        SyncOutbox::ApplyGuard guard;
        if (mode == ImportMode::New) {
            auto created = repo.create_portfolio(name, root.value("owner").toString(""), currency,
                                                 root.value("description").toString(""));
            if (created.is_err())
                return Result<ImportResult>::err(created.error());
            target = created.value();
            result.portfolio_name = name;
        } else {
            auto existing = repo.get_portfolio(target);
            if (existing.is_err())
                return fail("Merge target does not exist");
            if (existing.value().currency.toUpper() != currency)
                return fail("Cannot merge different currencies");
            result.portfolio_name = existing.value().name;
        }
        if (snapshot) {
            for (const auto& value : holdings) {
                const auto row = value.toObject();
                auto added = repo.add_asset(target, symbol_of(row), row.value("quantity").toDouble(),
                                            row.value("avg_buy_price").toDouble(),
                                            row.value("first_purchase_date").toString(), row.value("sector").toString(),
                                            row.value("broker_symbol").toString(), row.value("exchange").toString());
                if (added.is_err())
                    return Result<ImportResult>::err(added.error());
            }
        }
        for (const auto& row : ordered) {
            const auto symbol = symbol_of(row);
            const auto type = row.value("type").toString();
            const double qty = row.value("quantity").toDouble();
            const double price = row.value("price").toDouble();
            if (!snapshot && type == "BUY") {
                auto added = repo.add_asset(target, symbol, qty, price, row.value("date").toString(),
                                            row.value("sector").toString(sector_hints.value(symbol)));
                if (added.is_err())
                    return Result<ImportResult>::err(added.error());
            } else if (!snapshot && type == "SELL") {
                auto assets = repo.get_assets(target);
                if (assets.is_err())
                    return Result<ImportResult>::err(assets.error());
                const auto it = std::find_if(assets.value().cbegin(), assets.value().cend(),
                                             [&symbol](const auto& a) { return a.symbol == symbol; });
                if (it == assets.value().cend())
                    return fail("SELL has no holding: " + symbol);
                const double tolerance = 8 * std::numeric_limits<double>::epsilon() * std::max(qty, it->quantity);
                if (qty > it->quantity + tolerance)
                    return fail("SELL exceeds held quantity: " + symbol);
                const double remaining = it->quantity - qty;
                auto changed = remaining <= tolerance ? repo.remove_asset(target, symbol)
                                                      : repo.update_asset(target, symbol, remaining, it->avg_buy_price);
                if (changed.is_err())
                    return Result<ImportResult>::err(changed.error());
            }
            auto recorded = repo.add_transaction(target, symbol, type, qty, price, row.value("date").toString(),
                                                 row.value("notes").toString());
            if (recorded.is_err())
                return Result<ImportResult>::err(recorded.error());
            ++result.transactions_replayed;
        }
    }
    auto final_assets = repo.get_assets(target);
    if (final_assets.is_err())
        return Result<ImportResult>::err(final_assets.error());
    double total_cost = 0;
    for (const auto& asset : final_assets.value()) {
        total_cost += asset.quantity * asset.avg_buy_price;
        if (!std::isfinite(asset.quantity) || !std::isfinite(asset.avg_buy_price) || !std::isfinite(total_cost))
            return fail("Holding amounts exceed the supported range");
    }
    auto committed = transaction.commit();
    if (committed.is_err())
        return Result<ImportResult>::err(committed.error());
    SyncOutbox::record_unique("portfolio", target, "sync");
    result.portfolio_id = target;
    return Result<ImportResult>::ok(result);
}
} // namespace fincept::portfolio
