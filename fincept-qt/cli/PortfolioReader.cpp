#include "PortfolioReader.h"

#include "services/portfolio/PortfolioValuation.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QSqlQuery>
#include <QUuid>

#include <cmath>

namespace fincept::cli {
namespace {
void execute(QSqlQuery& query) {
    if (!query.exec())
        throw Error("database_query_failed", "Portfolio query failed; database schema may be incompatible", 5);
}
QJsonObject as_json(const portfolio::Portfolio& p) {
    return {{"id", p.id}, {"name", p.name}, {"currency", p.currency}};
}
bool number(const QJsonObject& object, const QString& key, double* out) {
    const auto v = object.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        return false;
    *out = v.toDouble();
    return true;
}
} // namespace
PortfolioReader::PortfolioReader(const QString& path) {
    if (!QFileInfo(path).isFile())
        throw Error("database_unavailable", "Profile database does not exist; select an existing profile or --db", 3);
    db_ = QSqlDatabase::addDatabase("QSQLITE", QUuid::createUuid().toString());
    db_.setDatabaseName(path);
    db_.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1500");
    if (!db_.open()) {
        const auto name = db_.connectionName();
        db_ = {};
        QSqlDatabase::removeDatabase(name);
        throw Error("database_unavailable", "Cannot open portfolio database read-only", 5);
    }
    // One read transaction keeps portfolio metadata and holdings in the same snapshot.
    QSqlQuery query(db_);
    if (!query.exec("PRAGMA query_only=ON") || !query.exec("BEGIN")) {
        const auto name = db_.connectionName();
        query = QSqlQuery();
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name);
        throw Error("database_query_failed", "Cannot begin read-only portfolio snapshot", 5);
    }
}
PortfolioReader::~PortfolioReader() {
    const auto name = db_.connectionName();
    db_.rollback();
    db_.close();
    db_ = {};
    QSqlDatabase::removeDatabase(name);
}
QJsonObject PortfolioReader::list(int limit, int offset) const {
    QSqlQuery query(db_);
    query.prepare("SELECT id,name,currency FROM portfolios ORDER BY name,id LIMIT ? OFFSET ?");
    query.addBindValue(limit + 1);
    query.addBindValue(offset);
    execute(query);
    QJsonArray items;
    bool more = false;
    while (query.next()) {
        if (items.size() == limit) {
            more = true;
            break;
        }
        items.append(QJsonObject{{"id", query.value(0).toString()},
                                 {"name", query.value(1).toString()},
                                 {"currency", query.value(2).toString()}});
    }
    return {{"items", items},
            {"offset", offset},
            {"limit", limit},
            {"next_offset", more ? QJsonValue(offset + limit) : QJsonValue(QJsonValue::Null)}};
}
QString PortfolioReader::resolve(const QString& name) const {
    QSqlQuery query(db_);
    query.prepare("SELECT id FROM portfolios WHERE name=? ORDER BY id LIMIT 2");
    query.addBindValue(name);
    execute(query);
    if (!query.next())
        throw Error("not_found", "No portfolio with that exact name", 5);
    const auto id = query.value(0).toString();
    if (query.next())
        throw Error("ambiguous_name", "Multiple portfolios have that name; use an ID from portfolio list");
    return id;
}
portfolio::Portfolio PortfolioReader::portfolio(const QString& id) const {
    QSqlQuery query(db_);
    query.prepare("SELECT id,name,currency FROM portfolios WHERE id=?");
    query.addBindValue(id);
    execute(query);
    if (!query.next())
        throw Error("not_found", "Portfolio ID does not exist", 5);
    portfolio::Portfolio p;
    p.id = query.value(0).toString();
    p.name = query.value(1).toString();
    p.currency = query.value(2).toString();
    return p;
}
QVector<portfolio::PortfolioAsset> PortfolioReader::assets(const QString& id) const {
    QSqlQuery query(db_);
    query.prepare("SELECT symbol,quantity,avg_buy_price,COALESCE(sector,'') FROM portfolio_assets "
                  "WHERE portfolio_id=? ORDER BY symbol");
    query.addBindValue(id);
    execute(query);
    QVector<portfolio::PortfolioAsset> result;
    while (query.next()) {
        portfolio::PortfolioAsset asset;
        asset.symbol = query.value(0).toString();
        bool qty_ok, price_ok;
        asset.quantity = query.value(1).toDouble(&qty_ok);
        asset.avg_buy_price = query.value(2).toDouble(&price_ok);
        asset.sector = query.value(3).toString();
        if (!qty_ok || !price_ok || !std::isfinite(asset.quantity) || !std::isfinite(asset.avg_buy_price) ||
            asset.quantity < 0 || asset.avg_buy_price < 0 || !std::isfinite(asset.quantity * asset.avg_buy_price))
            throw Error("invalid_holdings", "Database contains invalid quantity or cost", 5);
        result.append(asset);
    }
    return result;
}
QJsonObject PortfolioReader::get(const QString& id) const {
    const auto p = portfolio(id);
    QJsonArray holdings;
    for (const auto& a : assets(id))
        holdings.append(QJsonObject{
            {"symbol", a.symbol}, {"quantity", a.quantity}, {"avg_buy_price", a.avg_buy_price}, {"sector", a.sector}});
    return {{"portfolio", as_json(p)}, {"holdings", holdings}};
}
QJsonObject PortfolioReader::value(const QString& id, const QString& prices_file, bool live,
                                   const Runtime& runtime) const {
    const auto p = portfolio(id);
    const auto holdings = assets(id);
    QSqlQuery snapshot(db_);
    if (!snapshot.exec("ROLLBACK"))
        throw Error("database_query_failed", "Cannot release read snapshot", 5);
    // No database read lock is held while waiting for network/price input.
    bool day_change_complete = true;
    QHash<QString, portfolio::PriceMark> marks;
    QJsonArray warnings;
    if (!live) {
        const auto input = read_json(prices_file).toObject();
        if (input.value("currency").toString() != p.currency || !input.value("prices").isObject())
            throw Error("invalid_prices", "Price file requires matching currency and a prices object");
        const auto prices = input.value("prices").toObject();
        for (auto it = prices.begin(); it != prices.end(); ++it) {
            const auto object = it.value().toObject();
            portfolio::PriceMark mark;
            if (!number(object, "price", &mark.price) || mark.price <= 0 ||
                (object.contains("change") && !number(object, "change", &mark.change)) ||
                (object.contains("change_percent") && !number(object, "change_percent", &mark.change_percent)))
                throw Error("invalid_prices",
                            "Each price requires finite positive price and finite optional change fields");
            marks.insert(it.key(), mark);
            if (!object.contains("change") || !object.contains("change_percent")) {
                for (const auto& holding : holdings)
                    if (holding.symbol == it.key())
                        day_change_complete = false;
            }
        }
    } else {
        for (const auto& a : holdings) {
            try {
                const auto quote = runtime.read_yahoo({"quote", a.symbol}).toObject();
                portfolio::PriceMark mark;
                if (quote.value("currency").toString() != p.currency)
                    throw Error("currency_mismatch", "Quote currency is missing or differs from portfolio currency");
                if (!number(quote, "price", &mark.price) || mark.price <= 0 || !number(quote, "change", &mark.change) ||
                    !number(quote, "change_percent", &mark.change_percent))
                    throw Error("quote_incomplete", "Quote or day change is unavailable");
                marks.insert(a.symbol, mark);
            } catch (const Error& e) {
                if (e.status == 6)
                    throw; // Total time budget failure cannot be reported as a successful valuation.
                warnings.append(QJsonObject{{"symbol", a.symbol}, {"code", e.code}});
            }
        }
    }
    const auto summary = portfolio::value_portfolio(p, holdings, marks);
    if (!std::isfinite(summary.total_market_value) || !std::isfinite(summary.total_cost_basis) ||
        !std::isfinite(summary.total_unrealized_pnl) || !std::isfinite(summary.total_unrealized_pnl_percent) ||
        !std::isfinite(summary.total_day_change) || !std::isfinite(summary.total_day_change_percent))
        throw Error("valuation_overflow", "Holdings or prices exceed numeric valuation limits");
    QJsonArray items;
    for (const auto& h : summary.holdings) {
        if (!std::isfinite(h.market_value) || !std::isfinite(h.unrealized_pnl_percent) || !std::isfinite(h.weight))
            throw Error("valuation_overflow", "Holding exceeds numeric valuation limits");
        items.append(QJsonObject{{"symbol", h.symbol},
                                 {"quantity", h.quantity},
                                 {"has_quote", h.has_quote},
                                 {"valuation_basis", h.has_quote ? "quote" : "cost"},
                                 {"price", h.current_price},
                                 {"market_value", h.market_value},
                                 {"cost_basis", h.cost_basis},
                                 {"unrealized_pnl", h.unrealized_pnl},
                                 {"unrealized_pnl_percent", h.unrealized_pnl_percent},
                                 {"weight_percent", h.weight}});
    }
    return {{"portfolio", as_json(p)},
            {"source", live ? "yahoo" : "price_file"},
            {"holdings", items},
            {"complete", portfolio::has_complete_prices(summary)},
            {"priced_positions", summary.priced_positions},
            {"total_positions", summary.total_positions},
            {"total_market_value", summary.total_market_value},
            {"total_cost_basis", summary.total_cost_basis},
            {"unrealized_pnl", summary.total_unrealized_pnl},
            {"unrealized_pnl_percent", summary.total_unrealized_pnl_percent},
            {"day_change_complete", day_change_complete && portfolio::has_complete_prices(summary)},
            {"day_change", day_change_complete && portfolio::has_complete_prices(summary)
                               ? QJsonValue(summary.total_day_change)
                               : QJsonValue(QJsonValue::Null)},
            {"day_change_percent", day_change_complete && portfolio::has_complete_prices(summary)
                                       ? QJsonValue(summary.total_day_change_percent)
                                       : QJsonValue(QJsonValue::Null)},
            {"as_of", summary.last_updated},
            {"warnings", warnings}};
}
} // namespace fincept::cli
