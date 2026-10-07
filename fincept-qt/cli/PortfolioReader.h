#pragma once
#include "Runtime.h"
#include "screens/portfolio/PortfolioTypes.h"

#include <QJsonObject>
#include <QSqlDatabase>

namespace fincept::cli {
class PortfolioReader {
  public:
    explicit PortfolioReader(const QString& path);
    ~PortfolioReader();
    PortfolioReader(const PortfolioReader&) = delete;
    PortfolioReader& operator=(const PortfolioReader&) = delete;
    QJsonObject list(int limit, int offset) const;
    QJsonObject get(const QString& id) const;
    QString resolve(const QString& name) const;
    QJsonObject value(const QString& id, const QString& prices_file, bool live, const Runtime& runtime) const;

  private:
    portfolio::Portfolio portfolio(const QString& id) const;
    QVector<portfolio::PortfolioAsset> assets(const QString& id) const;
    QSqlDatabase db_;
};
} // namespace fincept::cli
