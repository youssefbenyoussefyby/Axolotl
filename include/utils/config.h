#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace AxolotlDB::utils {

struct DatabaseConfig {
    std::string name = "AxolotlDB";
    std::string data_dir = "./data";
    int max_connections = 100;
};

struct StorageConfig {
    uint32_t page_size = 4096;
    uint32_t buffer_pool_size = 1000;
    uint32_t checkpoint_interval = 300;
    bool sync_writes = true;
};

struct LoggingConfig {
    std::string level = "info";
    std::string file = "logs/AxolotlDB.log";
    bool async = true;
    uint32_t max_file_size_mb = 10;
    uint32_t max_files = 5;
};

struct NetworkConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 6969;
    uint32_t thread_pool_size = 8;
    uint32_t connection_timeout_ms = 5000;
    uint32_t max_packet_size_bytes = 65536;
};

struct HnswConfig {
    uint32_t m = 16;
    uint32_t ef_construction = 200;
    uint32_t ef_search = 50;
};

struct VectorConfig {
    uint32_t default_dimension = 768;
    std::string distance_metric = "cosine";
    HnswConfig hnsw;
};

struct QueryConfig {
    uint32_t max_result_rows = 10000;
    uint32_t join_buffer_size_mb = 32;
    bool enable_optimizer = true;
};
struct AppConfig {
    DatabaseConfig database;
    StorageConfig storage;
    LoggingConfig logging;
    NetworkConfig network;
    VectorConfig vector;
    QueryConfig query;
};

class ConfigException : public std::runtime_error {
   public:
    explicit ConfigException(const std::string& msg) : std::runtime_error("[Config] " + msg) {}
};

class ConfigValidationError : public ConfigException {
   public:
    explicit ConfigValidationError(const std::string& field, const std::string& reason)
        : ConfigException("Validation failed for '" + field + "': " + reason) {}
};
class ConfigManager {
   public:
    static void LoadFromFile(const std::string& yaml_path);
    static void LoadFromString(const std::string& yaml_content);
    static void LoadDefaults();
    static void ApplyEnvOverrides();
    static const AppConfig& Get();
    static const DatabaseConfig& Database();
    static const StorageConfig& Storage();
    static const LoggingConfig& Logging();
    static const NetworkConfig& Network();
    static const VectorConfig& Vector();
    static const QueryConfig& Query();
    static void OverrideNetworkPort(uint16_t port);
    static void OverrideLogLevel(const std::string& level);

    static bool IsInitialized();
    static void Reset();

   private:
    ConfigManager() = delete;
    static void Validate(const AppConfig& config);
    static void ParseYaml(const std::string& yaml_content, AppConfig& out);
    static std::optional<std::string> GetEnv(const std::string& var_name);
    static AppConfig s_config_;
    static bool s_initialized_;
};

}  // namespace AxolotlDB::utils
