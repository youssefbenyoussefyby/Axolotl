// Regression tests for Tuple::Serialize / Tuple::Deserialize.
//
// These pin down the variable-length encoding contract -- see docs/audit.md
// findings A1 (var-length corruption around NULLs), A2 (missing bounds checks)
// and A8 (oversized values wrapping the uint16_t length table).
//
// Since B5, VARCHAR is the only variable-length column type. A VECTOR column
// stores an 8-byte VectorRef in the fixed region and the floats live in a
// VectorHeap, so the offset table no longer describes vector columns at all.
// The dimension check that used to live here now belongs to VectorHeap::Put,
// which owns the dimension -- see vector_heap_test.cpp.

#include <gtest/gtest.h>
#include <storage/tuple.h>
#include <storage/vector_ref.h>
#include <utils/errors.h>

#include <string>
#include <vector>

using namespace AxolotlDB::storage;

namespace {

Schema MakeSchema(std::vector<ColumnDef> cols) {
    Schema s;
    s.columns = std::move(cols);
    return s;
}

ColumnDef Varchar(const char* name) {
    return ColumnDef{name, ColumnType::VARCHAR, true, 0};
}

ColumnDef Vector(const char* name, uint32_t dim) {
    return ColumnDef{name, ColumnType::VECTOR, true, dim};
}

ColumnDef Int32Col(const char* name) {
    return ColumnDef{name, ColumnType::INT32, true, 0};
}

// Serialize then immediately deserialize.
std::vector<Value> RoundTrip(const Schema& schema, const std::vector<Value>& values) {
    auto bytes = Tuple::Serialize(schema, values);
    return Tuple::Deserialize(schema, std::span<const std::byte>(bytes));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Baseline: no NULLs at all. If this fails, the fix broke the common path.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleVarLength, TwoVarcharsNoNulls) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b")});
    auto out = RoundTrip(schema, {Value::Varchar("ab"), Value::Varchar("cd")});

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].AsVarchar(), "ab");
    EXPECT_EQ(out[1].AsVarchar(), "cd");
}

TEST(TupleVarLength, SingleVarchar) {
    auto schema = MakeSchema({Varchar("a")});
    auto out = RoundTrip(schema, {Value::Varchar("hello")});

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].AsVarchar(), "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// A1: NULL in each position. The middle case is the documented corruption.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleVarLength, NullBetweenTwoVarchars) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b"), Varchar("c")});
    auto out = RoundTrip(
        schema, {Value::Varchar("ab"), Value::Null(ColumnType::VARCHAR), Value::Varchar("cd")});

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].AsVarchar(), "ab") << "column a absorbed a later column's bytes";
    EXPECT_TRUE(out[1].IsNull());
    EXPECT_EQ(out[2].AsVarchar(), "cd");
}

TEST(TupleVarLength, NullFirst) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b")});
    auto out = RoundTrip(schema, {Value::Null(ColumnType::VARCHAR), Value::Varchar("ab")});

    ASSERT_EQ(out.size(), 2u);
    EXPECT_TRUE(out[0].IsNull());
    EXPECT_EQ(out[1].AsVarchar(), "ab");
}

TEST(TupleVarLength, NullLast) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b")});
    auto out = RoundTrip(schema, {Value::Varchar("ab"), Value::Null(ColumnType::VARCHAR)});

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].AsVarchar(), "ab");
    EXPECT_TRUE(out[1].IsNull());
}

TEST(TupleVarLength, ConsecutiveNullsThenValue) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b"), Varchar("c"), Varchar("d")});
    auto out = RoundTrip(schema, {Value::Varchar("ab"), Value::Null(ColumnType::VARCHAR),
                                  Value::Null(ColumnType::VARCHAR), Value::Varchar("cd")});

    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0].AsVarchar(), "ab");
    EXPECT_TRUE(out[1].IsNull());
    EXPECT_TRUE(out[2].IsNull());
    EXPECT_EQ(out[3].AsVarchar(), "cd");
}

TEST(TupleVarLength, AllNull) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b")});
    auto out =
        RoundTrip(schema, {Value::Null(ColumnType::VARCHAR), Value::Null(ColumnType::VARCHAR)});

    ASSERT_EQ(out.size(), 2u);
    EXPECT_TRUE(out[0].IsNull());
    EXPECT_TRUE(out[1].IsNull());
}

// NULL and "" must stay distinguishable. The null bitmap is the authority.
TEST(TupleVarLength, EmptyStringIsNotNull) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b"), Varchar("c")});
    auto out = RoundTrip(
        schema, {Value::Varchar(""), Value::Null(ColumnType::VARCHAR), Value::Varchar("x")});

    ASSERT_EQ(out.size(), 3u);
    EXPECT_FALSE(out[0].IsNull()) << "empty string must not read back as NULL";
    EXPECT_EQ(out[0].AsVarchar(), "");
    EXPECT_TRUE(out[1].IsNull());
    EXPECT_EQ(out[2].AsVarchar(), "x");
}

// ─────────────────────────────────────────────────────────────────────────────
// Fixed + variable columns interleaved.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleVarLength, MixedFixedAndVariable) {
    auto schema = MakeSchema({Int32Col("id"), Varchar("a"), Int32Col("n"), Varchar("b")});
    auto out = RoundTrip(
        schema, {Value::Int32(42), Value::Varchar("ab"), Value::Int32(7), Value::Varchar("cd")});

    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0].AsInt32(), 42);
    EXPECT_EQ(out[1].AsVarchar(), "ab");
    EXPECT_EQ(out[2].AsInt32(), 7);
    EXPECT_EQ(out[3].AsVarchar(), "cd");
}

TEST(TupleVarLength, NullFixedColumnDoesNotShiftVarData) {
    auto schema = MakeSchema({Int32Col("id"), Varchar("a"), Varchar("b")});
    auto out = RoundTrip(
        schema, {Value::Null(ColumnType::INT32), Value::Varchar("ab"), Value::Varchar("cd")});

    ASSERT_EQ(out.size(), 3u);
    EXPECT_TRUE(out[0].IsNull());
    EXPECT_EQ(out[1].AsVarchar(), "ab");
    EXPECT_EQ(out[2].AsVarchar(), "cd");
}

// ─────────────────────────────────────────────────────────────────────────────
// VECTOR columns -- now a fixed-width reference, not inline floats (B5).
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleVector, VectorRefRoundTrips) {
    auto schema = MakeSchema({Vector("embedding", 768)});
    const VectorRef ref{42, 1234};
    auto out = RoundTrip(schema, {Value::Vector(ref)});

    ASSERT_EQ(out.size(), 1u);
    ASSERT_FALSE(out[0].IsNull());
    EXPECT_EQ(out[0].AsVector(), ref) << "the reference did not survive the round trip";
}

// The row cost of a VECTOR column no longer depends on its dimension. This is
// the whole point of B2/B5: a 1536-dim embedding is 6144 bytes, larger than an
// entire 4096-byte page, so a row that stored it inline could never be
// inserted at all.
TEST(TupleVector, RowSizeIsIndependentOfDimension) {
    auto small = Tuple::Serialize(MakeSchema({Vector("v", 3)}), {Value::Vector(VectorRef{1, 0})});
    auto huge = Tuple::Serialize(MakeSchema({Vector("v", 1536)}), {Value::Vector(VectorRef{1, 0})});

    EXPECT_EQ(small.size(), huge.size())
        << "a vector column's row footprint still scales with its dimension";
    EXPECT_LT(huge.size(), PAGE_SIZE) << "a 1536-dim row must now fit on a page";
}

TEST(TupleVector, NullVectorStaysNullAndCostsNothing) {
    auto schema = MakeSchema({Vector("v", 128), Int32Col("id")});
    auto out = RoundTrip(schema, {Value::Null(ColumnType::VECTOR), Value::Int32(9)});

    ASSERT_EQ(out.size(), 2u);
    EXPECT_TRUE(out[0].IsNull());
    EXPECT_EQ(out[0].Type(), ColumnType::VECTOR) << "a NULL must remember its column type";
    EXPECT_EQ(out[1].AsInt32(), 9) << "the NULL vector's absent bytes shifted the next column";
}

// A NULL vector occupies no bytes in the fixed region, exactly like a NULL
// int. If Deserialize mis-tracked that, every later fixed column would read
// eight bytes off.
TEST(TupleVector, NullVectorBetweenFixedColumnsDoesNotShiftThem) {
    auto schema = MakeSchema({Int32Col("a"), Vector("v", 4), Int32Col("b")});
    auto out =
        RoundTrip(schema, {Value::Int32(111), Value::Null(ColumnType::VECTOR), Value::Int32(222)});

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].AsInt32(), 111);
    EXPECT_TRUE(out[1].IsNull());
    EXPECT_EQ(out[2].AsInt32(), 222);
}

// A1's shape, restaged: a fixed VECTOR immediately before a NULL VARCHAR and
// then a live VARCHAR. The vector no longer participates in the offset table,
// so this now checks that its move out of that table did not disturb it.
TEST(TupleVector, VectorBeforeNullVarcharFollowedByVarchar) {
    auto schema = MakeSchema({Vector("v", 1), Varchar("gap"), Varchar("tail")});
    const VectorRef ref{3, 77};
    auto out = RoundTrip(
        schema, {Value::Vector(ref), Value::Null(ColumnType::VARCHAR), Value::Varchar("abc")});

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].AsVector(), ref);
    EXPECT_TRUE(out[1].IsNull());
    EXPECT_EQ(out[2].AsVarchar(), "abc");
}

// NULL is how an absent vector is spelled. A reference with no heap id is a
// caller who forgot to store the vector first, and it would resolve to index
// 0 of nothing -- so it must not reach the page.
TEST(TupleVector, UnassignedVectorRefIsRejected) {
    auto schema = MakeSchema({Vector("v", 4)});
    EXPECT_THROW((void)Tuple::Serialize(schema, {Value::Vector(VectorRef{})}),
                 AxolotlDB::utils::StorageException);
}

// ─────────────────────────────────────────────────────────────────────────────
// A2: corrupt / truncated input must throw, never read out of bounds.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleCorruption, TruncatedBufferThrows) {
    auto schema = MakeSchema({Int32Col("id"), Varchar("a"), Varchar("b")});
    auto full =
        Tuple::Serialize(schema, {Value::Int32(42), Value::Varchar("ab"), Value::Varchar("cd")});

    // Every proper prefix of a valid tuple is corrupt input.
    for (std::size_t len = 0; len < full.size(); ++len) {
        std::vector<std::byte> truncated(full.begin(), full.begin() + len);
        EXPECT_THROW(Tuple::Deserialize(schema, std::span<const std::byte>(truncated)),
                     AxolotlDB::utils::StorageException)
            << "no throw for truncation at length " << len;
    }
}

TEST(TupleCorruption, EmptyBufferThrows) {
    auto schema = MakeSchema({Int32Col("id")});
    std::vector<std::byte> empty;
    EXPECT_THROW(Tuple::Deserialize(schema, std::span<const std::byte>(empty)),
                 AxolotlDB::utils::StorageException);
}

// ─────────────────────────────────────────────────────────────────────────────
// Type discipline. Value now holds exactly one payload, so a value whose type
// disagrees with its column is caught instead of silently reading whichever
// member the switch reached for.
//
// B6's dimension check is deliberately absent from this file: Serialize never
// sees floats any more. VectorHeap::Put owns the dimension and is the sole
// enforcement point -- see vector_heap_test.cpp's WrongSizedVectorThrows.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleValidation, ValueOfTheWrongTypeForItsColumnThrows) {
    auto schema = MakeSchema({Varchar("name")});
    EXPECT_THROW((void)Tuple::Serialize(schema, {Value::Int32(42)}),
                 AxolotlDB::utils::TypeMismatchException);
}

TEST(TupleValidation, TypeMismatchMessageNamesTheColumnAndBothTypes) {
    auto schema = MakeSchema({Vector("embedding", 4)});
    try {
        (void)Tuple::Serialize(schema, {Value::Varchar("not a vector")});
        FAIL() << "expected TypeMismatchException";
    } catch (const AxolotlDB::utils::TypeMismatchException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("embedding"), std::string::npos) << msg;
        EXPECT_NE(msg.find("VECTOR"), std::string::npos) << msg;
        EXPECT_NE(msg.find("VARCHAR"), std::string::npos) << msg;
    }
}

TEST(TupleValidation, ReadingAValueAsTheWrongTypeThrows) {
    // The fat struct returned a default-constructed zero here, which is how a
    // wiring bug got to look like legitimate data.
    const Value v = Value::Varchar("hello");
    EXPECT_THROW((void)v.AsInt32(), AxolotlDB::utils::TypeMismatchException);
    EXPECT_THROW((void)v.AsVector(), AxolotlDB::utils::TypeMismatchException);
    EXPECT_EQ(v.AsVarchar(), "hello");
}

TEST(TupleValidation, ReadingANullValueThrowsRatherThanReturningZero) {
    const Value v = Value::Null(ColumnType::INT32);
    ASSERT_TRUE(v.IsNull());
    EXPECT_EQ(v.Type(), ColumnType::INT32);
    EXPECT_THROW((void)v.AsInt32(), AxolotlDB::utils::TypeMismatchException);
}

TEST(TupleValidation, ADefaultConstructedValueIsNull) {
    EXPECT_TRUE(Value().IsNull());
}

// ─────────────────────────────────────────────────────────────────────────────
// A8: variable-length values larger than the uint16_t length table can describe.
//
// Found during the Phase 1 reliability audit. Serialize cast each column's
// byte count to uint16_t at the write site with no range check, so a value
// over 65535 bytes wrapped: a 70000-byte VARCHAR recorded its length as 4464
// (70000 & 0xFFFF) and Deserialize handed back a 4464-byte string. No
// exception, no warning -- the same silent-truncation shape as A1 and B6.
//
// Such a tuple could never fit in a 4096-byte page, so it was unreachable
// through the page path. But Serialize/Deserialize are public static API and
// Phase 3's executor calls them directly, so the corruption happened before
// anything got the chance to reject the size.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleValidation, OversizedVarcharThrowsRatherThanTruncating) {
    auto schema = MakeSchema({Varchar("big")});
    const std::string too_big(MAX_VAR_COLUMN_BYTES + 1, 'x');

    EXPECT_THROW((void)Tuple::Serialize(schema, {Value::Varchar(too_big)}),
                 AxolotlDB::utils::ValueTooLargeException);
}

// A8's vector half is gone by construction: since B5 a VECTOR column is eight
// fixed bytes regardless of dimension, so there is no length-table entry left
// to overflow. A 20000-dim embedding is now a VectorHeap concern, where it
// spans as many pages as it needs.
TEST(TupleValidation, HugeDimensionVectorColumnSerializesFine) {
    auto schema = MakeSchema({Vector("emb", 20000)});
    EXPECT_NO_THROW((void)Tuple::Serialize(schema, {Value::Vector(VectorRef{1, 0})}));
}

TEST(TupleValidation, VarcharExactlyAtTheLimitStillRoundTrips) {
    // The boundary itself must remain legal -- an off-by-one in the guard
    // would reject a value the format can represent perfectly well.
    auto schema = MakeSchema({Varchar("big")});
    const std::string at_limit(MAX_VAR_COLUMN_BYTES, 'y');

    auto bytes = Tuple::Serialize(schema, {Value::Varchar(at_limit)});
    auto back = Tuple::Deserialize(schema, std::span<const std::byte>(bytes));

    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].AsVarchar().size(), MAX_VAR_COLUMN_BYTES);
    EXPECT_EQ(back[0].AsVarchar(), at_limit);
}

TEST(TupleValidation, OversizedValueMessageNamesTheColumn) {
    auto schema = MakeSchema({Varchar("document_body")});
    const std::string too_big(MAX_VAR_COLUMN_BYTES + 100, 'z');

    try {
        (void)Tuple::Serialize(schema, {Value::Varchar(too_big)});
        FAIL() << "expected ValueTooLargeException";
    } catch (const AxolotlDB::utils::ValueTooLargeException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("document_body"), std::string::npos)
            << "message does not name the offending column: " << msg;
    }
}

TEST(TupleValidation, ValueCountMismatchThrows) {
    auto schema = MakeSchema({Int32Col("a"), Int32Col("b")});
    EXPECT_THROW(Tuple::Serialize(schema, {Value::Int32(1)}), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialize, checked in isolation against the exact expected byte layout.
// Bypasses Deserialize entirely -- these hand-decode the buffer so a bug in
// one function can't hide behind a matching bug in the other.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleSerializeRawBytes, TwoVarcharsNoNulls) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b")});
    auto buf = Tuple::Serialize(schema, {Value::Varchar("ab"), Value::Varchar("cd")});

    // ver(1) + bitmap(1) + fixed(0) + length_table(2*2=4) + var_data(2+2=4) = 10
    ASSERT_EQ(buf.size(), 10u);

    auto b = [&](size_t i) { return static_cast<uint8_t>(buf[i]); };

    EXPECT_EQ(b(0), TUPLE_FORMAT_VERSION) << "version byte at index 0";
    EXPECT_EQ(b(1), 0x00) << "null bitmap: no columns are null";

    uint16_t len_a = static_cast<uint16_t>(b(2)) | (static_cast<uint16_t>(b(3)) << 8);
    uint16_t len_b = static_cast<uint16_t>(b(4)) | (static_cast<uint16_t>(b(5)) << 8);
    EXPECT_EQ(len_a, 2) << "length_table[0] should be len(\"ab\") = 2";
    EXPECT_EQ(len_b, 2) << "length_table[1] should be len(\"cd\") = 2";

    EXPECT_EQ(b(6), 'a');
    EXPECT_EQ(b(7), 'b');
    EXPECT_EQ(b(8), 'c');
    EXPECT_EQ(b(9), 'd');
}

TEST(TupleSerializeRawBytes, NullBetweenTwoVarcharsWritesZeroLengthAndSkipsPayload) {
    auto schema = MakeSchema({Varchar("a"), Varchar("b"), Varchar("c")});
    auto buf = Tuple::Serialize(
        schema, {Value::Varchar("ab"), Value::Null(ColumnType::VARCHAR), Value::Varchar("cd")});

    // ver(1) + bitmap(1) + fixed(0) + length_table(3*2=6) + var_data(2+0+2=4) = 12
    ASSERT_EQ(buf.size(), 12u);

    auto b = [&](size_t i) { return static_cast<uint8_t>(buf[i]); };

    EXPECT_EQ(b(0), TUPLE_FORMAT_VERSION) << "version byte at index 0";
    EXPECT_EQ(b(1), 0x02) << "null bitmap: bit 1 (column b) set";

    uint16_t len_a = static_cast<uint16_t>(b(2)) | (static_cast<uint16_t>(b(3)) << 8);
    uint16_t len_b = static_cast<uint16_t>(b(4)) | (static_cast<uint16_t>(b(5)) << 8);
    uint16_t len_c = static_cast<uint16_t>(b(6)) | (static_cast<uint16_t>(b(7)) << 8);
    EXPECT_EQ(len_a, 2);
    EXPECT_EQ(len_b, 0) << "NULL column's length slot must be exactly 0";
    EXPECT_EQ(len_c, 2);

    // var_data must be "ab" immediately followed by "cd" -- NULL contributes
    // no bytes and does not leave a gap.
    EXPECT_EQ(b(8), 'a');
    EXPECT_EQ(b(9), 'b');
    EXPECT_EQ(b(10), 'c');
    EXPECT_EQ(b(11), 'd');
}

TEST(TupleSerializeRawBytes, VectorColumnIsEightFixedBytesWithNoLengthEntry) {
    auto schema = MakeSchema({Vector("v", 3)});
    auto buf = Tuple::Serialize(schema, {Value::Vector(VectorRef{0xAABBCCDDu, 0x11223344u})});

    // ver(1) + bitmap(1) + fixed(8) + length_table(0) + var_data(0) = 10. The absent
    // length table is the load-bearing part: VECTOR left the variable set.
    ASSERT_EQ(buf.size(), 10u);

    auto b = [&](size_t i) { return static_cast<uint8_t>(buf[i]); };
    EXPECT_EQ(b(0), TUPLE_FORMAT_VERSION) << "version byte at index 0";
    EXPECT_EQ(b(1), 0x00) << "null bitmap: the column is not null";

    // heap_id first, then index, both little-endian.
    EXPECT_EQ(b(2), 0xDD);
    EXPECT_EQ(b(3), 0xCC);
    EXPECT_EQ(b(4), 0xBB);
    EXPECT_EQ(b(5), 0xAA);
    EXPECT_EQ(b(6), 0x44);
    EXPECT_EQ(b(7), 0x33);
    EXPECT_EQ(b(8), 0x22);
    EXPECT_EQ(b(9), 0x11);
}

TEST(TupleSerializeRawBytes, NullVectorWritesTheBitmapBitAndNoPayload) {
    auto schema = MakeSchema({Vector("v", 3)});
    auto buf = Tuple::Serialize(schema, {Value::Null(ColumnType::VECTOR)});

    ASSERT_EQ(buf.size(), 2u)
        << "a NULL vector must cost ver byte + the bitmap bit and nothing else";
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), TUPLE_FORMAT_VERSION);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x01);
}

// ─────────────────────────────────────────────────────────────────────────────
// B5's measurable claim. The fat struct was 112 bytes on this toolchain
// because it carried a std::string and a std::vector<float> simultaneously,
// whichever type was actually held. Query execution will build millions of
// these, so the size is worth pinning: a future "just add one more field"
// regression trips this rather than passing quietly.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TupleValueLayout, ValueStaysSmall) {
    EXPECT_LE(sizeof(Value), 48u) << "sizeof(Value) is " << sizeof(Value)
                                  << "; the pre-B5 fat struct was 112";
    // Recorded into the test XML so the number is retrievable without
    // editing the bound. Measured at 48 on MSVC 19.42 Debug.
    RecordProperty("sizeof_Value", static_cast<int>(sizeof(Value)));
}
