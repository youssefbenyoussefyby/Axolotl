#pragma once

#include <storage/tuple.h>

namespace AxolotlDB::executor {

/// Convert a value produced by expression evaluation into the exact type a
/// column stores, so Tuple::Serialize accepts it. Shared by INSERT and UPDATE.
///
///  - NULL: rejected for a NOT NULL column, otherwise retyped to the column.
///  - Same type: returned unchanged.
///  - Numeric -> numeric: INT32 -> INT64 and any integer -> FLOAT32/FLOAT64
///    widen; INT64 -> INT32 is range-checked; FLOAT64 -> FLOAT32 narrows.
///    FLOAT -> INT is rejected (it would silently drop the fraction), the same
///    rule the binder applies to literals.
///  - Everything else (string <-> number, boolean <-> anything, vectors) is
///    rejected.
///
/// @throws utils::TypeMismatchException for an incompatible type,
///         utils::AxolotException for a NOT NULL violation or an INT32 overflow.
[[nodiscard]] storage::Value CoerceToColumn(const storage::Value& value,
                                            const storage::ColumnDef& column);

}  // namespace AxolotlDB::executor
