#pragma once

#include <storage/page.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace AxolotlDB::storage {

struct IFileIO {
    virtual ~IFileIO() = default;

    [[nodiscard]] virtual PageId AllocatePage() = 0;

    /// @throws IOException on I/O error.
    /// @throws InvalidPageException if page_id is out of range.
    virtual void ReadPage(PageId page_id, Page& out) = 0;

    /// @throws IOException on I/O error.
    virtual void WritePage(PageId page_id, const Page& page) = 0;

    [[nodiscard]] virtual PageId PageCount() const = 0;

    virtual void Sync() = 0;
};
using FileIOFactory = std::function<std::unique_ptr<IFileIO>(const std::string& path)>;

}  // namespace AxolotlDB::storage
