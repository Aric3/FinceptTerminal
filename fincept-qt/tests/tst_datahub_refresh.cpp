#include "datahub/DataHub.h"

#include <QtTest>
using namespace fincept::datahub;
class StubProducer final : public Producer {
  public:
    ~StubProducer() override { DataHub::instance().unregister_producer(this); }
    QStringList topic_patterns() const override { return {"test:refresh:*"}; }
    void refresh(const QStringList& topics) override { batches.append(topics); }
    QList<QStringList> batches;
};
class RefreshTest : public QObject {
    Q_OBJECT
  private slots:
    void cancelled_batch_can_be_requested_again_data() {
        QTest::addColumn<bool>("single");
        QTest::newRow("unsubscribe-owner") << false;
        QTest::newRow("unsubscribe-topic") << true;
    }
    void cancelled_batch_can_be_requested_again() {
        QFETCH(bool, single);
        auto& hub = DataHub::instance();
        StubProducer producer;
        QObject owner;
        const QString topic = single ? "test:refresh:single" : "test:refresh:owner";
        hub.register_producer(&producer);
        hub.set_coalesce_window_ms(30);
        hub.subscribe(&owner, topic, [](const QVariant&) {});
        if (single)
            hub.unsubscribe(&owner, topic);
        else
            hub.unsubscribe(&owner);
        hub.subscribe(&owner, topic, [](const QVariant&) {});
        QTRY_COMPARE_WITH_TIMEOUT(producer.batches.size(), 1, 500);
        QCOMPARE(producer.batches.first(), QStringList{topic});
        hub.publish(topic, 42);
        hub.unsubscribe(&owner);
        hub.unregister_producer(&producer);
    }
    void timeout_notifies_consumers_and_allows_retry() {
        auto& hub = DataHub::instance();
        StubProducer producer;
        QObject owner;
        const QString topic = "test:refresh:timeout";
        hub.register_producer(&producer);
        TopicPolicy policy;
        policy.refresh_timeout_ms = 50;
        policy.min_interval_ms = 10000;
        hub.set_policy(topic, policy);
        int direct = 0, pattern = 0;
        hub.subscribe_errors(&owner, topic, [&](const QString& error) {
            QVERIFY(!error.isEmpty());
            ++direct;
        });
        hub.subscribe_pattern_errors(&owner, "test:refresh:*", [&](const QString&, const QString&) { ++pattern; });
        hub.subscribe(&owner, topic, [](const QVariant&) {});
        QTRY_COMPARE_WITH_TIMEOUT(producer.batches.size(), 1, 500);
        QTRY_COMPARE_WITH_TIMEOUT(direct, 1, 2500);
        QCOMPARE(pattern, 1);
        hub.request(topic, true);
        QTRY_COMPARE_WITH_TIMEOUT(producer.batches.size(), 2, 500);
        hub.publish(topic, 42);
        hub.unsubscribe(&owner);
        hub.unregister_producer(&producer);
    }
};
QTEST_GUILESS_MAIN(RefreshTest)
#include "tst_datahub_refresh.moc"
