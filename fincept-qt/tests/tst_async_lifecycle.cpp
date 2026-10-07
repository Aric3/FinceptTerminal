#include "mcp/AsyncDispatch.h"
#include "mcp/AsyncToolCall.h"
#include "mcp/tools/ThreadHelper.h"

#include <QtTest>

#include <future>
#include <stdexcept>

using namespace fincept::mcp;
using namespace fincept::mcp::tools::detail;

class AsyncLifecycleTest : public QObject {
    Q_OBJECT
  private slots:
    void synchronous_and_duplicate_completion() {
        int result = 0;
        QVERIFY(run_async_wait(
            nullptr,
            [&](auto done) {
                auto callback = [done, &result]() {
                    auto lease = done.lease();
                    if (!lease.owns_lock())
                        return;
                    ++result;
                    done();
                };
                callback();
                callback();
            },
            100));
        QCOMPARE(result, 1);
    }
    void same_thread_timeout_ignores_late_stack_write() {
        std::function<void()> late;
        int writes = 0;
        {
            int stack_value = 0;
            QVERIFY(!run_async_wait(
                nullptr,
                [&](auto done) {
                    late = [done, &stack_value, &writes]() {
                        auto lease = done.lease();
                        if (!lease.owns_lock())
                            return;
                        stack_value = 42;
                        ++writes;
                        done();
                    };
                },
                20));
            QCOMPARE(stack_value, 0);
        }
        late();
        QCOMPARE(writes, 0);
    }
    void queued_start_cannot_run_after_timeout() {
        QObject target;
        std::atomic<int> starts{0};
        auto task = std::async(std::launch::async, [&]() {
            return run_async_wait(
                &target,
                [&](auto done) {
                    ++starts;
                    done();
                },
                20);
        });
        // Intentionally don't pump the target loop until the worker has returned.
        QVERIFY(task.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
        QVERIFY(!task.get());
        QCoreApplication::processEvents();
        QCOMPARE(starts.load(), 0);
    }
    void cross_thread_completion_and_late_callback() {
        QObject target;
        std::function<void()> late;
        std::atomic<int> writes{0};
        auto task = std::async(std::launch::async, [&]() {
            int stack_value = 0;
            const bool completed = run_async_wait(
                &target,
                [&](auto done) {
                    late = [done, &stack_value, &writes]() {
                        auto lease = done.lease();
                        if (!lease.owns_lock())
                            return;
                        stack_value = 7;
                        ++writes;
                        done();
                    };
                    late();
                },
                300);
            return completed && stack_value == 7;
        });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 1000);
        QVERIFY(task.get());
        late();
        QCOMPARE(writes.load(), 1);
    }
    void cross_thread_timeout_ignores_late_stack_write() {
        QObject target;
        std::function<void()> late;
        int writes = 0;
        auto task = std::async(std::launch::async, [&]() {
            int stack_value = 0;
            return run_async_wait(
                &target,
                [&](auto done) {
                    late = [done, &stack_value, &writes]() {
                        auto lease = done.lease();
                        if (!lease.owns_lock())
                            return;
                        stack_value = 42;
                        ++writes;
                        done();
                    };
                },
                40);
        });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 500);
        QVERIFY(!task.get());
        QVERIFY(bool(late));
        late();
        QCOMPARE(writes, 0);
    }

    void cancellation_releases_same_thread_wait() {
        bool cancelled = false;
        QTimer::singleShot(20, [&]() { cancelled = true; });
        QVERIFY(!run_async_wait(nullptr, [](auto) {}, 500, [&]() { return cancelled; }));
    }
    void destroyed_target_and_throw_release_wait() {
        auto* target = new QObject;
        QVERIFY(!run_async_wait(target, [target](auto) { delete target; }, 100));
        QVERIFY(!run_async_wait(nullptr, [](auto) { throw std::runtime_error("fixture"); }, 100));
    }
    void typed_bridge_delivers_arguments() {
        int result = 0;
        QString message;
        QVERIFY(run_async_callback_sync(
            nullptr, [](auto resolve) { resolve(7, QString("ok")); },
            [&](int value, QString text) {
                result = value;
                message = std::move(text);
            },
            100));
        QCOMPARE(result, 7);
        QCOMPARE(message, QString("ok"));
    }
    void async_timeout_and_late_resolve_have_one_result() {
        ToolContext context;
        context.timeout_ms = 20;
        auto call = prepare_async_tool_call("fixture", context);
        auto future = call.promise->future();
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 500);
        QVERIFY(!future.result().success);
        QVERIFY(future.result().error.contains("timed out"));
        QVERIFY(call.context.cancelled());
        call.resolve(ToolResult::ok("late"));
        QCOMPARE(future.resultCount(), 1);
    }
    void async_cancellation_releases_and_cleans_timers() {
        const auto timers_before = QCoreApplication::instance()->findChildren<QTimer*>().size();
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        ToolContext context;
        context.timeout_ms = 500;
        context.is_cancelled = [cancelled]() { return cancelled->load(); };
        auto call = prepare_async_tool_call("fixture", context);
        auto future = call.promise->future();
        cancelled->store(true);
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 300);
        QCOMPARE(future.result().error, QString("cancelled"));
        call.resolve(ToolResult::ok("late"));
        QCOMPARE(future.resultCount(), 1);
        QTRY_COMPARE(QCoreApplication::instance()->findChildren<QTimer*>().size(), timers_before);
    }
    void async_dispatch_finishes_shared_gate_and_cleans_timers() {
        const auto before = QCoreApplication::instance()->findChildren<QTimer*>().size();
        ToolContext context;
        context.timeout_ms = 500;
        auto call = prepare_async_tool_call("fixture", context);
        AsyncDispatch::callback_to_promise(nullptr, call.context, call.promise, [](auto resolve) {
            QTimer::singleShot(20, [resolve]() { resolve(ToolResult::ok("done")); });
        });
        auto future = call.promise->future();
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 300);
        QVERIFY(future.result().success);
        QTRY_COMPARE(QCoreApplication::instance()->findChildren<QTimer*>().size(), before);
        call.resolve(ToolResult::fail("late"));
        QCOMPARE(future.resultCount(), 1);
    }
    void queued_dispatch_destruction_and_exception_complete() {
        auto* target = new QObject;
        auto call = prepare_async_tool_call("fixture", {});
        std::atomic<int> invoked{0};
        auto task = std::async(std::launch::async, [&]() {
            AsyncDispatch::callback_to_promise(target, call.context, call.promise, [&](auto resolve) {
                ++invoked;
                resolve(ToolResult::ok("done"));
            });
        });
        task.wait();
        delete target;
        QVERIFY(call.promise->future().isFinished());
        QVERIFY(!call.promise->future().result().success);
        QCOMPARE(invoked.load(), 0);
        auto throwing = prepare_async_tool_call("fixture", {});
        AsyncDispatch::callback_to_promise(nullptr, throwing.context, throwing.promise,
                                           [](auto) { throw std::runtime_error("fixture"); });
        QVERIFY(throwing.promise->future().isFinished());
        QVERIFY(throwing.promise->future().result().error.contains("fixture"));
    }
};
QTEST_GUILESS_MAIN(AsyncLifecycleTest)
#include "tst_async_lifecycle.moc"
