#pragma once
#include "network/ProxyManager.h"

#include <QDeadlineTimer>
#include <QJsonValue>
#include <QStringList>

#include <stdexcept>

namespace fincept::cli {
struct Error : std::runtime_error {
    QString code;
    int status;
    Error(QString code, QString message, int status = 2)
        : std::runtime_error(message.toStdString()), code(std::move(code)), status(status) {}
};
struct Runtime {
    QString root;
    QString profile;
    QString db_path;
    QString python;
    QString scripts;
    network::ProxySettings proxy;
    QDeadlineTimer deadline;
    QJsonValue read_yahoo(const QStringList& arguments) const;
    QJsonObject doctor(bool online) const;
};
QString default_root();
QString profile_database(const QString& root, const QString& profile);
QJsonValue read_json(const QString& path);
QJsonArray profiles(const QString& root);
} // namespace fincept::cli
