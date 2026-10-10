#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace AxolotlDB::utils {

struct MemoryStats {
    std::uint64_t allocation_calls = 0;
    std::uint64_t deallocation_calls = 0;
    std::uint64_t bytes_allocated = 0;
    std::uint64_t bytes_deallocated = 0;
    std::uint64_t bytes_live = 0;
    std::uint64_t peak_bytes_live = 0;
    std::uint64_t guard_failures = 0;
};

struct AllocatorOptions {
    bool enable_tracking = true;
    bool enable_guards = false;
};

class IAllocator {
   public:
    virtual ~IAllocator() = default;

    virtual void* AllocateBytes(std::size_t bytes,
                                std::size_t alignment = alignof(std::max_align_t)) = 0;
    virtual void DeallocateBytes(void* ptr,
                                 std::size_t alignment = alignof(std::max_align_t)) noexcept = 0;

    template <typename T, typename... Args>
    T* New(Args&&... args) {
        static_assert(!std::is_array_v<T>, "Use AllocateBytes for array allocations.");
        void* raw = AllocateBytes(sizeof(T), alignof(T));
        return new (raw) T(std::forward<Args>(args)...);
    }

    template <typename T>
    void Delete(T* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }
        ptr->~T();
        DeallocateBytes(static_cast<void*>(ptr), alignof(T));
    }
};

class Allocator final : public IAllocator {
   public:
    explicit Allocator(AllocatorOptions options = {});

    void* AllocateBytes(std::size_t bytes,
                        std::size_t alignment = alignof(std::max_align_t)) override;
    void DeallocateBytes(void* ptr,
                         std::size_t alignment = alignof(std::max_align_t)) noexcept override;

    MemoryStats GetStats() const noexcept;
    void ResetStats() noexcept;

    const AllocatorOptions& GetOptions() const noexcept;
    void SetOptions(AllocatorOptions options) noexcept;

   private:
    struct AtomicStats {
        std::atomic<std::uint64_t> allocation_calls{0};
        std::atomic<std::uint64_t> deallocation_calls{0};
        std::atomic<std::uint64_t> bytes_allocated{0};
        std::atomic<std::uint64_t> bytes_deallocated{0};
        std::atomic<std::uint64_t> bytes_live{0};
        std::atomic<std::uint64_t> peak_bytes_live{0};
        std::atomic<std::uint64_t> guard_failures{0};
    };

    static void UpdatePeakLive(std::atomic<std::uint64_t>& peak_live,
                               std::uint64_t current_live) noexcept;

    AllocatorOptions options_;
    AtomicStats stats_;
};

IAllocator& DefaultAllocator() noexcept;
void SetDefaultAllocator(IAllocator* allocator) noexcept;

}  // namespace AxolotlDB::utils
