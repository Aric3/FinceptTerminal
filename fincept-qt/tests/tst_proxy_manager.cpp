#include "network/ProxyManager.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QtTest>

using namespace fincept::network;

class ProxyManagerTest : public QObject {
    Q_OBJECT
  private slots:
    void validation_and_bypass() {
        ProxySettings settings;
        settings.mode = ProxyMode::Manual;
        settings.host = "127.0.0.1";
        QVERIFY(settings.validation_error().isEmpty());
        settings.host = "http://127.0.0.1";
        QVERIFY(!settings.validation_error().isEmpty());
        settings.host = "user:password@host";
        QVERIFY(!settings.validation_error().isEmpty());
        settings.host = "127.0.0.1";
        settings.bypass.append(".example.org");
        QVERIFY(ProxyManager::bypasses("localhost", settings));
        QVERIFY(ProxyManager::bypasses("::1", settings));
        QVERIFY(ProxyManager::bypasses("api.example.org", settings));
        QVERIFY(ProxyManager::bypasses("example.org", settings));
        QVERIFY(!ProxyManager::bypasses("example.org.evil", settings));
        QVERIFY(!ProxyManager::bypasses("market.invalid", settings));
        QVERIFY(!ProxyManager::bypasses("127.evil.example", settings));
    }

    void environment_replaces_stale_proxy_values() {
        QProcessEnvironment inherited;
        inherited.insert("HTTP_PROXY", "http://old:9999");
        inherited.insert("all_proxy", "socks5://old:9999");
        inherited.insert("MY_API_KEY", "preserved");
        ProxySettings settings;
        settings.mode = ProxyMode::Manual;
        settings.host = "127.0.0.1";
        settings.port = 7897;
        auto env = ProxyManager::environment(inherited, settings);
        QCOMPARE(env.value("HTTPS_PROXY"), QString("http://127.0.0.1:7897"));
        QCOMPARE(env.value("http_proxy"), env.value("HTTP_PROXY"));
        QVERIFY(!env.contains("all_proxy"));
        QCOMPARE(env.value("MY_API_KEY"), QString("preserved"));
        QVERIFY(env.value("NO_PROXY").contains("localhost"));
        settings.mode = ProxyMode::Direct;
        env = ProxyManager::environment(inherited, settings);
        QVERIFY(!env.contains("HTTP_PROXY"));
        QVERIFY(!env.contains("all_proxy"));
    }

    void manual_http_uses_proxy() {
        QTcpServer proxy;
        QVERIFY(proxy.listen(QHostAddress::LocalHost));
        QByteArray received;
        connect(&proxy, &QTcpServer::newConnection, this, [&] {
            auto* socket = proxy.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                received += socket->readAll();
                if (!received.contains("\r\n\r\n"))
                    return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                socket->disconnectFromHost();
            });
        });
        ProxySettings settings;
        settings.mode = ProxyMode::Manual;
        settings.host = "127.0.0.1";
        settings.port = proxy.serverPort();
        const QUrl target("http://market.invalid/quote");
        auto proxies = ProxyManager::proxies(QNetworkProxyQuery(target), settings);
        QCOMPARE(proxies.size(), 1);
        QNetworkAccessManager manager;
        manager.setProxy(proxies.first());
        auto* reply = manager.get(QNetworkRequest(target));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QVERIFY(received.startsWith("GET http://market.invalid/quote HTTP/1.1"));
        reply->deleteLater();
        QCOMPARE(ProxyManager::proxies(QNetworkProxyQuery(QUrl("http://localhost:11434")), settings).first().type(),
                 QNetworkProxy::NoProxy);
    }

    void webengine_shares_manual_and_direct_policy() {
        ProxySettings settings;
        settings.mode = ProxyMode::Manual;
        settings.host = "127.0.0.1";
        settings.port = 7897;
        auto flags = ProxyManager::chromium_flags(settings);
        QVERIFY(flags.contains("--proxy-server=http=http://127.0.0.1:7897;https=http://127.0.0.1:7897"));
        QVERIFY(flags.contains("--proxy-bypass-list="));
        settings.mode = ProxyMode::Direct;
        QVERIFY(ProxyManager::chromium_flags(settings).contains("--no-proxy-server"));
    }

    void invalid_manual_never_falls_back_to_direct() {
        ProxySettings settings;
        settings.mode = ProxyMode::Manual;
        const auto proxies = ProxyManager::proxies(QNetworkProxyQuery(QUrl("https://market.invalid")), settings);
        QCOMPARE(proxies.size(), 1);
        QCOMPARE(proxies.first().type(), QNetworkProxy::HttpProxy);
        QCOMPARE(proxies.first().port(), quint16(1));
    }
};

QTEST_GUILESS_MAIN(ProxyManagerTest)
#include "tst_proxy_manager.moc"
