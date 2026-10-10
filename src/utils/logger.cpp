#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <utils/logger.h>

#include <cassert>

namespace AxolotlDB::utils {

std::shared_ptr<spdlog::logger> Logger::s_logger_ = nullptr;

void Logger::Init(const std::string& logger_name, const std::string& log_file_path, LogLevel level,
                  bool async, size_t mx_file_sz, size_t mx_files) {
    if (s_logger_) {
        Shutdown();
    }
    spdlog::drop(logger_name);

    std::vector<spdlog::sink_ptr> sinks;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(ToSpdlogLevel(level));
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");
    sinks.push_back(console_sink);

    auto file_sink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file_path, mx_file_sz, mx_files);
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%H:%M:%S.%e] [%l] [%t] %v");
    sinks.push_back(file_sink);

    if (async) {
        spdlog::init_thread_pool(8192, 1);
        s_logger_ = std::make_shared<spdlog::async_logger>(logger_name, sinks.begin(), sinks.end(),
                                                           spdlog::thread_pool(),
                                                           spdlog::async_overflow_policy::block);
    } else {
        s_logger_ = std::make_shared<spdlog::logger>(logger_name, sinks.begin(), sinks.end());
    }

    s_logger_->set_level(ToSpdlogLevel(level));
    s_logger_->flush_on(spdlog::level::warn);
    spdlog::register_logger(s_logger_);

    LOG_INFO("Logger initialized. name={} file={} level={} async={}", logger_name, log_file_path,
             spdlog::level::to_string_view(ToSpdlogLevel(level)), async);
}

void Logger::InitWithSinks(const std::string& loggerName, std::vector<spdlog::sink_ptr> sinks,
                           LogLevel level) {
    if (s_logger_) {
        Shutdown();
    }
    spdlog::drop(loggerName);

    s_logger_ = std::make_shared<spdlog::logger>(loggerName, sinks.begin(), sinks.end());
    s_logger_->set_level(ToSpdlogLevel(level));
    spdlog::register_logger(s_logger_);
}

void Logger::Shutdown() {
    if (s_logger_) {
        s_logger_->flush();
    }
    spdlog::shutdown();
    s_logger_ = nullptr;
}

spdlog::logger* Logger::Get() {
    assert(s_logger_ != nullptr && "Logger::Init() was never called!");
    return s_logger_.get();
}

void Logger::SetLevel(LogLevel level) {
    if (s_logger_) {
        s_logger_->set_level(ToSpdlogLevel(level));
    }
}

spdlog::level::level_enum Logger::ToSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::kTrace:
            return spdlog::level::trace;
        case LogLevel::kDebug:
            return spdlog::level::debug;
        case LogLevel::kInfo:
            return spdlog::level::info;
        case LogLevel::kWarn:
            return spdlog::level::warn;
        case LogLevel::kError:
            return spdlog::level::err;
        case LogLevel::kCritical:
            return spdlog::level::critical;
        case LogLevel::kOff:
            return spdlog::level::off;
        default:
            return spdlog::level::info;
    }
}

}  // namespace AxolotlDB::utils
