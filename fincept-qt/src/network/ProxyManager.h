#pragma once

#include <QNetworkProxy>
#include <QNetworkProxyQuery>
#include <QProcessEnvironment>
#include <QStringList>

namespace fincept::network {

enum class ProxyMode { System, Manual, Direct };

struct ProxySettings {
    ProxyMode mode = ProxyMode::System;
    QString host;
    quint16 port = 7897;
    QStringList bypass = {"localhost", "127.0.0.1", "::1"};

    static ProxySettings load();
    bool save(QString* error = nullptr) const;
    QString validation_error() const;
};

// One startup snapshot is shared by Qt's application proxy factory and every
// child process. Changes apply after restart so existing workers/WebSockets
// cannot continue under a different policy from newly created requests.
class ProxyManager {
  public:
    static void initialize();
    static QProcessEnvironment environment(QProcessEnvironment env);

    // Pure configuration seams, also used by integration tests.
    static bool bypasses(const QString& host, const ProxySettings& settings);
    static QList<QNetworkProxy> proxies(const QNetworkProxyQuery& query, const ProxySettings& settings);
    static QProcessEnvironment environment(QProcessEnvironment env, const ProxySettings& settings);
    static QByteArray chromium_flags(const ProxySettings& settings);
};

} // namespace fincept::network
