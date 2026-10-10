#include <gtest/gtest.h>
#include <utils/logger.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

TEST(LoggerTest, WritesMessageToFile) {
    const std::string log_file = "logger_test.log";
    const std::string message = "logger_test_unique_message";

    std::filesystem::remove(log_file);

    AxolotlDB::utils::Logger::Init("logger-test", log_file, AxolotlDB::utils::LogLevel::kTrace);

    LOG_INFO("{}", message);
    AxolotlDB::utils::Logger::Shutdown();

    std::ifstream input(log_file);
    ASSERT_TRUE(input.is_open());

    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string content = buffer.str();

    EXPECT_NE(content.find(message), std::string::npos);
}
