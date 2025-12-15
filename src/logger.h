// ============================================================================
// logger.h — Structured JSON logger
// ============================================================================
// Emits newline-delimited JSON (NDJSON) to stderr by default.
// Every log line is a JSON object with timestamp (ms epoch), level,
// component, pid, and msg fields — machine-parseable for later analysis.
//
// Usage:
//   LOG_INFO("runtime", "container started, pid=", child_pid);
//   LOG_ERROR("cgroup", "failed to write memory.max: ", strerror(errno));
// ============================================================================
#pragma once

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void setLevel(LogLevel level) { min_level_ = level; }
    void setOutput(std::ostream& out) { out_ = &out; }

    // Variadic log — concatenates all args into the message field.
    template <typename... Args>
    void log(LogLevel level, const char* component, Args&&... args) {
        if (level < min_level_) return;

        // Collect message first (outside the lock to minimize hold time).
        std::ostringstream msg_buf;
        (msg_buf << ... << std::forward<Args>(args));

        auto now   = std::chrono::system_clock::now();
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now.time_since_epoch())
                           .count();

        std::lock_guard<std::mutex> lock(mutex_);
        *out_ << '{'
              << "\"ts\":"        << ts                         << ','
              << "\"level\":\""   << levelStr(level)            << "\","
              << "\"component\":\"" << component                << "\","
              << "\"pid\":"       << static_cast<int>(getpid()) << ','
              << "\"msg\":\""     << escapeJson(msg_buf.str())  << '"'
              << "}\n";
        out_->flush();
    }

private:
    Logger() : min_level_(LogLevel::INFO), out_(&std::cerr) {}

    static const char* levelStr(LogLevel l) {
        switch (l) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
        }
        return "UNKNOWN";
    }

    // Escape special JSON characters so the output stays valid JSON.
    static std::string escapeJson(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }

    LogLevel      min_level_;
    std::ostream* out_;
    std::mutex    mutex_;
};

// Convenience macros — pass component as a string literal + variadic message.
#define LOG_DEBUG(comp, ...) \
    Logger::instance().log(LogLevel::DEBUG, comp, __VA_ARGS__)
#define LOG_INFO(comp, ...) \
    Logger::instance().log(LogLevel::INFO,  comp, __VA_ARGS__)
#define LOG_WARN(comp, ...) \
    Logger::instance().log(LogLevel::WARN,  comp, __VA_ARGS__)
#define LOG_ERROR(comp, ...) \
    Logger::instance().log(LogLevel::ERROR, comp, __VA_ARGS__)
