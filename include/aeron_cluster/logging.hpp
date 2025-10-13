#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
// Note: source_location is C++20 only, we'll use a simpler approach
#include <iostream>
#include <fstream>
#include <iomanip>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace aeron_cluster {

/**
 * @brief Exception class for Aeron Cluster operations
 */
class AeronClusterException : public std::runtime_error {
public:
    explicit AeronClusterException(const std::string& message) : std::runtime_error(message) {}
    explicit AeronClusterException(const char* message) : std::runtime_error(message) {}
};

/**
 * @brief Log levels in order of severity
 */
enum class LogLevel : int {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    FATAL = 5,
    OFF = 6
};

/**
 * @brief Convert log level to string
 */
constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        case LogLevel::OFF:   return "OFF";
    }
    return "UNKNOWN";
}

/**
 * @brief Log entry with all contextual information
 */
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string message;
    std::string logger_name;
    std::thread::id thread_id;
    std::string_view file_name;
    std::string_view function_name;
    int line_number;
    
    LogEntry(LogLevel lvl, std::string msg, std::string_view logger,
             const char* file = "unknown", const char* func = "unknown", int line = 0)
        : timestamp(std::chrono::system_clock::now())
        , level(lvl)
        , message(std::move(msg))
        , logger_name(logger)
        , thread_id(std::this_thread::get_id())
        , file_name(file)
        , function_name(func)
        , line_number(line) {}
};

/**
 * @brief Interface for log output destinations
 */
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogEntry& entry) = 0;
    virtual void flush() = 0;
};

/**
 * @brief Console output sink with colored output
 */
class ConsoleSink : public LogSink {
public:
    explicit ConsoleSink(bool use_colors = true) : use_colors_(use_colors) {}
    
    void write(const LogEntry& entry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()) % 1000;
        
        std::ostringstream oss;
        oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";
        
        if (use_colors_) {
            oss << getColorCode(entry.level);
        }
        
        oss << "[" << to_string(entry.level) << "] ";
        oss << "[" << entry.logger_name << "] ";
        oss << entry.message;
        
        if (entry.level >= LogLevel::DEBUG) {
            oss << " (" << getFileName(entry.file_name) << ":" << entry.line_number << ")";
        }
        
        if (use_colors_) {
            oss << "\033[0m"; // Reset color
        }
        
        oss << "\n";
        
        if (entry.level >= LogLevel::ERROR) {
            std::cerr << oss.str();
        } else {
            std::cout << oss.str();
        }
    }
    
    void flush() override {
        std::cout.flush();
        std::cerr.flush();
    }

private:
    bool use_colors_;
    std::mutex mutex_;
    
    const char* getColorCode(LogLevel level) const noexcept {
        switch (level) {
            case LogLevel::TRACE: return "\033[37m";  // White
            case LogLevel::DEBUG: return "\033[36m";  // Cyan
            case LogLevel::INFO:  return "\033[32m";  // Green
            case LogLevel::WARN:  return "\033[33m";  // Yellow
            case LogLevel::ERROR: return "\033[31m";  // Red
            case LogLevel::FATAL: return "\033[35m";  // Magenta
            default: return "";
        }
    }
    
    std::string_view getFileName(std::string_view path) const noexcept {
        auto pos = path.find_last_of("/\\");
        return pos != std::string_view::npos ? path.substr(pos + 1) : path;
    }
};

/**
 * @brief File output sink with rotation support
 */
class FileSink : public LogSink {
public:
    explicit FileSink(std::string filename, std::size_t max_size = 10 * 1024 * 1024)
        : filename_(std::move(filename)), max_size_(max_size) {
        openFile();
    }
    
    void write(const LogEntry& entry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!file_.is_open()) {
            openFile();
        }
        
        auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()) % 1000;
        
        file_ << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        file_ << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";
        file_ << "[" << to_string(entry.level) << "] ";
        file_ << "[" << entry.logger_name << "] ";
        file_ << entry.message;
        file_ << " (" << getFileName(entry.file_name) << ":" << entry.line_number << ")";
        file_ << "\n";
        
        current_size_ += entry.message.size() + 100; // Approximate
        
        if (current_size_ > max_size_) {
            rotateFile();
        }
    }
    
    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.flush();
        }
    }

private:
    std::string filename_;
    std::size_t max_size_;
    std::size_t current_size_{0};
    std::ofstream file_;
    std::mutex mutex_;
    
    void openFile() {
        file_.open(filename_, std::ios::app);
        if (file_.is_open()) {
            file_.seekp(0, std::ios::end);
            current_size_ = static_cast<std::size_t>(file_.tellp());
        }
    }
    
    void rotateFile() {
        file_.close();
        
        // Rotate existing files
        std::string backup = filename_ + ".1";
        std::rename(filename_.c_str(), backup.c_str());
        
        current_size_ = 0;
        openFile();
    }
    
    std::string_view getFileName(std::string_view path) const noexcept {
        auto pos = path.find_last_of("/\\");
        return pos != std::string_view::npos ? path.substr(pos + 1) : path;
    }
};

/**
 * @brief Main logger class with multiple sinks support
 */
class Logger {
public:
    explicit Logger(std::string name) : name_(std::move(name)) {}
    
    void setLevel(LogLevel level) noexcept {
        level_.store(level, std::memory_order_relaxed);
    }
    
    [[nodiscard]] LogLevel getLevel() const noexcept {
        return level_.load(std::memory_order_relaxed);
    }
    
    void addSink(std::shared_ptr<LogSink> sink) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        sinks_.push_back(std::move(sink));
    }
    
    void removeSinks() {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        sinks_.clear();
    }
    
    template<typename... Args>
    void log(LogLevel level, std::string_view format, Args&&... args) {
        if (level < level_.load(std::memory_order_relaxed)) {
            return;
        }
        
        std::string message;
        if constexpr (sizeof...(args) == 0) {
            message = format;
        } else {
            message = formatMessage(format, std::forward<Args>(args)...);
        }
        
        LogEntry entry(level, std::move(message), name_, "unknown", "unknown", 0);
        
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        for (auto& sink : sinks_) {
            if (sink) {
                sink->write(entry);
            }
        }
    }
    
    template<typename... Args>
    void trace(std::string_view format, Args&&... args) {
        log(LogLevel::TRACE, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void debug(std::string_view format, Args&&... args) {
        log(LogLevel::DEBUG, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void info(std::string_view format, Args&&... args) {
        log(LogLevel::INFO, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void warn(std::string_view format, Args&&... args) {
        log(LogLevel::WARN, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void error(std::string_view format, Args&&... args) {
        log(LogLevel::ERROR, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void fatal(std::string_view format, Args&&... args) {
        log(LogLevel::FATAL, format, std::forward<Args>(args)...);
    }
    
    void flush() {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        for (auto& sink : sinks_) {
            if (sink) {
                sink->flush();
            }
        }
    }

private:
    std::string name_;
    std::atomic<LogLevel> level_{LogLevel::INFO};
    std::vector<std::shared_ptr<LogSink>> sinks_;
    std::mutex sinks_mutex_;
    
    template<typename... Args>
    std::string formatMessage(std::string_view format, Args&&... args) {
        if constexpr (sizeof...(args) == 0) {
            return std::string(format);
        } else {
            // Simple format implementation - in production, consider using fmt library
            std::ostringstream oss;
            formatImpl(oss, format, std::forward<Args>(args)...);
            return oss.str();
        }
    }
    
    template<typename T, typename... Args>
    void formatImpl(std::ostringstream& oss, std::string_view format, T&& value, Args&&... args) {
        auto pos = format.find("{}");
        if (pos != std::string_view::npos) {
            oss << format.substr(0, pos) << value;
            if constexpr (sizeof...(args) > 0) {
                formatImpl(oss, format.substr(pos + 2), std::forward<Args>(args)...);
            } else {
                oss << format.substr(pos + 2);
            }
        } else {
            oss << format;
        }
    }
};

/**
 * @brief Global logger factory and management
 */
class LoggerFactory {
public:
    static LoggerFactory& instance() {
        static LoggerFactory factory;
        return factory;
    }
    
    std::shared_ptr<Logger> getLogger(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = loggers_.find(name);
        if (it != loggers_.end()) {
            if (auto logger = it->second.lock()) {
                return logger;
            } else {
                loggers_.erase(it);
            }
        }
        
        auto logger = std::make_shared<Logger>(name);
        loggers_[name] = logger;
        
        // Apply default configuration
        logger->setLevel(default_level_);
        for (auto& sink : default_sinks_) {
            logger->addSink(sink);
        }
        
        return logger;
    }
    
    void setDefaultLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        default_level_ = level;
        
        // Apply to existing loggers
        for (auto& [name, weak_logger] : loggers_) {
            if (auto logger = weak_logger.lock()) {
                logger->setLevel(level);
            }
        }
    }
    
    void addDefaultSink(std::shared_ptr<LogSink> sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        default_sinks_.push_back(sink);
        
        // Apply to existing loggers
        for (auto& [name, weak_logger] : loggers_) {
            if (auto logger = weak_logger.lock()) {
                logger->addSink(sink);
            }
        }
    }
    
    void configureConsoleLogging(bool enabled = true, bool colors = true) {
        if (enabled) {
            addDefaultSink(std::make_shared<ConsoleSink>(colors));
        }
    }
    
    void configureFileLogging(const std::string& filename, std::size_t max_size = 10 * 1024 * 1024) {
        addDefaultSink(std::make_shared<FileSink>(filename, max_size));
    }
    
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Flush all loggers
        for (auto& [name, weak_logger] : loggers_) {
            if (auto logger = weak_logger.lock()) {
                logger->flush();
            }
        }
        
        loggers_.clear();
        default_sinks_.clear();
    }

private:
    std::unordered_map<std::string, std::weak_ptr<Logger>> loggers_;
    std::vector<std::shared_ptr<LogSink>> default_sinks_;
    LogLevel default_level_{LogLevel::INFO};
    std::mutex mutex_;
    
    LoggerFactory() {
        // Default console sink
        addDefaultSink(std::make_shared<ConsoleSink>());
    }
};

/**
 * @brief Convenience macros for logging
 */
#define AERON_LOG_TRACE(logger, ...) logger->trace(__VA_ARGS__)
#define AERON_LOG_DEBUG(logger, ...) logger->debug(__VA_ARGS__)
#define AERON_LOG_INFO(logger, ...)  logger->info(__VA_ARGS__)
#define AERON_LOG_WARN(logger, ...)  logger->warn(__VA_ARGS__)
#define AERON_LOG_ERROR(logger, ...) logger->error(__VA_ARGS__)
#define AERON_LOG_FATAL(logger, ...) logger->fatal(__VA_ARGS__)

/**
 * @brief Get logger for current class/component
 */
#define AERON_LOGGER(name) aeron_cluster::LoggerFactory::instance().getLogger(name)

/**
 * @brief Enhanced error result type for operations that can fail
 */
template<typename T, typename E = AeronClusterException>
class Result {
public:
    // Success constructor
    explicit Result(T value) : value_(std::move(value)), has_value_(true) {}
    
    // Error constructor
    explicit Result(E error) : error_(std::move(error)), has_value_(false) {}
    
    // Factory methods
    static Result success(T value) {
        return Result(std::move(value));
    }
    
    static Result error(E error) {
        return Result(std::move(error));
    }
    
    [[nodiscard]] bool isSuccess() const noexcept { return has_value_; }
    [[nodiscard]] bool isError() const noexcept { return !has_value_; }
    
    [[nodiscard]] const T& value() const {
        if (!has_value_) {
            throw std::logic_error("Attempting to access value of error result");
        }
        return value_;
    }
    
    [[nodiscard]] T& value() {
        if (!has_value_) {
            throw std::logic_error("Attempting to access value of error result");
        }
        return value_;
    }
    
    [[nodiscard]] const E& error() const {
        if (has_value_) {
            throw std::logic_error("Attempting to access error of success result");
        }
        return error_;
    }
    
    [[nodiscard]] T valueOr(T defaultValue) const {
        return has_value_ ? value_ : defaultValue;
    }
    
    template<typename F>
    auto map(F&& func) -> Result<std::invoke_result_t<F, T>, E> {
        if (has_value_) {
            return Result<std::invoke_result_t<F, T>, E>::success(func(value_));
        } else {
            return Result<std::invoke_result_t<F, T>, E>::error(error_);
        }
    }
    
    template<typename F>
    Result& onError(F&& func) {
        if (!has_value_) {
            func(error_);
        }
        return *this;
    }

private:
    union {
        T value_;
        E error_;
    };
    bool has_value_;
};

} // namespace aeron_cluster