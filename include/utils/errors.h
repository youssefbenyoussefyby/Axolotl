#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace AxolotlDB::utils {
class AxolotException : public std::runtime_error {
   public:
    explicit AxolotException(const std::string& msg) : std::runtime_error("[AxolotlDB] " + msg) {}
};

class SyntaxException : public AxolotException {
   public:
    uint16_t line;
    uint16_t column;
    std::string token;

    SyntaxException(const std::string& msg, uint16_t line, uint16_t column,
                    const std::string& token = "")
        : AxolotException(msg), line(line), column(column), token(token) {}

    uint16_t GetLine() const {
        return line;
    }
    uint16_t GetColumn() const {
        return column;
    }
    const std::string& GetToken() const {
        return token;
    }
    const std::string& GetLexeme() const {
        return token;
    }
    const std::string& GetLexem() const {
        return token;
    }
};

class SemanticException : public SyntaxException {
   public:
    explicit SemanticException(const std::string& msg, uint16_t line = 0, uint16_t column = 0,
                               const std::string& token = "")
        : SyntaxException(msg, line, column, token) {}
};

class StorageException : public AxolotException {
   public:
    explicit StorageException(const std::string& msg) : AxolotException("[Storage] " + msg) {}
};

class InvalidPageException : public StorageException {
   public:
    explicit InvalidPageException(const std::string& msg)
        : StorageException("[InvalidPage] " + msg) {}
};

class PageFullException : public StorageException {
   public:
    explicit PageFullException() : StorageException("Page is full — no space for new tuple") {}
};

class CorruptPageException : public StorageException {
   public:
    CorruptPageException(int32_t page_id, const std::string& invariant)
        : StorageException("Corrupt page " + std::to_string(page_id) + ": " + invariant) {}
};

class InvalidSlotException : public StorageException {
   public:
    explicit InvalidSlotException(uint16_t slot)
        : StorageException("Invalid slot index: " + std::to_string(slot)) {}
};

class IOException : public StorageException {
   public:
    explicit IOException(const std::string& msg) : StorageException("[IO] " + msg) {}
};

class BufferPoolExhaustedException : public StorageException {
   public:
    explicit BufferPoolExhaustedException()
        : StorageException("Buffer pool exhausted - all frames are pinned") {}
};

class ValueTooLargeException : public StorageException {
   public:
    ValueTooLargeException(const std::string& column, std::size_t bytes, std::size_t limit)
        : StorageException("Value for column '" + column + "' is " + std::to_string(bytes) +
                           " bytes, exceeding the " + std::to_string(limit) +
                           "-byte limit of the tuple length table") {}
};

class TypeMismatchException : public StorageException {
   public:
    TypeMismatchException(const std::string& where, const std::string& expected,
                          const std::string& actual)
        : StorageException("Type mismatch in " + where + ": expected " + expected + ", got " +
                           actual) {}
};

class InvalidVectorIdException : public StorageException {
   public:
    InvalidVectorIdException(uint64_t id, uint64_t count)
        : StorageException("Invalid vector id " + std::to_string(id) + ": heap holds " +
                           std::to_string(count) + (count == 1 ? " vector" : " vectors")) {}
};
class ForeignVectorRefException : public StorageException {
   public:
    ForeignVectorRefException(uint32_t ref_heap_id, uint32_t this_heap_id)
        : StorageException("VectorRef belongs to heap " + std::to_string(ref_heap_id) +
                           ", not heap " + std::to_string(this_heap_id)) {}
};

class VectorDimensionMismatchException : public StorageException {
   public:
    VectorDimensionMismatchException(const std::string& where, std::size_t expected,
                                     std::size_t actual)
        : StorageException("Vector dimension mismatch in " + where + ": expected " +
                           std::to_string(expected) + ", got " + std::to_string(actual)) {}
};

}  // namespace AxolotlDB::utils
