#include <utils/config.h>
#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace AxolotlDB::utils {

AppConfig ConfigManager::s_config_ = {};
bool ConfigManager::s_initialized_ = false;

template <typename T>
T SafeGet(const YAML::Node& node, const std::string& key, const T& default_val) {
    try {
        if (node[key] && !node[key].IsNull()) {
            return node[key].as<T>();
        }
    } catch (const YAML::Exception& e) {
        throw ConfigException("Failed to parse key '" + key + "': " + e.what());
    }
    return default_val;
}

void ConfigManager::ParseYaml(const std::string& yaml_content, AppConfig& cfg) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_content);
    } catch (const YAML::ParserException& e) {
        throw ConfigException("YAML parse error at line " + std::to_string(e.mark.line + 1) + ": " +
                              e.what());
    }

    if (root["database"]) {
        auto db = root["database"];
        cfg.database.name = SafeGet<std::string>(db, "name", cfg.database.name);
        cfg.database.data_dir = SafeGet<std::string>(db, "data_dir", cfg.database.data_dir);
        cfg.database.max_connections =
            SafeGet<int>(db, "max_connections", cfg.database.max_connections);
    }

    if (root["storage"]) {
        auto st = root["storage"];
        cfg.storage.page_size = SafeGet<uint32_t>(st, "page_size", cfg.storage.page_size);
        cfg.storage.buffer_pool_size =
            SafeGet<uint32_t>(st, "buffer_pool_size", cfg.storage.buffer_pool_size);
        cfg.storage.checkpoint_interval =
            SafeGet<uint32_t>(st, "checkpoint_interval", cfg.storage.checkpoint_interval);
        cfg.storage.sync_writes = SafeGet<bool>(st, "sync_writes", cfg.storage.sync_writes);
    }

    if (root["logging"]) {
        auto lg = root["logging"];
        cfg.logging.level = SafeGet<std::string>(lg, "level", cfg.logging.level);
        cfg.logging.file = SafeGet<std::string>(lg, "file", cfg.logging.file);
        cfg.logging.async = SafeGet<bool>(lg, "async", cfg.logging.async);
        cfg.logging.max_file_size_mb =
            SafeGet<uint32_t>(lg, "max_file_size_mb", cfg.logging.max_file_size_mb);
        cfg.logging.max_files = SafeGet<uint32_t>(lg, "max_files", cfg.logging.max_files);
    }

    if (root["network"]) {
        auto net = root["network"];
        cfg.network.host = SafeGet<std::string>(net, "host", cfg.network.host);
        cfg.network.port = SafeGet<uint16_t>(net, "port", cfg.network.port);
        cfg.network.thread_pool_size =
            SafeGet<uint32_t>(net, "thread_pool_size", cfg.network.thread_pool_size);
        cfg.network.connection_timeout_ms =
            SafeGet<uint32_t>(net, "connection_timeout_ms", cfg.network.connection_timeout_ms);
        cfg.network.max_packet_size_bytes =
            SafeGet<uint32_t>(net, "max_packet_size_bytes", cfg.network.max_packet_size_bytes);
    }

    if (root["vector"]) {
        auto vec = root["vector"];
        cfg.vector.default_dimension =
            SafeGet<uint32_t>(vec, "default_dimension", cfg.vector.default_dimension);
        cfg.vector.distance_metric =
            SafeGet<std::string>(vec, "distance_metric", cfg.vector.distance_metric);

        if (vec["hnsw"]) {
            auto hnsw = vec["hnsw"];
            cfg.vector.hnsw.m = SafeGet<uint32_t>(hnsw, "m", cfg.vector.hnsw.m);
            cfg.vector.hnsw.ef_construction =
                SafeGet<uint32_t>(hnsw, "ef_construction", cfg.vector.hnsw.ef_construction);
            cfg.vector.hnsw.ef_search =
                SafeGet<uint32_t>(hnsw, "ef_search", cfg.vector.hnsw.ef_search);
        }
    }

    if (root["query"]) {
        auto qry = root["query"];
        cfg.query.max_result_rows =
            SafeGet<uint32_t>(qry, "max_result_rows", cfg.query.max_result_rows);
        cfg.query.join_buffer_size_mb =
            SafeGet<uint32_t>(qry, "join_buffer_size_mb", cfg.query.join_buffer_size_mb);
        cfg.query.enable_optimizer =
            SafeGet<bool>(qry, "enable_optimizer", cfg.query.enable_optimizer);
    }
}

void ConfigManager::LoadFromFile(const std::string& yaml_path) {
    if (!std::filesystem::exists(yaml_path)) {
        throw ConfigException("Config file not found: '" + yaml_path + "'");
    }

    std::ifstream file(yaml_path);
    if (!file.is_open()) {
        throw ConfigException("Cannot open config file: '" + yaml_path + "'");
    }

    std::ostringstream ss;
    ss << file.rdbuf();

    AppConfig cfg{};
    ParseYaml(ss.str(), cfg);
    Validate(cfg);

    s_config_ = std::move(cfg);
    s_initialized_ = true;
}

void ConfigManager::LoadFromString(const std::string& yaml_content) {
    AppConfig cfg{};
    ParseYaml(yaml_content, cfg);
    Validate(cfg);

    s_config_ = std::move(cfg);
    s_initialized_ = true;
}

void ConfigManager::LoadDefaults() {
    s_config_ = AppConfig{};
    s_initialized_ = true;
}

void ConfigManager::ApplyEnvOverrides() {
    auto parse_u16 = [](const std::string& key, const std::string& value) -> uint16_t {
        try {
            const int parsed = std::stoi(value);
            if (parsed <= 0 || parsed > 65535) {
                throw ConfigException("Invalid value for env '" + key +
                                      "': expected integer in range [1, 65535], got '" + value +
                                      "'");
            }
            return static_cast<uint16_t>(parsed);
        } catch (const std::invalid_argument&) {
            throw ConfigException("Invalid value for env '" + key + "': expected integer, got '" +
                                  value + "'");
        } catch (const std::out_of_range&) {
            throw ConfigException("Invalid value for env '" + key + "': value out of range '" +
                                  value + "'");
        }
    };

    auto parse_u32 = [](const std::string& key, const std::string& value) -> uint32_t {
        try {
            const unsigned long parsed = std::stoul(value);
            if (parsed > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
                throw ConfigException("Invalid value for env '" + key + "': value out of range '" +
                                      value + "'");
            }
            return static_cast<uint32_t>(parsed);
        } catch (const std::invalid_argument&) {
            throw ConfigException("Invalid value for env '" + key +
                                  "': expected unsigned integer, got '" + value + "'");
        } catch (const std::out_of_range&) {
            throw ConfigException("Invalid value for env '" + key + "': value out of range '" +
                                  value + "'");
        }
    };

    auto parse_bool = [](const std::string& key, const std::string& value) -> bool {
        if (value == "true" || value == "1")
            return true;
        if (value == "false" || value == "0")
            return false;
        throw ConfigException("Invalid value for env '" + key +
                              "': expected one of true/false/1/0, got '" + value + "'");
    };

    if (auto v = GetEnv("AxolotlDB_NETWORK_PORT")) {
        s_config_.network.port = parse_u16("AxolotlDB_NETWORK_PORT", *v);
    }

    if (auto v = GetEnv("AxolotlDB_NETWORK_HOST")) {
        s_config_.network.host = *v;
    }

    if (auto v = GetEnv("AxolotlDB_NETWORK_THREAD_POOL_SIZE")) {
        s_config_.network.thread_pool_size = parse_u32("AxolotlDB_NETWORK_THREAD_POOL_SIZE", *v);
    }

    if (auto v = GetEnv("AxolotlDB_STORAGE_BUFFER_POOL_SIZE")) {
        s_config_.storage.buffer_pool_size = parse_u32("AxolotlDB_STORAGE_BUFFER_POOL_SIZE", *v);
    }

    if (auto v = GetEnv("AxolotlDB_STORAGE_SYNC_WRITES")) {
        s_config_.storage.sync_writes = parse_bool("AxolotlDB_STORAGE_SYNC_WRITES", *v);
    }

    if (auto v = GetEnv("AxolotlDB_LOGGING_LEVEL")) {
        s_config_.logging.level = *v;
    }

    if (auto v = GetEnv("AxolotlDB_DATABASE_DATA_DIR")) {
        s_config_.database.data_dir = *v;
    }

    Validate(s_config_);
}

void ConfigManager::Validate(const AppConfig& cfg) {
    if (cfg.storage.page_size == 0 || (cfg.storage.page_size & (cfg.storage.page_size - 1)) != 0)
        throw ConfigValidationError("storage.page_size", "must be a power of 2 (e.g. 4096, 8192)");
    if (cfg.storage.buffer_pool_size == 0)
        throw ConfigValidationError("storage.buffer_pool_size", "must be > 0");
    if (cfg.network.port == 0)
        throw ConfigValidationError("network.port", "must be between 1 and 65535");

    if (cfg.network.thread_pool_size == 0)
        throw ConfigValidationError("network.thread_pool_size", "must be > 0");
    static const std::set<std::string> valid_levels = {"trace", "debug",    "info", "warn",
                                                       "error", "critical", "off"};
    if (valid_levels.find(cfg.logging.level) == valid_levels.end())
        throw ConfigValidationError(
            "logging.level", "must be one of: trace, debug, info, warn, error, critical, off");
    static const std::set<std::string> valid_metrics = {"cosine", "l2", "dot"};
    if (valid_metrics.find(cfg.vector.distance_metric) == valid_metrics.end())
        throw ConfigValidationError("vector.distance_metric", "must be one of: cosine, l2, dot");
    if (cfg.vector.default_dimension == 0)
        throw ConfigValidationError("vector.default_dimension", "must be > 0");
    if (cfg.vector.hnsw.ef_construction < cfg.vector.hnsw.m)
        throw ConfigValidationError("vector.hnsw.ef_construction",
                                    "must be >= vector.hnsw.m for correct HNSW behavior");
    if (cfg.database.max_connections == 0)
        throw ConfigValidationError("database.max_connections", "must be > 0");
}

const AppConfig& ConfigManager::Get() {
    if (!s_initialized_) {
        throw ConfigException(
            "ConfigManager is not initialized; call LoadDefaults/LoadFromFile/LoadFromString "
            "first");
    }
    return s_config_;
}
const DatabaseConfig& ConfigManager::Database() {
    return Get().database;
}
const StorageConfig& ConfigManager::Storage() {
    return Get().storage;
}
const LoggingConfig& ConfigManager::Logging() {
    return Get().logging;
}
const NetworkConfig& ConfigManager::Network() {
    return Get().network;
}
const VectorConfig& ConfigManager::Vector() {
    return Get().vector;
}
const QueryConfig& ConfigManager::Query() {
    return Get().query;
}

bool ConfigManager::IsInitialized() {
    return s_initialized_;
}
void ConfigManager::Reset() {
    s_config_ = {};
    s_initialized_ = false;
}
void ConfigManager::OverrideNetworkPort(uint16_t port) {
    s_config_.network.port = port;
    Validate(s_config_);
}

void ConfigManager::OverrideLogLevel(const std::string& level) {
    s_config_.logging.level = level;
    Validate(s_config_);
}
std::optional<std::string> ConfigManager::GetEnv(const std::string& var_name) {
    // MSVC deprecates std::getenv (C4996) in favour of _dupenv_s, which
    // returns an owned buffer the caller must free. Same platform-branch
    // pattern as FileManager's _commit/fsync split, rather than silencing
    // the warning project-wide.
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (::_dupenv_s(&buf, &len, var_name.c_str()) != 0 || buf == nullptr) {
        return std::nullopt;
    }
    std::string val(buf);
    std::free(buf);
    if (val.empty())
        return std::nullopt;
    return val;
#else
    const char* val = std::getenv(var_name.c_str());
    if (val == nullptr || val[0] == '\0')
        return std::nullopt;
    return std::string(val);
#endif
}

}  // namespace AxolotlDB::utils
