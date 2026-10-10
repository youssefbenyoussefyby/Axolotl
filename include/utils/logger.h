#pragma once

#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <vector>

namespace AxolotlDB {
namespace utils {

enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError, kCritical, kOff };
class Logger {
   public:
    static void Init(const std::string& logger_name, const std::string& log_file_path,
                     LogLevel level = LogLevel::kInfo, bool async = false,
                     size_t mx_file_sz = 10 * 1024 * 1024,  // 10Mb
                     size_t mx_files = 3);
    static void InitWithSinks(const std::string& loggerName, std::vector<spdlog::sink_ptr> sinks,
                              LogLevel level = LogLevel::kDebug);
    static void Shutdown();
    static spdlog::logger* Get();

    static void SetLevel(LogLevel level);

   private:
    Logger() = delete;
    static std::shared_ptr<spdlog::logger> s_logger_;
    static spdlog::level::level_enum ToSpdlogLevel(LogLevel level);
};

}  // namespace utils
}  // namespace AxolotlDB
#define LOG_TRACE(...) SPDLOG_LOGGER_TRACE(AxolotlDB::utils::Logger::Get(), __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(AxolotlDB::utils::Logger::Get(), __VA_ARGS__)
#define LOG_INFO(...) SPDLOG_LOGGER_INFO(AxolotlDB::utils::Logger::Get(), __VA_ARGS__)
#define LOG_WARN(...) SPDLOG_LOGGER_WARN(AxolotlDB::utils::Logger::Get(), __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(AxolotlDB::utils::Logger::Get(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(AxolotlDB::utils::Logger::Get(), __VA_ARGS__)
