#pragma once
// The CLI's only contact with the engine. main.cpp includes this header and
// nothing else from the engine, so every engine-signature assumption lives in
// sql_runner.cpp (look for the "ADAPT" markers).

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SqlError {
    std::string kind;     // "syntax error", "error", ...
    std::string message;
    int line = 0;         // 1-based; 0 means "no position available"
    int column = 0;       // 1-based
};

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;  // already rendered; NULL is "NULL"
    std::optional<SqlError> error;
    double elapsed_ms = 0.0;
};

class SqlRunner {
public:
    /// Opens (or creates) the database files under `data_dir`.
    /// @throws std::exception if the storage layer cannot be opened.
    explicit SqlRunner(const std::filesystem::path& data_dir);
    ~SqlRunner();  // flushes everything; never throws

    SqlRunner(const SqlRunner&) = delete;
    SqlRunner& operator=(const SqlRunner&) = delete;

    /// Runs one statement through Parser -> Binder -> Planner -> rewrites -> executor.
    /// Never throws: failures come back in QueryResult::error.
    [[nodiscard]] QueryResult Execute(const std::string& sql);

    /// Creates a demo `users` table and inserts rows through TableHeap directly
    /// (INSERT is not an executor yet, P4 item 4.3). Idempotent.
    /// @returns a human-readable outcome line.
    [[nodiscard]] std::string SeedDemo();

    /// Lists user table names. Used by :tables.
    [[nodiscard]] std::vector<std::string> TableNames();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};