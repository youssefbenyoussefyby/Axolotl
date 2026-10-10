// benchmarks/vector_bench/allocate_page_bench.cpp
//
// N10 Regression Benchmark: AllocatePage flush/sync is O(1), not O(n).
//
// audit.md N10 and P0.md §0.6 document the bug:
//   AllocatePage called file_.flush() on every invocation, making a loop of
//   10,000 NewPage calls fire 10,000 OS flushes.  The fix removed the per-call
//   flush so that durability is only guaranteed after an explicit Sync().
//
// This program has two modes, both run on every invocation:
//
// 1. CORRECTNESS GATE (hard failure, non-zero exit)
//    Uses the IFileIO injection seam (same CountingIO mock as
//    storage_manager_test.cpp) to count Sync() calls during the allocation
//    loop.  Asserts:
//      * sync_calls == 0  after 10,000 NewPage calls  (O(1) = zero in loop)
//      * sync_calls == 1  after one explicit FlushAll() call
//    Any O(n) result exits with code 1.
//
// 2. THROUGHPUT MEASUREMENT (informational, always printed)
//    Allocates 10,000 pages through the real StorageManager (disk I/O) and
//    reports wall-clock time and pages/second.  Compare against the pre-fix
//    baseline via git history: before N10 the same loop would fire 10,000
//    fsync/FlushFileBuffers calls and be orders of magnitude slower.

#include <storage/file_io.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <utils/errors.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace AxolotlDB::storage;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// CountingIO — in-memory IFileIO that records how many times Sync() is called.
// Matches the mock used in storage_manager_test.cpp but extended with a
// public allocate_calls counter for extra visibility.
// ---------------------------------------------------------------------------
struct CountingIO : IFileIO {
    std::map<PageId, std::array<std::byte, PAGE_SIZE>> pages;
    std::atomic<int> sync_calls{0};
    std::atomic<int> allocate_calls{0};
    std::atomic<int> write_calls{0};

    PageId AllocatePage() override {
        ++allocate_calls;
        const PageId id = static_cast<PageId>(pages.size());
        pages[id];  // zero-filled default
        return id;
    }
    void ReadPage(PageId id, Page& out) override {
        out = Page(pages.at(id));
    }
    void WritePage(PageId id, const Page& p) override {
        ++write_calls;
        pages[id] = p.RawData();
    }
    PageId PageCount() const override {
        return static_cast<PageId>(pages.size());
    }
    void Sync() override {
        ++sync_calls;
    }
};

struct IORegistry {
    std::map<std::string, CountingIO*> by_path;
    FileIOFactory factory() {
        return [this](const std::string& path) {
            auto io = std::make_unique<CountingIO>();
            by_path[path] = io.get();
            return std::unique_ptr<IFileIO>(std::move(io));
        };
    }
};

// ---------------------------------------------------------------------------
// Gate: assert sync count.  Returns false and prints a failure message if the
// assertion fails so the caller can set the exit code.
// ---------------------------------------------------------------------------
static bool AssertSyncCount(const std::string& label, int actual, int expected) {
    if (actual == expected) {
        std::cout << "  [OK] " << label << ": sync_calls=" << actual << "\n";
        return true;
    }
    std::cerr << "\n[GATE FAILED] " << label << "\n"
              << "  Expected sync_calls = " << expected << "\n"
              << "  Actual   sync_calls = " << actual << "\n"
              << "  This is the N10 regression: AllocatePage is calling Sync()\n"
              << "  per invocation (O(n)) instead of zero times (O(1)).\n"
              << "  See audit.md N10 and docs/phases/P0.md §0.6.\n\n";
    return false;
}

// ---------------------------------------------------------------------------
// Part 1: Correctness gate via injected mock (no disk I/O).
// ---------------------------------------------------------------------------
static bool RunCorrectnessGate(int n_pages) {
    std::cout << "=== CORRECTNESS GATE (mock I/O, N10) ===\n";
    std::cout << "Allocating " << n_pages << " pages via StorageManager::NewPage...\n";

    IORegistry reg;
    StorageManager sm(/*pool_size=*/static_cast<std::size_t>(n_pages) + 4, reg.factory());
    const FileId fid = sm.OpenFile("mock://allocate_bench");
    CountingIO* io = reg.by_path.at("mock://allocate_bench");

    // Drop guards immediately — we only care about allocation, not pinning.
    for (int i = 0; i < n_pages; ++i) {
        PageRef ref{};
        auto g = sm.NewPage(fid, ref);
        g.Drop();
    }

    const int sync_during_loop = io->sync_calls.load();
    std::cout << "  allocate_calls = " << io->allocate_calls.load() << "\n";
    std::cout << "  write_calls    = " << io->write_calls.load() << "\n";
    std::cout << "  sync_calls     = " << sync_during_loop << "  (expected: 0)\n";

    bool ok = AssertSyncCount(
        "sync_calls during " + std::to_string(n_pages) + " NewPage calls must be 0 (O(1))",
        sync_during_loop, 0);

    // Now flush explicitly: exactly 1 Sync() must fire.
    sm.FlushAll();
    const int sync_after_flush = io->sync_calls.load();
    std::cout << "  sync_calls after FlushAll() = " << sync_after_flush << "  (expected: 1)\n";
    ok &= AssertSyncCount("sync_calls after explicit FlushAll() must be 1", sync_after_flush, 1);
    return ok;
}

// ---------------------------------------------------------------------------
// Part 2: Throughput measurement on real disk.
// ---------------------------------------------------------------------------
static void RunThroughputBenchmark(int n_pages, const fs::path& tmp_dir) {
    std::cout << "\n=== THROUGHPUT BENCHMARK (real disk I/O) ===\n";
    std::cout << "Allocating " << n_pages << " pages via StorageManager::NewPage (real file)...\n";

    const fs::path db = tmp_dir / "alloc_bench_throughput.db";
    std::error_code ec;
    fs::remove(db, ec);

    // Large pool so eviction doesn't add write syscalls to the measurement.
    StorageManager sm(static_cast<std::size_t>(n_pages) + 8);
    const FileId fid = sm.OpenFile(db.string());

    const auto t0 = Clock::now();
    for (int i = 0; i < n_pages; ++i) {
        PageRef ref{};
        auto g = sm.NewPage(fid, ref);
        g.Drop();
    }
    const auto t_loop = Clock::now();

    sm.FlushAll();
    const auto t_flush = Clock::now();
    sm.CloseFile(fid);

    const double loop_ms = std::chrono::duration<double, std::milli>(t_loop - t0).count();
    const double flush_ms = std::chrono::duration<double, std::milli>(t_flush - t_loop).count();
    const double pages_per_sec = (static_cast<double>(n_pages) / loop_ms) * 1000.0;

    std::cout << "  Loop (NewPage x" << n_pages << ")  : " << loop_ms << " ms\n";
    std::cout << "  FlushAll (1 sync) : " << flush_ms << " ms\n";
    std::cout << "  Throughput        : " << static_cast<long long>(pages_per_sec)
              << " pages/sec\n";
    std::cout << "  Per-page latency  : " << (loop_ms * 1e6 / n_pages) << " ns\n";

    const double total_mb = (static_cast<double>(n_pages) * PAGE_SIZE) / (1024.0 * 1024.0);
    std::cout << "  Data written      : " << total_mb << " MiB\n";

    // Pre-fix baseline context (from audit.md N10): with per-call file_.flush()
    // each of these 10,000 allocations fired an OS flush, making the loop
    // effectively serialised on I/O.  On a typical NVMe drive that was ~10,000
    // x 0.1ms = ~1000ms.  The current code should be << 100ms since OS buffers
    // absorb the writes and only one flush() fires at FlushAll().
    if (loop_ms > 500.0) {
        std::cerr << "\n[PERF WARNING] Loop took " << loop_ms << " ms for " << n_pages
                  << " pages.\n"
                  << "  Pre-fix N10 code would take ~1000ms on NVMe due to\n"
                  << "  per-call file_.flush().  If you are seeing this on\n"
                  << "  fast hardware, suspect an O(n) flush regression.\n";
    }

    fs::remove(db, ec);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    constexpr int kPages = 10000;

    const fs::path tmp_dir = fs::temp_directory_path() / "axo_alloc_bench";
    fs::create_directories(tmp_dir);

    std::cout << "N10 AllocatePage Flush Regression Benchmark\n";
    std::cout << "============================================\n";
    std::cout << "Claim (audit.md N10 + P0.md §0.6):\n";
    std::cout << "  AllocatePage must NOT call Sync/flush per invocation.\n";
    std::cout << "  sync_calls during " << kPages << " NewPage calls must be 0 (O(1)), not "
              << kPages << " (O(n)).\n\n";

    const bool gate_passed = RunCorrectnessGate(kPages);
    RunThroughputBenchmark(kPages, tmp_dir);

    std::cout << "\n=== SUMMARY ===\n";
    if (gate_passed) {
        std::cout << "[PASS] N10 O(1) flush gate: AllocatePage does not sync "
                     "per call.\n";
        return 0;
    } else {
        std::cerr << "[FAIL] N10 O(1) flush gate FAILED.\n"
                  << "  AllocatePage is syncing on every call (O(n) behaviour).\n"
                  << "  See audit.md N10 and docs/phases/P0.md §0.6 for the fix.\n";
        return 1;
    }
}
