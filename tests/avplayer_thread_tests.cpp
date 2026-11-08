// AvPlayer worker threads join themselves on exit and the demuxer joins the decoders; none of it may hang.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/libraries/avplayer/avplayer_thread.h"

extern "C" void runtime_thread_attach_host(const char*) {}

using Libraries::AvPlayer::GuestThread;
using namespace std::chrono_literals;

static int failures;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

template <class F>
static void within(const char* name, F&& body) {
    std::mutex m;
    std::condition_variable done_cv;
    bool done = false;
    std::thread runner([&] {
        body();
        std::lock_guard lock(m);
        done = true;
        done_cv.notify_all();
    });
    std::unique_lock lock(m);
    if (!done_cv.wait_for(lock, 5s, [&] { return done; })) {
        std::printf("FAIL: %s hangs\n", name);
        std::fflush(stdout);
        std::_Exit(1);
    }
    lock.unlock();
    runner.join();
    std::printf("ok: %s\n", name);
}

int main() {
    within("a thread that joins itself on exit", [] {
        GuestThread worker;
        std::atomic<bool> exited{false};
        worker.Run([&](std::stop_token) {
            worker.Join();
            exited = true;
        });
        while (!exited) std::this_thread::sleep_for(1ms);
        check(!worker.Joinable(), "a self-joined thread still counts as running");
        worker.Stop();
    });

    within("demuxer joins self-joining decoders at end of file", [] {
        GuestThread demuxer, decoder;
        std::atomic<bool> eof{false};
        decoder.Run([&](std::stop_token stop) {
            while (!eof && !stop.stop_requested()) std::this_thread::sleep_for(1ms);
            decoder.Join();
        });
        demuxer.Run([&](std::stop_token) {
            eof = true;
            decoder.Join();
            demuxer.Join();
        });
        std::this_thread::sleep_for(20ms);
        decoder.Stop();
        demuxer.Stop();
        check(!decoder.Joinable() && !demuxer.Joinable(), "threads still running after Stop");
    });

    within("Stop reaches a thread another thread is joining", [] {
        GuestThread demuxer, decoder;
        std::mutex m;
        std::condition_variable_any cv;
        decoder.Run([&](std::stop_token stop) {
            std::unique_lock lock(m);
            cv.wait(lock, stop, [] { return false; });
        });
        std::atomic<bool> joined{false};
        demuxer.Run([&](std::stop_token) {
            decoder.Join();
            joined = true;
        });
        std::this_thread::sleep_for(20ms);
        decoder.Stop();
        demuxer.Stop();
        check(joined, "the demuxer's join did not finish");
    });

    within("the destructor reaps a released thread", [] {
        auto* worker = new GuestThread;
        std::atomic<bool> exited{false};
        worker->Run([&](std::stop_token) {
            worker->Join();
            exited = true;
        });
        while (!exited) std::this_thread::sleep_for(1ms);
        delete worker;
    });

    within("a thread restarted after a self-join", [] {
        GuestThread worker;
        for (int round = 0; round < 3; ++round) {
            std::atomic<bool> exited{false};
            worker.Run([&](std::stop_token) {
                worker.Join();
                exited = true;
            });
            while (!exited) std::this_thread::sleep_for(1ms);
        }
        worker.Stop();
    });

    std::printf(failures ? "%d failure(s)\n" : "all AvPlayer thread tests passed\n", failures);
    return failures ? 1 : 0;
}
