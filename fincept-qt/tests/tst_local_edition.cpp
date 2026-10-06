#include "core/config/EditionPolicy.h"
#include "network/cloud/CloudClient.h"
#include "network/http/HttpClient.h"
#include "services/llm/ProviderCatalog.h"

#include <QNetworkProxy>
#include <QTcpServer>
#include <QtTest>

class LocalEditionTest : public QObject {
    Q_OBJECT
  private slots:
    void service_policy() {
        QVERIFY(fincept::edition::is_local());
        for (const auto* url : {"https://api.fincept.in/v1", "wss://api.fincept.in/ws/news", "https://FINCEPT.IN"})
            QVERIFY(!fincept::edition::allows_url(QUrl(QString::fromLatin1(url))));
        for (const auto* url : {"https://fincept.in.example.org", "https://myfincept.in", "http://localhost:8080"})
            QVERIFY(fincept::edition::allows_url(QUrl(QString::fromLatin1(url))));
        QVERIFY(!fincept::edition::allows_screen("profile"));
        QVERIFY(fincept::edition::allows_screen("portfolio"));
        QVERIFY(!fincept::ai_chat::ProviderCatalog::known_providers().contains("fincept"));
        QVERIFY(fincept::ai_chat::ProviderCatalog::is_blocked("fincept"));
        QVERIFY(!fincept::ai_chat::ProviderCatalog::is_blocked("ollama", "http://localhost:11434"));
    }

    void rejects_official_http_asynchronously() {
        auto& client = fincept::HttpClient::instance();
        client.set_base_url("https://api.fincept.in");
        int calls = 0;
        auto callback = [&](fincept::Result<QJsonDocument> result) {
            QVERIFY(!result.is_ok());
            QVERIFY(QString::fromStdString(result.error()).contains("disabled"));
            ++calls;
        };
        client.get("/health", callback, this);
        client.post("/write", {}, callback, this);
        client.put("/write", {}, callback, this);
        client.del("/write", callback, this);
        client.del("/write", QJsonObject{}, callback, this);
        client.post_form("/write", {}, callback, this);
        QCOMPARE(calls, 0);
        QTRY_COMPARE(calls, 6);
    }

    void canceled_receiver_is_not_called() {
        bool called = false;
        auto* receiver = new QObject;
        fincept::HttpClient::instance().get("/health", [&](auto) { called = true; }, receiver);
        delete receiver;
        QTest::qWait(10);
        QVERIFY(!called);
    }

    void rejects_cloud_without_auth_error() {
        bool called = false;
        fincept::cloud::CloudClient::instance().get(
            "/sync",
            [&](auto result) {
                QVERIFY(!result.ok);
                QVERIFY(!result.is_auth_error());
                QCOMPARE(result.error, QStringLiteral("service_disabled"));
                called = true;
            },
            this);
        QVERIFY(!called);
        QTRY_VERIFY(called);
    }

    void independent_http_still_works() {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}");
                socket->disconnectFromHost();
            });
        });
        bool called = false;
        fincept::HttpClient::instance().get(
            QString("http://127.0.0.1:%1/").arg(server.serverPort()),
            [&](auto result) {
                QVERIFY(result.is_ok());
                QVERIFY(result.value().object().value("ok").toBool());
                called = true;
            },
            this);
        QTRY_VERIFY(called);
    }
};

QTEST_GUILESS_MAIN(LocalEditionTest)
#include "tst_local_edition.moc"
