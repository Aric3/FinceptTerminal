#include "services/portfolio/PortfolioValuation.h"

#include <QtTest>

#include <limits>

using namespace fincept::portfolio;
class PortfolioValuationTest : public QObject {
    Q_OBJECT
    static QVector<PortfolioAsset> assets() {
        PortfolioAsset a;
        a.symbol = "AAPL";
        a.quantity = 10;
        a.avg_buy_price = 100;
        PortfolioAsset b;
        b.symbol = "MSFT";
        b.quantity = 5;
        b.avg_buy_price = 200;
        return {a, b};
    }
  private slots:
    void complete_prices() {
        const auto s = value_portfolio({}, assets(), {{"AAPL", {120, 10, 9.09}}, {"MSFT", {180, -20, -10}}});
        QVERIFY(has_complete_prices(s));
        QCOMPARE(s.priced_positions, 2);
        QCOMPARE(s.total_market_value, 2100.0);
        QCOMPARE(s.total_cost_basis, 2000.0);
        QCOMPARE(s.total_unrealized_pnl, 100.0);
        QCOMPARE(s.total_unrealized_pnl_percent, 5.0);
        QCOMPARE(s.total_day_change, 0.0);
        QCOMPARE(s.total_day_change_percent, 0.0);
        QCOMPARE(s.gainers, 1);
        QCOMPARE(s.losers, 1);
        QVERIFY(qAbs(s.holdings[0].weight - 1200.0 / 2100 * 100) < 1e-10);
        QVERIFY(qAbs(s.holdings[0].weight + s.holdings[1].weight - 100) < 1e-10);
    }
    void partial_prices_use_cost_basis() {
        const auto s = value_portfolio({}, assets(), {{"AAPL", {120, 10, 9.09}}});
        QVERIFY(!has_complete_prices(s));
        QCOMPARE(s.priced_positions, 1);
        QCOMPARE(s.total_market_value, 2200.0);
        QCOMPARE(s.total_unrealized_pnl, 200.0);
        QCOMPARE(s.total_day_change, 100.0);
        QVERIFY(qAbs(s.total_day_change_percent - 100.0 / 1100 * 100) < 1e-10);
        QVERIFY(!s.holdings[1].has_quote);
        QCOMPARE(s.holdings[1].current_price, 200.0);
        QCOMPARE(s.gainers, 1);
        QCOMPARE(s.losers, 0);
    }
    void missing_and_invalid_prices() {
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const QList<QHash<QString, PriceMark>> cases = {
            {}, {{"AAPL", {0, 0, 0}}, {"MSFT", {nan, 0, 0}}}, {{"AAPL", {120, nan, 0}}, {"MSFT", {-1, 0, 0}}}};
        for (const auto& quotes : cases) {
            const auto s = value_portfolio({}, assets(), quotes);
            QVERIFY(!has_complete_prices(s));
            QCOMPARE(s.priced_positions, 0);
            QCOMPARE(s.total_market_value, 2000.0);
            QCOMPARE(s.total_unrealized_pnl, 0.0);
            QCOMPARE(s.total_day_change_percent, 0.0);
            QCOMPARE(s.gainers, 0);
        }
    }
    void empty_and_zero_cost_are_finite() {
        const auto empty = value_portfolio({}, {}, {});
        QVERIFY(has_complete_prices(empty));
        QCOMPARE(empty.total_market_value, 0.0);
        auto positions = assets();
        for (auto& a : positions) {
            a.avg_buy_price = 0;
            a.quantity = 0;
        }
        const auto s = value_portfolio({}, positions, {{"AAPL", {120, 10, 9.09}}, {"MSFT", {180, -20, -10}}});
        QVERIFY(has_complete_prices(s));
        QCOMPARE(s.total_unrealized_pnl_percent, 0.0);
        QCOMPARE(s.total_day_change_percent, 0.0);
        for (const auto& h : s.holdings)
            QCOMPARE(h.weight, 0.0);
    }
};
QTEST_GUILESS_MAIN(PortfolioValuationTest)
#include "tst_portfolio_valuation.moc"
