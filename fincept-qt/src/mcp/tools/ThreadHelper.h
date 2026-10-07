#pragma once
// ThreadHelper.h — shared thread-marshaling helpers for MCP tool handlers.
//
// MCP tool handlers run on a QtConcurrent::run worker thread (off the main
// UI thread). Calling a Qt service that owns a QNetworkAccessManager (or
// any QObject parented on a different thread) directly from a worker is a
// Qt invariant violation: signals fire on the owning thread, child QObjects
// get the wrong parent thread, and the app crashes a few seconds later
// with `QObject: Cannot create children for a parent that is in a different
// thread` warnings. We've seen this pattern crash `get_news`, `refresh_news`,
// and is latent in 7+ other tool .cpp files (Edgar, Forum, Profile, MA,
// AltInvestments, PythonTools async runner, DataHub subscribe).
//
// The fix is always the same: post the work to the target's thread, sleep
// the worker on a wait condition, wake when the callback fires. These two
// helpers consolidate that pattern so each tool file doesn't re-implement
// it (poorly).

#include "core/logging/Logger.h"

#include <QDeadlineTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QRecursiveMutex>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>

namespace fincept::mcp::tools::detail {

// ── run_on_target_thread_sync ────────────────────────────────────────────────
//
// Run `fn` on `target`'s thread and block the calling thread until it
// returns. If we're already on the target's thread we just call directly.
//
// Use this when the target call is itself synchronous on the main thread —
// e.g. `service->some_getter()` — and you want the result back on the
// worker without races.
template <typename F>
void run_on_target_thread_sync(QObject* target, F&& fn) {
    if (!target || QThread::currentThread() == target->thread()) {
        std::forward<F>(fn)();
        return;
    }
    QMetaObject::invokeMethod(target, std::forward<F>(fn), Qt::BlockingQueuedConnection);
}

// A callback must hold signal_done.lease() while touching caller-owned storage.
// Timeout closes the gate under the same lock before the caller can unwind.
// The queued start is also leased, so it cannot run after a timed-out call.
inline constexpr int kAsyncWaitTimeoutMs = 120000;
inline constexpr const char* kAsyncWaitFailure =
    "Request did not complete (timeout, cancellation, failed start or unavailable service)";

struct AsyncWaitState {
    QRecursiveMutex access;
    QMutex mutex;
    QWaitCondition condition;
    bool closed = false;        // guarded by access
    bool completed = false;     // guarded by mutex
    QEventLoop* loop = nullptr; // guarded by mutex
    QDeadlineTimer deadline;
};

class AsyncWaitSignal {
  public:
    explicit AsyncWaitSignal(std::shared_ptr<AsyncWaitState> state) : state_(std::move(state)) {}

    std::unique_lock<QRecursiveMutex> lease() const {
        std::unique_lock<QRecursiveMutex> lock(state_->access);
        if (!state_->closed && state_->deadline.hasExpired())
            finish(false);
        if (state_->closed)
            lock.unlock();
        return lock;
    }

    void operator()() const { finish(true); }
    void abort() const { finish(false); }

  private:
    void finish(bool completed) const {
        std::unique_lock<QRecursiveMutex> access(state_->access);
        if (state_->closed)
            return;
        state_->closed = true;
        QMutexLocker lock(&state_->mutex);
        state_->completed = completed;
        state_->condition.wakeAll();
        if (state_->loop)
            QMetaObject::invokeMethod(state_->loop, &QEventLoop::quit, Qt::QueuedConnection);
    }
    std::shared_ptr<AsyncWaitState> state_;
};

// Returns false on timeout, cancellation, target destruction or a failed start.
// This ends the wait, not the underlying service work. Late callbacks are inert
// only when they take a lease before reading/writing caller-owned storage.
template <typename Start>
bool run_async_wait(QObject* target, Start&& start, int timeout_ms = kAsyncWaitTimeoutMs,
                    std::function<bool()> is_cancelled = {}) {
    auto state = std::make_shared<AsyncWaitState>();
    state->deadline = timeout_ms > 0 ? QDeadlineTimer(timeout_ms) : QDeadlineTimer(QDeadlineTimer::Forever);
    AsyncWaitSignal signal(state);
    auto invoke_start = [start = std::forward<Start>(start), signal, is_cancelled]() mutable {
        auto lease = signal.lease();
        if (!lease.owns_lock())
            return;
        if (is_cancelled && is_cancelled()) {
            signal.abort();
            return;
        }
        try {
            start(signal);
        } catch (...) {
            LOG_ERROR("ThreadHelper", "Async start threw; releasing the call");
            signal.abort();
        }
    };
    const auto destroyed = target ? QObject::connect(target, &QObject::destroyed, [signal]() { signal.abort(); })
                                  : QMetaObject::Connection{};

    if (!target || QThread::currentThread() == target->thread()) {
        QEventLoop loop;
        {
            QMutexLocker lock(&state->mutex);
            state->loop = &loop;
        }
        invoke_start();
        bool closed;
        {
            std::unique_lock<QRecursiveMutex> lock(state->access);
            closed = state->closed;
        }
        if (!closed) {
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            if (timeout_ms > 0)
                timer.start(static_cast<int>(state->deadline.remainingTime()));
            QTimer cancellation;
            if (is_cancelled) {
                cancellation.setInterval(10);
                QObject::connect(&cancellation, &QTimer::timeout, &loop, [&]() {
                    if (is_cancelled())
                        loop.quit();
                });
                cancellation.start();
            }
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        {
            QMutexLocker lock(&state->mutex);
            state->loop = nullptr;
        }
    } else {
        if (!QMetaObject::invokeMethod(target, std::move(invoke_start), Qt::QueuedConnection))
            signal.abort();
        QMutexLocker lock(&state->mutex);
        while (true) {
            if (state->completed || state->deadline.hasExpired() || (is_cancelled && is_cancelled()))
                break;
            // A destroyed target / failed start closes the gate without a result.
            lock.unlock();
            bool closed;
            {
                std::unique_lock<QRecursiveMutex> access(state->access);
                closed = state->closed;
            }
            lock.relock();
            if (closed || state->completed)
                break;
            if (is_cancelled)
                state->condition.wait(&state->mutex, 10);
            else
                state->condition.wait(&state->mutex, state->deadline);
        }
    }
    // Synchronize with an executing start/callback before releasing its stack.
    signal.abort();
    QObject::disconnect(destroyed);
    QMutexLocker lock(&state->mutex);
    if (!state->completed)
        LOG_WARN("ThreadHelper", "Async wait ended without a result (timeout, cancellation or unavailable target)");
    return state->completed;
}

// Typed callback bridge. Unlike the old stub this forwards callback arguments
// and invokes on_done on the caller, only after a successful completion.
template <typename Start, typename OnDone>
bool run_async_callback_sync(QObject* target, Start&& start, OnDone&& on_done, int timeout_ms = kAsyncWaitTimeoutMs) {
    auto consume = std::make_shared<std::function<void()>>();
    const bool completed = run_async_wait(
        target,
        [start = std::forward<Start>(start), done = std::forward<OnDone>(on_done), consume](auto signal_done) mutable {
            start([done, consume, signal_done](auto... args) mutable {
                auto lease = signal_done.lease();
                if (!lease.owns_lock())
                    return;
                *consume = [done, values = std::make_tuple(std::move(args)...)]() mutable {
                    std::apply(done, std::move(values));
                };
                signal_done();
            });
        },
        timeout_ms);
    if (completed && *consume)
        (*consume)();
    return completed;
}

} // namespace fincept::mcp::tools::detail
