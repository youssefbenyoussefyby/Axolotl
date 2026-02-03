#include <utils/memory.h>

#include <cstdint>
#include <limits>
#include <new>

namespace AxolotlDB::utils {
namespace {

constexpr std::uint64_t kHeadGuard = 0xA110C0DECAFEBABELL;
constexpr std::uint64_t kTailGuard = 0xBADF00D0D15EA5E5ULL;

struct alignas(std::max_align_t) AllocationHeader {
    std::size_t size = 0;
    std::uint64_t head_guard = kHeadGuard;
};

struct AllocationFooter {
    std::uint64_t tail_guard = kTailGuard;
};

static_assert((sizeof(AllocationHeader) % alignof(std::max_align_t)) == 0,
              "AllocationHeader must preserve max alignment.");

inline bool WouldOverflow(std::size_t lhs, std::size_t rhs) {
    return lhs > (std::numeric_limits<std::size_t>::max() - rhs);
}

// Distance from the base allocation to the user pointer, in the tracked/guarded
// layout.
//
// The header is always written immediately before the user pointer so
// DeallocateBytes can find it by subtracting sizeof(AllocationHeader). But for
// an over-aligned request the user pointer itself must sit on an `alignment`
// boundary, so the gap in front of it has to be rounded up to a multiple of
// `alignment` -- otherwise base+sizeof(header) lands off-boundary even when
// base is correctly aligned. Deallocate recomputes this from the same
// `alignment` argument, so no extra bookkeeping is stored.
inline std::size_t UserOffset(std::size_t alignment) {
    const std::size_t h = sizeof(AllocationHeader);
    if (alignment <= alignof(std::max_align_t)) {
        return h;  // header is alignas(max_align_t) and a whole multiple of it
    }
    return ((h + alignment - 1) / alignment) * alignment;
}

// Alignment actually handed to ::operator new. Never weaken the header's own
// requirement, even if the caller asks for less.
inline std::align_val_t BaseAlignment(std::size_t alignment) {
    return std::align_val_t{alignment < alignof(AllocationHeader) ? alignof(AllocationHeader)
                                                                  : alignment};
}

Allocator& FallbackAllocator() {
    static Allocator allocator{};
    return allocator;
}

std::atomic<IAllocator*>& DefaultAllocatorPtr() {
    static std::atomic<IAllocator*> allocator_ptr{&FallbackAllocator()};
    return allocator_ptr;
}

}  // namespace

Allocator::Allocator(AllocatorOptions options) : options_(options) {}

void* Allocator::AllocateBytes(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) {
        bytes = 1;
    }
    if (alignment == 0) {
        alignment = alignof(std::max_align_t);
    }

    if (!options_.enable_tracking && !options_.enable_guards) {
        return ::operator new(bytes, BaseAlignment(alignment));
    }

    const std::size_t user_offset = UserOffset(alignment);
    if (WouldOverflow(user_offset, bytes) ||
        (options_.enable_guards && WouldOverflow(user_offset + bytes, sizeof(AllocationFooter)))) {
        throw std::bad_alloc();
    }

    std::size_t total_size = user_offset + bytes;
    if (options_.enable_guards) {
        total_size += sizeof(AllocationFooter);
    }

    auto* base = static_cast<std::byte*>(::operator new(total_size, BaseAlignment(alignment)));
    auto* user_ptr = base + user_offset;

    // Header sits immediately before the user pointer, not at `base` -- for an
    // over-aligned request those differ by the padding UserOffset added.
    auto* header = reinterpret_cast<AllocationHeader*>(user_ptr - sizeof(AllocationHeader));
    header->size = bytes;
    header->head_guard = kHeadGuard;

    if (options_.enable_guards) {
        auto* footer = reinterpret_cast<AllocationFooter*>(user_ptr + bytes);
        footer->tail_guard = kTailGuard;
    }

    stats_.allocation_calls.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_allocated.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    const std::uint64_t live_now =
        stats_.bytes_live.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed) +
        static_cast<std::uint64_t>(bytes);
    UpdatePeakLive(stats_.peak_bytes_live, live_now);
    return static_cast<void*>(user_ptr);
}

void Allocator::DeallocateBytes(void* ptr, std::size_t alignment) noexcept {
    if (ptr == nullptr) {
        return;
    }
    if (alignment == 0) {
        alignment = alignof(std::max_align_t);
    }

    if (!options_.enable_tracking && !options_.enable_guards) {
        // Must match the aligned form used to allocate: mixing the sized and
        // aligned operator delete overloads is UB.
        ::operator delete(ptr, BaseAlignment(alignment));
        return;
    }

    auto* user_ptr = static_cast<std::byte*>(ptr);
    auto* header = reinterpret_cast<AllocationHeader*>(user_ptr - sizeof(AllocationHeader));
    const std::size_t bytes = header->size;

    bool guards_ok = true;
    if (options_.enable_guards) {
        guards_ok = guards_ok && (header->head_guard == kHeadGuard);
        auto* footer = reinterpret_cast<AllocationFooter*>(user_ptr + bytes);
        guards_ok = guards_ok && (footer->tail_guard == kTailGuard);
        if (!guards_ok) {
            stats_.guard_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    stats_.deallocation_calls.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_deallocated.fetch_add(static_cast<std::uint64_t>(bytes),
                                       std::memory_order_relaxed);
    stats_.bytes_live.fetch_sub(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    // Free the base of the allocation, which for an over-aligned request is
    // further back than the header -- recompute the same padding Allocate used.
    ::operator delete(static_cast<void*>(user_ptr - UserOffset(alignment)),
                      BaseAlignment(alignment));
}

MemoryStats Allocator::GetStats() const noexcept {
    MemoryStats out{};
    out.allocation_calls = stats_.allocation_calls.load(std::memory_order_relaxed);
    out.deallocation_calls = stats_.deallocation_calls.load(std::memory_order_relaxed);
    out.bytes_allocated = stats_.bytes_allocated.load(std::memory_order_relaxed);
    out.bytes_deallocated = stats_.bytes_deallocated.load(std::memory_order_relaxed);
    out.bytes_live = stats_.bytes_live.load(std::memory_order_relaxed);
    out.peak_bytes_live = stats_.peak_bytes_live.load(std::memory_order_relaxed);
    out.guard_failures = stats_.guard_failures.load(std::memory_order_relaxed);
    return out;
}

void Allocator::ResetStats() noexcept {
    stats_.allocation_calls.store(0, std::memory_order_relaxed);
    stats_.deallocation_calls.store(0, std::memory_order_relaxed);
    stats_.bytes_allocated.store(0, std::memory_order_relaxed);
    stats_.bytes_deallocated.store(0, std::memory_order_relaxed);
    stats_.bytes_live.store(0, std::memory_order_relaxed);
    stats_.peak_bytes_live.store(0, std::memory_order_relaxed);
    stats_.guard_failures.store(0, std::memory_order_relaxed);
}

const AllocatorOptions& Allocator::GetOptions() const noexcept {
    return options_;
}

void Allocator::SetOptions(AllocatorOptions options) noexcept {
    options_ = options;
}

void Allocator::UpdatePeakLive(std::atomic<std::uint64_t>& peak_live,
                               std::uint64_t current_live) noexcept {
    std::uint64_t observed = peak_live.load(std::memory_order_relaxed);
    while (observed < current_live &&
           !peak_live.compare_exchange_weak(observed, current_live, std::memory_order_relaxed)) {
    }
}

IAllocator& DefaultAllocator() noexcept {
    IAllocator* current = DefaultAllocatorPtr().load(std::memory_order_acquire);
    return *current;
}

void SetDefaultAllocator(IAllocator* allocator) noexcept {
    DefaultAllocatorPtr().store(
        allocator == nullptr ? static_cast<IAllocator*>(&FallbackAllocator()) : allocator,
        std::memory_order_release);
}

}  // namespace AxolotlDB::utils
