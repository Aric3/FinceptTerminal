#pragma once

#include "mcp/McpTypes.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFutureWatcher>
#include <QTimer>

namespace fincept::mcp {

struct AsyncToolCall {
    std::shared_ptr<QPromise<ToolResult>> promise;
    ToolContext context;
    std::function<void(ToolResult)> resolve;
};

// One completion gate for the handler, timeout, cancellation and exception path.
// Timers live on the application thread and are released whenever the future
// finishes, including handlers that resolve through AsyncDispatch directly.
inline AsyncToolCall prepare_async_tool_call(QString name, ToolContext context) {
    auto promise = std::make_shared<QPromise<ToolResult>>();
    promise->start();
    auto resolved = std::make_shared<std::atomic<bool>>(false);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto original_cancel = context.is_cancelled;
    context.resolve_guard = resolved;
    context.is_cancelled = [cancelled, original_cancel]() {
        return cancelled->load() || (original_cancel && original_cancel());
    };
    auto resolve = [promise, resolved](ToolResult result) {
        bool expected = false;
        if (!resolved->compare_exchange_strong(expected, true))
            return;
        promise->addResult(std::move(result));
        promise->finish();
    };
    AsyncToolCall call{promise, context, resolve};
    auto* application = QCoreApplication::instance();
    if (!application || context.timeout_ms <= 0) {
        resolve(ToolResult::fail("Async call requires an event loop and a positive timeout"));
        return call;
    }
    const QDeadlineTimer deadline(context.timeout_ms);
    QMetaObject::invokeMethod(
        application,
        [application, promise, resolved, cancelled, original_cancel, resolve, name, deadline]() {
            if (promise->future().isFinished())
                return;
            auto* lifetime = new QObject(application);
            auto* watcher = new QFutureWatcher<ToolResult>(lifetime);
            QObject::connect(watcher, &QFutureWatcher<ToolResult>::finished, lifetime, &QObject::deleteLater);
            watcher->setFuture(promise->future());
            auto* watchdog = new QTimer(lifetime);
            watchdog->setSingleShot(true);
            QObject::connect(watchdog, &QTimer::timeout, lifetime, [resolved, cancelled, resolve, name]() {
                if (resolved->load())
                    return;
                cancelled->store(true);
                resolve(ToolResult::fail("Tool '" + name + "' timed out"));
            });
            watchdog->start(static_cast<int>(deadline.remainingTime()));
            if (original_cancel) {
                auto* poll = new QTimer(lifetime);
                poll->setInterval(10);
                QObject::connect(poll, &QTimer::timeout, lifetime, [resolved, cancelled, original_cancel, resolve]() {
                    if (!resolved->load() && original_cancel()) {
                        cancelled->store(true);
                        resolve(ToolResult::fail("cancelled"));
                    }
                });
                poll->start();
            }
        },
        Qt::QueuedConnection);
    return call;
}

} // namespace fincept::mcp
