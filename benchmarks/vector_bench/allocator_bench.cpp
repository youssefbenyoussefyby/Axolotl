#include <utils/memory.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using Clock = std::chrono::steady_clock;

template <typename Fn>
double RunBenchmark(const char* name, std::size_t iterations, Fn&& fn) {
    auto start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        fn();
    }
    const auto end = Clock::now();
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    const double per_op_ns =
        (static_cast<double>(elapsed_us) * 1000.0) / static_cast<double>(iterations);
    std::cout << name << ": total_us=" << elapsed_us << " per_op_ns=" << per_op_ns << '\n';
    return per_op_ns;
}

}  // namespace

int main() {
    constexpr std::size_t kIterations = 400000;
    std::cout << "allocator_bench iterations=" << kIterations << '\n';

    volatile std::size_t sink = 0;

    const double raw_new_delete = RunBenchmark("raw_new_delete", kIterations, [&]() {
        auto* p = new std::byte[64];
        sink += static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) & 1U);
        delete[] p;
    });

    AxolotlDB::utils::Allocator raw_allocator({.enable_tracking = false, .enable_guards = false});
    const double allocator_raw = RunBenchmark("allocator_raw", kIterations, [&]() {
        auto* p = static_cast<std::byte*>(raw_allocator.AllocateBytes(64));
        sink += static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) & 1U);
        raw_allocator.DeallocateBytes(p);
    });

    AxolotlDB::utils::Allocator tracking_allocator(
        {.enable_tracking = true, .enable_guards = false});
    const double allocator_tracking = RunBenchmark("allocator_tracking", kIterations, [&]() {
        auto* p = static_cast<std::byte*>(tracking_allocator.AllocateBytes(64));
        sink += static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) & 1U);
        tracking_allocator.DeallocateBytes(p);
    });

    AxolotlDB::utils::Allocator guard_allocator({.enable_tracking = true, .enable_guards = true});
    const double allocator_tracking_guards =
        RunBenchmark("allocator_tracking_guards", kIterations, [&]() {
            auto* p = static_cast<std::byte*>(guard_allocator.AllocateBytes(64));
            sink += static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) & 1U);
            guard_allocator.DeallocateBytes(p);
        });

    std::cout << "sink=" << sink << '\n';
    std::cout << "ratio allocator_raw/raw_new_delete=" << (allocator_raw / raw_new_delete) << '\n';
    std::cout << "ratio allocator_tracking/raw_new_delete=" << (allocator_tracking / raw_new_delete)
              << '\n';
    std::cout << "ratio allocator_tracking_guards/raw_new_delete="
              << (allocator_tracking_guards / raw_new_delete) << '\n';
    return 0;
}
