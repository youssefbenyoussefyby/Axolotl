#include <utils/errors.h>

namespace AxolotlDB::utils {

// All exception types are defined inline in errors.h.
// This translation unit anchors the vtables for the storage-layer exception
// hierarchy so the linker produces exactly one definition.

}  // namespace AxolotlDB::utils