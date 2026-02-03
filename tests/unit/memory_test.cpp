#include <gtest/gtest.h>
#include <utils/memory.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace AxolotlDB::utils {
namespace {

struct CountingObject {
    static inline int ctor_count = 0;
    static inline int dtor_count = 0;

    explicit CountingObject(int value_in) : value(value_in) {
        ++ctor_count;
    }
    ~CountingObject() {
        ++dtor_count;
    }

    int value;
};

TEST(MemoryAllocatorTest, TypedNewDeleteTracksLifecycle) {
    CountingObject::ctor_count = 0;
    CountingObject::dtor_count = 0;

    Allocator allocator{};
    auto* obj = allocator.New<CountingObject>(42);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->value, 42);
    EXPECT_EQ(CountingObject::ctor_count, 1);

    allocator.Delete(obj);
    EXPECT_EQ(CountingObject::dtor_count, 1);

    const MemoryStats stats = allocator.GetStats();
    EXPECT_EQ(stats.allocation_calls, 1);
    EXPECT_EQ(stats.deallocation_calls, 1);
    EXPECT_EQ(stats.bytes_live, 0);
    EXPECT_GE(stats.peak_bytes_live, sizeof(CountingObject));
}

TEST(MemoryAllocatorTest, GuardFailureIsTracked) {
    Allocator allocator({.enable_tracking = true, .enable_guards = true});

    auto* bytes = static_cast<unsigned char*>(allocator.AllocateBytes(16));
    ASSERT_NE(bytes, nullptr);
    bytes[16] = 0xFF;  // Corrupt the trailing guard.
    allocator.DeallocateBytes(bytes);

    const MemoryStats stats = allocator.GetStats();
    EXPECT_EQ(stats.guard_failures, 1);
}

TEST(MemoryAllocatorTest, RawModeSkipsTracking) {
    Allocator allocator({.enable_tracking = false, .enable_guards = false});

    void* ptr = allocator.AllocateBytes(64);
    ASSERT_NE(ptr, nullptr);
    allocator.DeallocateBytes(ptr);

    const MemoryStats stats = allocator.GetStats();
    EXPECT_EQ(stats.allocation_calls, 0);
    EXPECT_EQ(stats.deallocation_calls, 0);
    EXPECT_EQ(stats.bytes_allocated, 0);
}

TEST(MemoryAllocatorTest, DefaultAllocatorCanBeRebound) {
    Allocator custom{};
    SetDefaultAllocator(&custom);

    auto* value = DefaultAllocator().New<int>(7);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 7);
    DefaultAllocator().Delete(value);

    SetDefaultAllocator(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Audit finding B4: over-aligned allocations.
//
// AllocateBytes used to discard its `alignment` argument entirely
// ((void)alignment;) and always call plain ::operator new, which only
// guarantees alignof(std::max_align_t) -- typically 16. Any type declared
// alignas(32) or alignas(64) came back misaligned: UB, and an immediate crash
// on targets where aligned SIMD loads fault. Phase 5's HNSW work uses exactly
// such types, so this had to be right before any of it lands.
//
// The tracked/guarded path is the harder half: the header sits in front of the
// user pointer, so aligning only the base allocation is not enough -- the gap
// in front of the user pointer must itself be a multiple of the requested
// alignment.
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(64) OverAligned64 {
    float data[16];
};

bool IsAlignedTo(const void* p, std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(p) % alignment) == 0;
}

TEST(MemoryAllocatorTest, RawModeHonoursOverAlignment) {
    Allocator allocator({/*enable_tracking=*/false, /*enable_guards=*/false});
    for (std::size_t alignment : {32u, 64u, 128u, 256u}) {
        void* p = allocator.AllocateBytes(100, alignment);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(IsAlignedTo(p, alignment))
            << "raw-mode allocation not aligned to " << alignment;
        allocator.DeallocateBytes(p, alignment);
    }
}

TEST(MemoryAllocatorTest, TrackedModeHonoursOverAlignment) {
    Allocator allocator({/*enable_tracking=*/true, /*enable_guards=*/false});
    for (std::size_t alignment : {32u, 64u, 128u, 256u}) {
        void* p = allocator.AllocateBytes(100, alignment);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(IsAlignedTo(p, alignment))
            << "tracked-mode allocation not aligned to " << alignment;
        allocator.DeallocateBytes(p, alignment);
    }
    EXPECT_EQ(allocator.GetStats().bytes_live, 0u) << "tracked bytes leaked";
}

TEST(MemoryAllocatorTest, GuardedModeHonoursOverAlignmentAndKeepsGuardsIntact) {
    // Guards are the strictest case: the footer is placed at user_ptr + bytes,
    // so if the padding maths is wrong the footer lands somewhere else and the
    // guard check fails on free.
    Allocator allocator({/*enable_tracking=*/true, /*enable_guards=*/true});
    for (std::size_t alignment : {32u, 64u, 128u}) {
        void* p = allocator.AllocateBytes(100, alignment);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(IsAlignedTo(p, alignment)) << "guarded allocation not aligned to " << alignment;
        std::memset(p, 0xAB, 100);  // touch the whole user region
        allocator.DeallocateBytes(p, alignment);
    }
    const MemoryStats stats = allocator.GetStats();
    EXPECT_EQ(stats.guard_failures, 0u)
        << "guard corrupted -- padding maths put the footer in the wrong place";
    EXPECT_EQ(stats.bytes_live, 0u);
}

TEST(MemoryAllocatorTest, NewHonoursOverAlignedTypes) {
    // The end-to-end path callers actually use: New<T> forwards alignof(T).
    Allocator allocator({/*enable_tracking=*/true, /*enable_guards=*/true});
    auto* obj = allocator.New<OverAligned64>();
    ASSERT_NE(obj, nullptr);
    EXPECT_TRUE(IsAlignedTo(obj, alignof(OverAligned64)))
        << "New<T> returned memory misaligned for alignas(64) type";
    allocator.Delete(obj);
    EXPECT_EQ(allocator.GetStats().guard_failures, 0u);
}

}  // namespace
}  // namespace AxolotlDB::utils
