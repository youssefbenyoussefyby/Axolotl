#include <gtest/gtest.h>
#include <utils/config.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

void SetEnvVar(const std::string& key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
}

void UnsetEnvVar(const std::string& key) {
#ifdef _WIN32
    _putenv_s(key.c_str(), "");
#else
    unsetenv(key.c_str());
#endif
}

void ClearConfigEnv() {
    UnsetEnvVar("AxolotlDB_NETWORK_PORT");
    UnsetEnvVar("AxolotlDB_NETWORK_HOST");
    UnsetEnvVar("AxolotlDB_NETWORK_THREAD_POOL_SIZE");
    UnsetEnvVar("AxolotlDB_STORAGE_BUFFER_POOL_SIZE");
    UnsetEnvVar("AxolotlDB_STORAGE_SYNC_WRITES");
    UnsetEnvVar("AxolotlDB_LOGGING_LEVEL");
    UnsetEnvVar("AxolotlDB_DATABASE_DATA_DIR");
}

class ConfigTest : public ::testing::Test {
   protected:
    void SetUp() override {
        AxolotlDB::utils::ConfigManager::Reset();
        ClearConfigEnv();
    }

    void TearDown() override {
        ClearConfigEnv();
        AxolotlDB::utils::ConfigManager::Reset();
    }
};

}  // namespace

TEST_F(ConfigTest, GetBeforeInitThrows) {
    EXPECT_THROW(AxolotlDB::utils::ConfigManager::Get(), AxolotlDB::utils::ConfigException);
}

TEST_F(ConfigTest, LoadDefaultsInitializesConfig) {
    AxolotlDB::utils::ConfigManager::LoadDefaults();
    ASSERT_TRUE(AxolotlDB::utils::ConfigManager::IsInitialized());
    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Network().port, static_cast<uint16_t>(6969));
    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Logging().level, "info");
}

TEST_F(ConfigTest, LoadFromStringReadsYamlValues) {
    const std::string yaml = R"(
network:
  host: "0.0.0.0"
  port: 7777
logging:
  level: "debug"
vector:
  distance_metric: "l2"
)";

    AxolotlDB::utils::ConfigManager::LoadFromString(yaml);

    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Network().host, "0.0.0.0");
    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Network().port, static_cast<uint16_t>(7777));
    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Logging().level, "debug");
    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Vector().distance_metric, "l2");
}

TEST_F(ConfigTest, LoadFromStringRejectsInvalidConfig) {
    const std::string yaml = R"(
storage:
  page_size: 3000
)";

    EXPECT_THROW(AxolotlDB::utils::ConfigManager::LoadFromString(yaml),
                 AxolotlDB::utils::ConfigValidationError);
}

TEST_F(ConfigTest, ApplyEnvOverridesUpdatesValues) {
    AxolotlDB::utils::ConfigManager::LoadDefaults();
    SetEnvVar("AxolotlDB_NETWORK_PORT", "9090");
    SetEnvVar("AxolotlDB_STORAGE_SYNC_WRITES", "false");
    SetEnvVar("AxolotlDB_LOGGING_LEVEL", "warn");

    AxolotlDB::utils::ConfigManager::ApplyEnvOverrides();

    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Network().port, static_cast<uint16_t>(9090));
    EXPECT_FALSE(AxolotlDB::utils::ConfigManager::Storage().sync_writes);
    EXPECT_EQ(AxolotlDB::utils::ConfigManager::Logging().level, "warn");
}

TEST_F(ConfigTest, ApplyEnvOverridesRejectsInvalidBool) {
    AxolotlDB::utils::ConfigManager::LoadDefaults();
    SetEnvVar("AxolotlDB_STORAGE_SYNC_WRITES", "maybe");

    EXPECT_THROW(AxolotlDB::utils::ConfigManager::ApplyEnvOverrides(),
                 AxolotlDB::utils::ConfigException);
}
