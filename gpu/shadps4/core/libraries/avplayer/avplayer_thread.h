// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>
#include <functional>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::AvPlayer {

// The player's worker threads call the game's event, memory and file callbacks, so each one gets a guest TCB first.
class GuestThread {
public:
    GuestThread() = default;
    ~GuestThread() {
        Stop();
    }

    void Run(std::function<void(std::stop_token)>&& func) {
        Stop();
        std::lock_guard lock(mutex);
        released = false;
        thread = std::jthread([func = std::move(func)](std::stop_token stop) {
            runtime_thread_attach_host("AvPlayer");
            func(stop);
        });
        stop_source = thread.get_stop_source();
    }

    // The decoders and the demuxer join themselves on exit. libc++'s Win32 join would wait on the thread's own handle
    // forever, so a self-join only marks the thread released, as upstream's pthread join does.
    void Join() {
        std::unique_lock lock(mutex);
        if (!thread.joinable() || released) {
            return;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            released = true;
            return;
        }
        std::jthread joined = std::move(thread);
        lock.unlock();
        Reap(joined);
    }

    bool Joinable() const {
        std::lock_guard lock(mutex);
        return thread.joinable() && !released;
    }

    // The stop request also reaches a thread that another thread's Join() already took (the demuxer joins the
    // decoders at EOF), so that join finishes.
    void Stop() {
        std::unique_lock lock(mutex);
        stop_source.request_stop();
        if (!thread.joinable()) {
            return;
        }
        std::jthread stopped = std::move(thread);
        released = false;
        lock.unlock();
        Reap(stopped);
    }

private:
    // Guest code calls into the player and cannot unwind a C++ exception.
    static void Reap(std::jthread& t) {
        try {
            if (t.get_id() == std::this_thread::get_id()) {
                t.detach();
            } else {
                t.join();
            }
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "AvPlayer: thread join failed (%s)\n", error.what());
            if (t.joinable()) {
                t.detach();
            }
        }
    }

    mutable std::mutex mutex;
    std::jthread thread;
    std::stop_source stop_source{std::nostopstate};
    bool released = false;
};

} // namespace Libraries::AvPlayer
