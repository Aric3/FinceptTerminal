// src/services/portfolio/PortfolioService_ImportExport.cpp
//
// CSV / JSON export and JSON import (with the various ImportMode resolutions).
//
// Part of the partial-class split of PortfolioService.cpp.

#include "core/logging/Logger.h"
#include "services/portfolio/PortfolioService.h"
#include "services/portfolio/PortfolioTransfer.h"
#include "services/sectors/SectorResolver.h"
#include "storage/repositories/PortfolioRepository.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>

namespace fincept::services {

void PortfolioService::export_csv(const QString& portfolio_id, const QString& file_path) {
    auto assets_r = PortfolioRepository::instance().get_assets(portfolio_id);
    auto portfolio_r = PortfolioRepository::instance().get_portfolio(portfolio_id);
    auto txns_r = PortfolioRepository::instance().get_transactions(portfolio_id, 10000);

    if (assets_r.is_err() || portfolio_r.is_err()) {
        LOG_ERROR("PortfolioSvc", "Export CSV failed: cannot load data");
        emit export_failed(file_path, tr("Could not read the portfolio from the database."));
        return;
    }

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        LOG_ERROR("PortfolioSvc", "Cannot open file for writing: " + file_path);
        emit export_failed(file_path, file.errorString());
        return;
    }

    QTextStream out(&file);
    // QTextStream defaults to 6 significant digits, which silently rounded
    // prices/quantities (1234.5678 -> 1234.57, 1234567 -> 1.23457e+06) in the
    // exported file. 12 keeps any realistic holding exact without printing
    // binary-float noise.
    out.setRealNumberPrecision(12);
    auto& p = portfolio_r.value();
    out << "# Portfolio: " << p.name << "\n";
    out << "# Owner: " << p.owner << "\n";
    out << "# Currency: " << p.currency << "\n";
    out << "# Exported: " << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << "\n\n";

    // Holdings section
    out << "## HOLDINGS\n";
    out << "Symbol,Quantity,AvgBuyPrice,CostBasis\n";
    for (const auto& a : assets_r.value()) {
        out << a.symbol << "," << a.quantity << "," << a.avg_buy_price << "," << (a.quantity * a.avg_buy_price) << "\n";
    }

    // Transactions section
    if (txns_r.is_ok() && !txns_r.value().isEmpty()) {
        out << "\n## TRANSACTIONS\n";
        out << "Date,Symbol,Type,Quantity,Price,TotalValue,Notes\n";
        for (const auto& t : txns_r.value()) {
            out << t.transaction_date << "," << t.symbol << "," << t.transaction_type << "," << t.quantity << ","
                << t.price << "," << t.total_value << ",\""
                << QString(t.notes).replace(QLatin1Char('"'), QStringLiteral("\"\"")) << "\"\n";
        }
    }

    file.close();
    LOG_INFO("PortfolioSvc", "Exported CSV to " + file_path);
    emit export_complete(file_path);
}

void PortfolioService::export_json(const QString& portfolio_id, const QString& file_path) {
    const auto data = portfolio::export_portfolio(portfolio_id);
    if (data.is_err()) {
        emit export_failed(file_path, QString::fromStdString(data.error()));
        return;
    }
    QSaveFile file(file_path);
    const auto bytes = QJsonDocument(data.value()).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        emit export_failed(file_path, file.errorString());
        return;
    }
    emit export_complete(file_path);
}

void PortfolioService::import_json(const QString& file_path, portfolio::ImportMode mode,
                                   const QString& merge_target_id) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit import_complete({"", "", 0, {file.errorString()}});
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (!document.isObject()) {
        emit import_complete({"", "", 0, {tr("Expected a JSON object: %1").arg(error.errorString())}});
        return;
    }
    const auto imported = portfolio::import_portfolio(document.object(), mode, merge_target_id);
    if (imported.is_err()) {
        emit import_complete({"", "", 0, {QString::fromStdString(imported.error())}});
        return;
    }
    const auto& result = imported.value();
    QStringList unresolved;
    if (auto assets = PortfolioRepository::instance().get_assets(result.portfolio_id); assets.is_ok()) {
        for (const auto& asset : assets.value()) {
            if (asset.sector.isEmpty())
                unresolved.append(asset.symbol);
            else
                SectorResolver::instance().remember(asset.symbol, asset.sector);
        }
    }
    if (!unresolved.isEmpty())
        SectorResolver::instance().prefetch(unresolved);
    invalidate_cache(result.portfolio_id);
    load_portfolios();
    emit import_complete(result);
    // Import preserves holdings/history only. Historical NAV is a separate,
    // explicit action: current holdings cannot reconstruct historical positions.
}

} // namespace fincept::services
