#pragma once
#include "screens/portfolio/PortfolioTypes.h"

#include <QHash>

#include <cmath>

namespace fincept::portfolio {
struct PriceMark {
    double price = 0;
    double change = 0;
    double change_percent = 0;
};

// Pure calculation shared by the application and deterministic acceptance tests.
inline PortfolioSummary value_portfolio(const Portfolio& portfolio, const QVector<PortfolioAsset>& assets,
                                        const QHash<QString, PriceMark>& prices) {
    PortfolioSummary result;
    result.portfolio = portfolio;
    double previous_value = 0;
    for (const auto& asset : assets) {
        HoldingWithQuote holding;
        holding.symbol = asset.symbol;
        holding.quantity = asset.quantity;
        holding.avg_buy_price = asset.avg_buy_price;
        holding.sector = asset.sector;
        holding.cost_basis = asset.quantity * asset.avg_buy_price;
        const auto mark = prices.constFind(asset.symbol);
        holding.has_quote = mark != prices.cend() && std::isfinite(mark->price) && mark->price > 0 &&
                            std::isfinite(mark->change) && std::isfinite(mark->change_percent);
        if (holding.has_quote) {
            holding.current_price = mark->price;
            holding.day_change = mark->change;
            holding.day_change_percent = mark->change_percent;
            previous_value += (mark->price - mark->change) * asset.quantity;
            ++result.priced_positions;
        } else {
            holding.current_price = asset.avg_buy_price;
        }
        holding.market_value = holding.quantity * holding.current_price;
        holding.unrealized_pnl = holding.market_value - holding.cost_basis;
        holding.unrealized_pnl_percent = holding.cost_basis > 0 ? holding.unrealized_pnl / holding.cost_basis * 100 : 0;
        result.total_market_value += holding.market_value;
        result.total_cost_basis += holding.cost_basis;
        result.total_day_change += holding.day_change * holding.quantity;
        if (holding.has_quote) {
            if (holding.unrealized_pnl >= 0)
                ++result.gainers;
            else
                ++result.losers;
        }
        result.holdings.append(holding);
    }
    for (auto& holding : result.holdings)
        holding.weight = result.total_market_value > 0 ? holding.market_value / result.total_market_value * 100 : 0;
    result.total_unrealized_pnl = result.total_market_value - result.total_cost_basis;
    result.total_unrealized_pnl_percent =
        result.total_cost_basis > 0 ? result.total_unrealized_pnl / result.total_cost_basis * 100 : 0;
    result.total_day_change_percent = previous_value > 0 ? result.total_day_change / previous_value * 100 : 0;
    result.total_positions = assets.size();
    result.last_updated = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return result;
}

inline bool has_complete_prices(const PortfolioSummary& summary) {
    return summary.priced_positions == summary.total_positions;
}
} // namespace fincept::portfolio
