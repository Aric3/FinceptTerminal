#include "services/feeds/FeedSelfTest.h"

#include <QtTest>

class FeedParsingTest : public QObject {
    Q_OBJECT
  private slots:
    void supported_formats() { QCOMPARE(fincept::feeds::run_feed_selftest(), 0); }
};

QTEST_GUILESS_MAIN(FeedParsingTest)
#include "tst_feed_parsing.moc"
