#include "Runtime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>

namespace fincept::cli {
QString default_root() {
#ifdef Q_OS_MACOS
    return QDir::homePath() + "/Library/Application Support/dev.aric.fincept.local";
#elif defined(Q_OS_WIN)
    return qEnvironmentVariable("LOCALAPPDATA") + "/dev.aric.fincept.local";
#else
    return QDir::homePath() + "/.local/share/dev.aric.fincept.local";
#endif
}
QString profile_database(const QString& root, const QString& profile) {
    static const QRegularExpression safe("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$");
    if (!safe.match(profile).hasMatch())
        throw Error("invalid_profile", "Profile must be a name, not a path");
    return root + (profile == "default" ? "" : "/profiles/" + profile) + "/data/fincept.db";
}
QJsonValue read_json(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error("file_unavailable", "Cannot read requested JSON file", 3);
    if (file.size() > 8 * 1024 * 1024)
        throw Error("file_too_large", "JSON input exceeds 8 MiB");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError)
        throw Error("invalid_json", "Input is not a JSON object or array");
    return document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
}
QJsonArray profiles(const QString& root) {
    QJsonArray result;
    QStringList names{"default"};
    const auto manifest = root + "/profiles.json";
    if (QFileInfo::exists(manifest)) {
        for (auto entry : read_json(manifest).toObject().value("profiles").toArray()) {
            const QString name = entry.isString() ? entry.toString() : entry.toObject().value("name").toString();
            if (!name.isEmpty() && !names.contains(name))
                names.append(name);
        }
    }
    for (const auto& name : names) {
        const auto path = profile_database(root, name);
        result.append(QJsonObject{{"name", name}, {"database_available", QFileInfo(path).isFile()}});
    }
    return result;
}
QJsonValue Runtime::read_yahoo(const QStringList& arguments) const {
    if (!QFileInfo(python).isExecutable() || !QFileInfo(scripts + "/yfinance_data.py").isFile())
        throw Error("runtime_unavailable", "Python environment or Yahoo script missing; run doctor", 3);
    if (deadline.hasExpired())
        throw Error("timeout", "Command exceeded its total time budget", 6);
    QProcess process;
    auto env = network::ProxyManager::environment(QProcessEnvironment::systemEnvironment(), proxy);
    env.insert("PYTHONUNBUFFERED", "1");
    env.insert("FINCEPT_YF_TIMEOUT",
               QString::number(qMin<qint64>(15, qMax<qint64>(1, deadline.remainingTime() / 1000))));
    process.setProcessEnvironment(env);
    process.start(python, QStringList{scripts + "/yfinance_data.py"} + arguments);
    if (!process.waitForStarted(static_cast<int>(qMin<qint64>(3000, deadline.remainingTime()))))
        throw Error("runtime_unavailable", "Could not start Python environment", 3);
    QByteArray output;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(static_cast<int>(qMin<qint64>(100, qMax<qint64>(1, deadline.remainingTime()))));
        output += process.readAllStandardOutput();
        process.readAllStandardError(); // Provider stderr can contain URLs/cookies; never forward it.
        if (deadline.hasExpired() || output.size() > 8 * 1024 * 1024) {
            process.kill();
            process.waitForFinished(3000);
            throw Error(deadline.hasExpired() ? "timeout" : "output_too_large",
                        "Provider exceeded command time/output budget", 6);
        }
    }
    output += process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        throw Error("provider_failed", "Yahoo script failed; check Python dependencies and proxy", 4);
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(output.trimmed(), &parse);
    if (parse.error != QJsonParseError::NoError)
        throw Error("provider_invalid_json", "Yahoo returned an invalid JSON response", 4);
    const auto object = document.object();
    if (object.contains("error"))
        throw Error("provider_failed",
                    "Yahoo could not complete the request; check ticker, connectivity and rate limits", 4);
    return document.isArray() ? QJsonValue(document.array()) : QJsonValue(object);
}
QJsonObject Runtime::doctor(bool online) const {
    const auto env = network::ProxyManager::environment(QProcessEnvironment::systemEnvironment(), proxy);
    const QString mode = proxy.mode == network::ProxyMode::Manual   ? "manual"
                         : proxy.mode == network::ProxyMode::Direct ? "direct"
                                                                    : "system";
    QJsonObject result{{"read_only", true},
                       {"profile", profile},
                       {"database_available", QFileInfo(db_path).isFile()},
                       {"python", python},
                       {"python_available", QFileInfo(python).isExecutable()},
                       {"scripts", scripts},
                       {"scripts_available", QFileInfo(scripts + "/yfinance_data.py").isFile()},
                       {"proxy_mode", mode},
                       {"http_proxy_available", !env.value("HTTP_PROXY").isEmpty()},
                       {"https_proxy_available", !env.value("HTTPS_PROXY").isEmpty()},
                       {"auth_required", false},
                       {"provider", "yahoo"},
                       {"online_checked", online}};
    if (online) {
        try {
            const auto quote = read_yahoo({"quote", "^GSPC"}).toObject();
            result.insert("online_ok", quote.value("price").toDouble() > 0);
        } catch (const Error& e) {
            result.insert("online_ok", false);
            result.insert("online_error_code", e.code);
        }
    }
    return result;
}
} // namespace fincept::cli
