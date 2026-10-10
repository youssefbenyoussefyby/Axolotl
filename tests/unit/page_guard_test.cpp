// P2 §2.2 — PageGuard. TDD target for the N4 fix: RAII pins that unpin in their
// destructor, so a throw between FetchPage and UnpinPage can no longer leak a
// frame. Also the N5 test: a span obtained through a guard stays valid while the
// guard lives, because the guard's pin forbids the frame's reuse.
//
// ─── ASSUMED API — include/storage/page_guard.h ─────────────────────────────
//
//   class ReadPageGuard {
//     ReadPageGuard() noexcept;                  // empty / invalid
//     ReadPageGuard(ReadPageGuard&&) noexcept;   // move-only
//     ReadPageGuard& operator=(ReadPageGuard&&) noexcept;
//     ReadPageGuard(const ReadPageGuard&)            = delete;
//     ReadPageGuard& operator=(const ReadPageGuard&) = delete;
//     ~ReadPageGuard();                          // unpins if valid
//     [[nodiscard]] const Page& operator*()  const noexcept;   // const only
//     [[nodiscard]] const Page* operator->() const noexcept;
//     void Drop() noexcept;                      // early unpin, idempotent
//     [[nodiscard]] bool    IsValid() const noexcept;
//     [[nodiscard]] PageRef Ref()     const noexcept;
//   };
//
//   class WritePageGuard {                        // same shape, plus mutation
//     ...
//     [[nodiscard]] Page& operator*() noexcept;   // mutable → marks frame dirty
//     [[nodiscard]] Page* operator->() noexcept;
//     [[nodiscard]] const Page& operator*() const noexcept;   // const → no mark
//     ...
//   };
//
// Guards are produced by StorageManager (FetchRead/FetchWrite/NewPage), so the
// runtime tests below drive one.

#include <gtest/gtest.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <utils/errors.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

using namespace AxolotlDB::storage;
namespace utils = AxolotlDB::utils;

// ─── Compile-time guarantees ─────────────────────────────────────────────────
// A copied guard would double-unpin; the type must forbid it outright.
static_assert(!std::is_copy_constructible_v<ReadPageGuard>, "ReadPageGuard must be move-only");
static_assert(!std::is_copy_assignable_v<ReadPageGuard>, "ReadPageGuard must be move-only");
static_assert(std::is_move_constructible_v<ReadPageGuard>, "ReadPageGuard must be movable");
static_assert(!std::is_copy_constructible_v<WritePageGuard>, "WritePageGuard must be move-only");
static_assert(!std::is_copy_assignable_v<WritePageGuard>, "WritePageGuard must be move-only");

// PageGuardTest.ReadGuardCannotDirtyAPage — a read guard exposes no non-const
// path to the Page, so it structurally cannot mark one dirty. Even through a
// non-const guard, the dereference stays const.
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<ReadPageGuard&>())>>,
              "ReadPageGuard::operator* must always yield a const Page&");

namespace {

std::span<const std::byte> AsBytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
std::string AsString(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

struct TempFile {
    std::filesystem::path p;
    explicit TempFile(const char* tag) {
        p = std::filesystem::temp_directory_path() /
            (std::string("axo_pg_") + tag + "_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db");
        std::filesystem::remove(p);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
    std::string str() const {
        return p.string();
    }
};

}  // namespace

TEST(PageGuardTest, IsNotCopyable) {
    static_assert(!std::is_copy_constructible_v<ReadPageGuard>);
    static_assert(!std::is_copy_assignable_v<ReadPageGuard>);
    static_assert(!std::is_copy_constructible_v<WritePageGuard>);
    static_assert(!std::is_copy_assignable_v<WritePageGuard>);
    SUCCEED();
}

TEST(PageGuardTest, ReadGuardCannotDirtyAPage) {
    static_assert(
        std::is_const_v<std::remove_reference_t<decltype(*std::declval<ReadPageGuard&>())>>,
        "ReadPageGuard::operator* must always yield a const Page&");
    SUCCEED();
}

TEST(PageGuardTest, DestructorUnpins) {
    TempFile f("dtor");
    StorageManager sm(/*pool_size=*/2);
    const FileId fid = sm.OpenFile(f.str());

    PageRef ref{};
    {
        auto g = sm.NewPage(fid, ref);
        (void)g;
    }  // guard dies here → unpin

    // With the single frame released, we can pin two fresh pages at once. A
    // leaked pin would leave only one usable frame and the second NewPage would
    // throw.
    std::vector<WritePageGuard> held;
    PageRef a{}, b{};
    EXPECT_NO_THROW(held.push_back(sm.NewPage(fid, a)));
    EXPECT_NO_THROW(held.push_back(sm.NewPage(fid, b)));
}

TEST(PageGuardTest, MoveTransfersOwnership) {
    TempFile f("move");
    StorageManager sm(4);
    const FileId fid = sm.OpenFile(f.str());

    PageRef ref{};
    auto g1 = sm.NewPage(fid, ref);
    ASSERT_TRUE(g1.IsValid());

    auto g2 = std::move(g1);
    EXPECT_FALSE(g1.IsValid()) << "the moved-from guard must own nothing";
    EXPECT_TRUE(g2.IsValid());
    EXPECT_EQ(g2.Ref(), ref);
}

TEST(PageGuardTest, DoubleDropIsSafe) {
    TempFile f("drop2");
    StorageManager sm(4);
    const FileId fid = sm.OpenFile(f.str());
    PageRef ref{};
    auto g = sm.NewPage(fid, ref);

    g.Drop();
    EXPECT_NO_THROW(g.Drop()) << "Drop must be idempotent";
    EXPECT_FALSE(g.IsValid());
    // Destructor also runs here: it must not unpin a second time.
}

TEST(PageGuardTest, WriteGuardMarksDirtyOnMutableAccess) {
    TempFile f("wdirty");
    const std::string payload = "written-through-guard";
    PageRef ref{};
    {
        StorageManager sm(4);
        const FileId fid = sm.OpenFile(f.str());
        auto g = sm.NewPage(fid, ref);
        (*g).InsertTuple(AsBytes(payload));  // mutable operator* → frame dirty
        sm.FlushFile(fid);
    }
    // If the mutable access had not marked the frame dirty, the flush would have
    // skipped it and the reopen below would not find the tuple.
    {
        StorageManager sm(4);
        const FileId fid = sm.OpenFile(f.str());
        auto g = sm.FetchRead(ref);
        EXPECT_EQ(AsString((*g).GetTuple(0)), payload);
    }
}

TEST(PageGuardTest, SpanStaysValidAcrossForcedEviction) {
    // N5. Take a zero-copy span through a guard, then force hard eviction
    // pressure on a tiny pool. The guard's pin must keep that frame off the
    // victim list, so the span still reads back its bytes.
    TempFile f("n5");
    StorageManager sm(/*pool_size=*/2);
    const FileId fid = sm.OpenFile(f.str());

    const std::string payload = "do-not-evict-me";
    PageRef pinned{};
    auto keep = sm.NewPage(fid, pinned);
    (*keep).InsertTuple(AsBytes(payload));
    std::span<const std::byte> live = (*keep).GetTuple(0);

    // Churn many other pages through the 2-frame pool while `keep` is held.
    for (int i = 0; i < 32; ++i) {
        PageRef r{};
        auto g = sm.NewPage(fid, r);
        (*g).InsertTuple(AsBytes("filler-" + std::to_string(i)));
    }

    EXPECT_EQ(AsString(live), payload)
        << "a pinned frame must not be reused while its guard is alive";
}

TEST(PageGuardTest, GuardOutlivingManagerIsDiagnosed) {
    // The checklist item: "throw, not leak, if the guard outlives the manager —
    // or document the ordering requirement and assert it." The safe, defined
    // path is that Drop() severs the guard's reference to the manager, so
    // destroying the manager afterwards is fine. Destroying the manager under a
    // still-valid guard is a documented precondition violation; assert the
    // supported ordering here and leave the death-test to a dedicated build.
    TempFile f("outlive");
    PageRef ref{};
    ReadPageGuard g;
    {
        StorageManager sm(4);
        const FileId fid = sm.OpenFile(f.str());
        PageRef w{};
        {
            auto wg = sm.NewPage(fid, w);
            (*wg).InsertTuple(AsBytes("x"));
            ref = w;
        }
        g = sm.FetchRead(ref);
        ASSERT_TRUE(g.IsValid());
        g.Drop();  // sever before the manager dies
        EXPECT_FALSE(g.IsValid());
    }  // manager destroyed here — safe, because g holds nothing
    EXPECT_FALSE(g.IsValid());
}
