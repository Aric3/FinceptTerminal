#include "network/ProxyManager.h"

#include "core/config/EditionPolicy.h"

#include <QHostAddress>
#include <QNetworkProxyFactory>
#include <QSettings>
#include <QUrl>

namespace fincept::network {
namespace {
ProxySettings startup_settings;

class ApplicationProxyFactory final : public QNetworkProxyFactory {
  public:
    explicit ApplicationProxyFactory(ProxySettings settings) : settings_(std::move(settings)) {}
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return ProxyManager::proxies(query, settings_);
    }

  private:
    const ProxySettings settings_;
};

QString proxy_url(const QList<QNetworkProxy>& proxies) {
    for (const auto& proxy : proxies) {
        if (proxy.type() == QNetworkProxy::NoProxy)
            return {};
        // HTTP works with urllib, requests, uv and curl without optional SOCKS
        // dependencies. SOCKS-only/PAC setups require a manual HTTP endpoint.
        if (proxy.type() != QNetworkProxy::HttpProxy && proxy.type() != QNetworkProxy::HttpCachingProxy)
            continue;
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(proxy.hostName());
        url.setPort(proxy.port());
        url.setUserName(proxy.user());
        url.setPassword(proxy.password());
        return url.toString(QUrl::FullyEncoded);
    }
    return {};
}
} // namespace

ProxySettings ProxySettings::load() {
    QSettings store(edition::organization(), edition::application());
    ProxySettings settings;
    const QString mode = store.value("network/proxy_mode", "system").toString();
    settings.mode = mode == "manual" ? ProxyMode::Manual : mode == "direct" ? ProxyMode::Direct : ProxyMode::System;
    settings.host = store.value("network/proxy_host").toString();
    const int port = store.value("network/proxy_port", 7897).toInt();
    settings.port = port > 0 && port <= 65535 ? static_cast<quint16>(port) : 0;
    settings.bypass = store.value("network/proxy_bypass", settings.bypass).toStringList();
    return settings;
}

QString ProxySettings::validation_error() const {
    if (mode != ProxyMode::Manual)
        return {};
    const QUrl parsed(QStringLiteral("http://") + host);
    if (!parsed.isValid() || host.trimmed() != host || parsed.host().isEmpty() || !parsed.userInfo().isEmpty() ||
        !parsed.path().isEmpty() || parsed.hasQuery() || parsed.hasFragment() || parsed.port() != -1 || !port)
        return QStringLiteral("Enter a proxy hostname/IP and a port from 1 to 65535, without a URL or credentials.");
    return {};
}

bool ProxySettings::save(QString* error) const {
    const QString invalid = validation_error();
    if (!invalid.isEmpty()) {
        if (error)
            *error = invalid;
        return false;
    }
    QSettings store(edition::organization(), edition::application());
    store.setValue("network/proxy_mode", mode == ProxyMode::Manual   ? "manual"
                                         : mode == ProxyMode::Direct ? "direct"
                                                                     : "system");
    store.setValue("network/proxy_host", host);
    store.setValue("network/proxy_port", port);
    store.setValue("network/proxy_bypass", bypass);
    store.sync();
    if (store.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("Could not save proxy settings.");
        return false;
    }
    return true;
}

bool ProxyManager::bypasses(const QString& host, const ProxySettings& settings) {
    const QString value = host.toLower();
    // Loopback always bypasses proxies, including local LLMs and IPC bridges.
    if (value == "localhost" || QHostAddress(value).isLoopback())
        return true;
    for (QString pattern : settings.bypass) {
        pattern = pattern.trimmed().toLower();
        if (pattern.isEmpty())
            continue;
        if (pattern == "*" || value == pattern ||
            (pattern.startsWith('.') && (value == pattern.mid(1) || value.endsWith(pattern))))
            return true;
        if (pattern.startsWith("*.") && (value == pattern.mid(2) || value.endsWith(pattern.mid(1))))
            return true;
    }
    return false;
}

QList<QNetworkProxy> ProxyManager::proxies(const QNetworkProxyQuery& query, const ProxySettings& settings) {
    if (settings.mode == ProxyMode::Direct || bypasses(query.peerHostName(), settings))
        return {QNetworkProxy::NoProxy};
    if (settings.mode == ProxyMode::Manual) {
        // Invalid saved settings must never silently turn a proxy request into
        // a direct request. A closed loopback port produces a visible failure.
        if (!settings.validation_error().isEmpty())
            return {QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1)};
        return {QNetworkProxy(QNetworkProxy::HttpProxy, QUrl(QStringLiteral("http://") + settings.host).host(),
                              settings.port)};
    }
    return QNetworkProxyFactory::systemProxyForQuery(query);
}

QProcessEnvironment ProxyManager::environment(QProcessEnvironment env, const ProxySettings& settings) {
    for (const auto* key :
         {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY", "http_proxy", "https_proxy", "all_proxy", "no_proxy"})
        env.remove(QString::fromLatin1(key));
    if (settings.mode != ProxyMode::Direct) {
        for (const auto* scheme : {"http", "https"}) {
            const QString endpoint = QString::fromLatin1(scheme) + QStringLiteral("://example.com");
            const QString url = proxy_url(proxies(QNetworkProxyQuery(QUrl(endpoint)), settings));
            if (!url.isEmpty()) {
                const QString key = QString::fromLatin1(scheme) + QStringLiteral("_proxy");
                env.insert(key, url);
                env.insert(key.toUpper(), url);
            }
        }
    }
    QStringList bypass = settings.bypass;
    for (const auto* host : {"localhost", "127.0.0.1", "::1"})
        if (!bypass.contains(QString::fromLatin1(host)))
            bypass.append(QString::fromLatin1(host));
    // Leading dots are understood by requests/urllib/curl/uv; '*.x' is not
    // portable across them. Normalize the subset offered by the settings UI.
    for (auto& host : bypass)
        if (host.startsWith("*."))
            host.remove(0, 1);
    env.insert("NO_PROXY", bypass.join(','));
    env.insert("no_proxy", bypass.join(','));
    return env;
}

QByteArray ProxyManager::chromium_flags(const ProxySettings& settings) {
    // Qt WebEngine ignores custom QNetworkProxyFactory instances. Set Chromium's
    // network switches before its first profile/page is created.
    // https://doc.qt.io/qt-6.8/qtwebengine-overview.html#proxy-support
    const auto env = environment(QProcessEnvironment{}, settings);
    QStringList rules;
    for (const auto* scheme : {"http", "https"}) {
        const QUrl proxy(env.value(QString::fromLatin1(scheme) + "_proxy"));
        if (!proxy.isEmpty())
            rules.append(QString::fromLatin1(scheme) + "=" + proxy.adjusted(QUrl::RemoveUserInfo).toString());
    }
    QByteArray flags =
        rules.isEmpty() ? QByteArray("--no-proxy-server") : QByteArray("--proxy-server=") + rules.join(';').toUtf8();
    QStringList bypass = {"localhost", "127.0.0.1", "[::1]"};
    for (const auto& entry : settings.bypass) {
        const QString host = entry.trimmed();
        if (host.isEmpty())
            continue;
        if (host.startsWith('.')) {
            bypass.append("*" + host);
            bypass.append(host.mid(1));
        } else {
            bypass.append(host);
        }
    }
    flags += " --proxy-bypass-list=" + bypass.join(';').toUtf8();
    return flags;
}

void ProxyManager::initialize() {
    startup_settings = ProxySettings::load();
    QNetworkProxyFactory::setApplicationProxyFactory(new ApplicationProxyFactory(startup_settings));
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!flags.isEmpty())
        flags += ' ';
    flags += chromium_flags(startup_settings);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
}

QProcessEnvironment ProxyManager::environment(QProcessEnvironment env) {
    return environment(std::move(env), startup_settings);
}
} // namespace fincept::network
