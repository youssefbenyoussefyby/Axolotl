// P2 §2.8 — Tuple format version byte. A leading `ver` byte on the wire format
// so a future field change is detectable rather than silently misread. Spending
// one byte is only worth it if an unknown version is actually rejected — both
// halves are tested.
//
// ─── ASSUMED additions to include/storage/tuple.h + src/storage/tuple.cpp ───
//
//   inline constexpr uint8_t TUPLE_FORMAT_VERSION = 1;   // current version
//   // Serialize writes TUPLE_FORMAT_VERSION as byte 0, before anything else.
//   // Deserialize reads byte 0 and throws StorageException on an unknown value.

#include <gtest/gtest.h>
#include <storage/tuple.h>
#include <utils/errors.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace AxolotlDB::storage;
namespace utils = AxolotlDB::utils;

namespace {

Schema OneInt() {
    return Schema{{ColumnDef{"a", ColumnType::INT32}}};
}

}  // namespace

TEST(TupleTest, SerializeWritesVersionByteFirst) {
    // Raw-byte assertion, independent of Deserialize (P1's lesson 3: never let
    // the reader vouch for the writer).
    const std::vector<std::byte> bytes = Tuple::Serialize(OneInt(), {Value::Int32(7)});
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes[0], std::byte{TUPLE_FORMAT_VERSION})
        << "the format version must be the very first byte on the wire";
}

TEST(TupleTest, UnknownVersionByteThrows) {
    std::vector<std::byte> bytes = Tuple::Serialize(OneInt(), {Value::Int32(7)});
    ASSERT_FALSE(bytes.empty());
    bytes[0] = std::byte{0xFF};  // a version this build has never heard of

    EXPECT_THROW((void)Tuple::Deserialize(OneInt(), bytes), utils::StorageException)
        << "an unknown version byte is the whole reason for spending the byte";
}
