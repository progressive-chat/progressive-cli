#pragma once

#include <string>
#include <mutex>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace matrixcli { namespace util {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void setLevel(LogLevel level) { _level = level; }
    LogLevel level() const { return _level; }

    void log(LogLevel lvl, const std::string& message) {
        if (lvl < _level) return;
        std::lock_guard<std::mutex> lock(_mutex);
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_r(&t, &tm);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
            << " [" << levelString(lvl) << "] " << message;
        std::cerr << oss.str() << std::endl;
    }

    void debug(const std::string& msg) { log(LogLevel::Debug, msg); }
    void info(const std::string& msg)  { log(LogLevel::Info, msg); }
    void warn(const std::string& msg)  { log(LogLevel::Warn, msg); }
    void error(const std::string& msg) { log(LogLevel::Error, msg); }

private:
    Logger() = default;
    LogLevel _level = LogLevel::Info;
    std::mutex _mutex;

    static const char* levelString(LogLevel lvl) {
        switch (lvl) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Error: return "ERROR";
        }
        return "???";
    }
};

}} // namespace matrixcli::util
