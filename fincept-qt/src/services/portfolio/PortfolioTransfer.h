#pragma once
#include "core/result/Result.h"
#include "screens/portfolio/PortfolioTypes.h"

#include <QJsonObject>

namespace fincept::portfolio {
// Version 2 stores authoritative holdings and transaction history independently.
// Version 1 remains readable via chronological replay. Imports are atomic.
Result<QJsonObject> export_portfolio(const QString& id);
Result<ImportResult> import_portfolio(const QJsonObject& document, ImportMode mode, const QString& target = {});
} // namespace fincept::portfolio
