// SPDX-License-Identifier: GPL-2.0-or-later
// aln: the queue behind the pipeline cache's IO thread. One mutex guards the tasks, and the count
// is the queue's own size, so the two cannot disagree.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>

#include "common/polyfill_thread.h"

namespace Storage {

class RequestQueue {
public:
    using Task = std::function<void()>;

    /// False once the queue is closed; the task is then not kept.
    bool Submit(Task task) {
        {
            std::scoped_lock lock{mutex};
            if (closed) {
                return false;
            }
            tasks.push_back(std::move(task));
            ++submitted;
        }
        cv.notify_one();
        return true;
    }

    /// The worker loop: runs tasks until stopped or closed. A stop request (the owner going away)
    /// abandons what is queued; Close finishes what fits in its time.
    void Run(const std::stop_token& stoken) {
        for (;;) {
            Task task;
            {
                std::unique_lock lock{mutex};
                Common::CondvarWait(cv, lock, stoken, [this] { return closed || !tasks.empty(); });
                const bool expired = closed && Clock::now() >= deadline;
                if (stoken.stop_requested() || expired || (closed && tasks.empty())) {
                    dropped += tasks.size();
                    tasks.clear();
                    return;
                }
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            try {
                task();
                ++completed;
            } catch (...) {
                ++failed;
            }
        }
    }

    /// No more tasks are accepted; the worker finishes the queued ones for at most `drain`.
    void Close(std::chrono::milliseconds drain) {
        {
            std::scoped_lock lock{mutex};
            if (!closed) {
                closed = true;
                deadline = Clock::now() + drain;
            }
        }
        cv.notify_all();
    }

    [[nodiscard]] std::size_t Pending() const {
        std::scoped_lock lock{mutex};
        return tasks.size();
    }
    [[nodiscard]] std::uint64_t Submitted() const {
        std::scoped_lock lock{mutex};
        return submitted;
    }
    [[nodiscard]] std::uint64_t Completed() const {
        return completed;
    }
    [[nodiscard]] std::uint64_t Failed() const {
        return failed;
    }
    [[nodiscard]] std::uint64_t Dropped() const {
        std::scoped_lock lock{mutex};
        return dropped;
    }

private:
    using Clock = std::chrono::steady_clock;

    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Task> tasks;
    Clock::time_point deadline{};
    std::uint64_t submitted{};
    std::uint64_t dropped{};
    bool closed{};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> failed{0};
};

} // namespace Storage
