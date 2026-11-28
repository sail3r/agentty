// SPDX-License-Identifier: Apache-2.0
//
// persistence_race_test.cpp — drive the REAL async save queue from many
// threads, under ThreadSanitizer.
//
// race_harness_test models the write-behind shape; this one exercises the
// actual code: persistence::save_thread's coalescing map, its lazily-started
// worker, thread_index_mu, and the offset/blob bookkeeping save_thread_sync
// performs. The concurrency that matters here is real — the reducer saves on
// the UI thread while the worker fsyncs, and `agentty acp` saves from several
// session workers at once.
//
// Invariants pinned:
//   • no data race (that is TSan's job; this file just supplies the pressure)
//   • every distinct thread id survives a concurrent save storm
//   • flush drains rather than truncates
//   • a save issued AFTER flush still lands (the write-behind queue restarts
//     its worker rather than silently dropping late writes — the shape that
//     left settings_cache's worker joinable and aborted at exit)

#include "agentty/io/persistence.hpp"
#include "agtest_cap.hpp"

#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

constexpr int kThreads = 6;
constexpr int kSaves   = 25;

agentty::Thread make_thread(int id, int gen) {
    agentty::Thread t;
    t.id    = agentty::ThreadId{"race-" + std::to_string(id)};
    t.title = "gen " + std::to_string(gen);
    agentty::Message m;
    m.role = agentty::Role::User;
    m.text = "body " + std::to_string(gen);
    t.messages.push_back(std::move(m));
    return t;
}

int count_saved() {
    int n = 0;
    for (const auto& t : agentty::persistence::load_all_threads(::agentty::IoAccess::grant(), agtest::cap()))
        if (t.id.value.starts_with("race-")) ++n;
    return n;
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() /
        ("agentty_persistence_race_" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
    fs::create_directories(root);
#ifdef _WIN32
    _putenv_s("AGENTTY_HOME", root.string().c_str());
#else
    ::setenv("AGENTTY_HOME", root.string().c_str(), 1);
#endif

    std::printf("=== persistence_race_test ===\n");

    // ── Concurrent save storm ────────────────────────────────────────────
    // Every thread hammers its OWN id, so coalescing is free to drop
    // intermediate generations but must never drop an id entirely.
    {
        std::barrier sync(kThreads);
        std::vector<std::thread> ts;
        std::atomic<int> threw{0};
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&, t] {
                sync.arrive_and_wait();
                for (int i = 0; i < kSaves; ++i) {
                    try { agentty::persistence::save_thread(::agentty::IoAccess::grant(), agtest::cap(), make_thread(t, i)); }
                    catch (...) { threw.fetch_add(1); }
                }
            });
        }
        for (auto& t : ts) t.join();
        agentty::persistence::flush_pending_saves(::agentty::IoAccess::grant(), agtest::cap());

        check(threw.load() == 0, "save_thread threw under concurrency");
        check(count_saved() == kThreads, "a thread id was lost in the storm");
        std::printf("  save storm: %d/%d ids landed\n", count_saved(), kThreads);
    }

    // ── A save AFTER flush still lands ───────────────────────────────────
    // flush_and_stop() latches `stopping`, so the write-behind queue has to
    // restart its worker for late writes rather than drop them. A late save
    // is not hypothetical: the Quit reducer issues one, and ACP sessions can
    // save after the TUI has flushed.
    {
        agentty::persistence::save_thread(::agentty::IoAccess::grant(), agtest::cap(), make_thread(999, 0));
        agentty::persistence::flush_pending_saves(::agentty::IoAccess::grant(), agtest::cap());
        const bool landed = std::ranges::any_of(
            agentty::persistence::load_all_threads(::agentty::IoAccess::grant(), agtest::cap()),
            [](const agentty::Thread& t) { return t.id.value == "race-999"; });
        check(landed, "a save issued after flush was dropped");
        std::printf("  post-flush save: %s\n", landed ? "persisted" : "DROPPED");
    }

    // ── Parallel loads while the thread is being saved ────────────────────
    // A whole-thread load parses its log on every core (ThreadLog::range,
    // jaal's parallel_for). Several readers do that at once while a writer
    // keeps growing the same thread under its lane: no race, and every load
    // sees a whole, in-order prefix of what was saved -- never a torn mix.
    {
        constexpr int kBase = 400, kGrowth = 40, kReaders = 4, kLoads = 6;
        agentty::Thread big;
        big.id = agentty::ThreadId{"race-big"};
        for (int i = 0; i < kBase; ++i) {
            agentty::Message m;
            m.role = (i % 2) ? agentty::Role::Assistant : agentty::Role::User;
            m.id   = agentty::MessageId{"m" + std::to_string(i)};
            m.text = "message " + std::to_string(i) + std::string(200, 'x');
            big.messages.push_back(std::move(m));
        }
        agentty::persistence::save_thread(::agentty::IoAccess::grant(), agtest::cap(), big);
        agentty::persistence::flush_pending_saves(::agentty::IoAccess::grant(), agtest::cap());

        std::atomic<int> bad{0}, loads{0};
        std::vector<std::thread> ts;
        ts.emplace_back([&] {                       // the writer
            agentty::Thread t = big;
            for (int g = 0; g < kGrowth; ++g) {
                agentty::Message m;
                m.role = agentty::Role::User;
                m.id   = agentty::MessageId{"g" + std::to_string(g)};
                m.text = "grown " + std::to_string(g);
                t.messages.push_back(std::move(m));
                agentty::persistence::save_thread(::agentty::IoAccess::grant(), agtest::cap(), t);
            }
        });
        for (int r = 0; r < kReaders; ++r) {
            ts.emplace_back([&] {
                for (int k = 0; k < kLoads; ++k) {
                    auto got = agentty::persistence::load_thread_by_id(
                        ::agentty::IoAccess::grant(), agtest::cap(), big.id);
                    loads.fetch_add(1);
                    if (!got || got->messages.size() < static_cast<std::size_t>(kBase)) { bad.fetch_add(1); continue; }
                    for (int i = 0; i < kBase; ++i)
                        if (got->messages[static_cast<std::size_t>(i)].id.value != "m" + std::to_string(i)) {
                            bad.fetch_add(1);
                            break;
                        }
                }
            });
        }
        for (auto& t : ts) t.join();
        agentty::persistence::flush_pending_saves(::agentty::IoAccess::grant(), agtest::cap());
        auto final_t = agentty::persistence::load_thread_by_id(::agentty::IoAccess::grant(), agtest::cap(), big.id);
        check(bad.load() == 0, "a parallel load returned a torn or out-of-order thread");
        check(final_t && final_t->messages.size() == static_cast<std::size_t>(kBase + kGrowth),
              "the final save did not land");
        std::printf("  parallel loads under saves: %d loads, %d bad\n", loads.load(), bad.load());
    }

    std::error_code ec;
    fs::remove_all(root, ec);

    if (failures == 0) { std::printf("PASS\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", failures);
    return 1;
}
