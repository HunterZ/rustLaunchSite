#ifndef LOGGER_H
#define LOGGER_H

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

// enable POSIX syslog support if available
#if __has_include(<syslog.h>)
  #define RLS_SYSLOG
#endif

namespace rustLaunchSite
{
/// @brief Logging levels
enum class LogLevel { INF = 0, WRN = 1, ERR = 2 };

/// @brief Pure virtual interface class for a logging sink
class LogSinkI
{
public:

  virtual bool CanWrite() const = 0;

  virtual void Write(
    std::string_view message,
    std::string_view file,
    const std::size_t line,
    const LogLevel level = LogLevel::INF
  ) = 0;

  virtual void Flush() {};
};

/// @brief Pure virtual interface class for a flushable log object
class LogFlushI
{
public:

  virtual void Flush() = 0;
};

/// @brief Implementation of LogSink that logs to @c std::out (stdout)
class LogSinkStdout : public LogSinkI, public LogFlushI
{
public:

  bool CanWrite() const override;

  void Write(
    std::string_view message,
    std::string_view file,
    const std::size_t line,
    const LogLevel level = LogLevel::INF
  ) override;

  void Flush() override;
};

/// @brief Implementation of LogSink that logs to a file
class LogSinkFile : public LogSinkI, public LogFlushI
{
public:

  /// @brief Create a log sink that writes to the specified file
  /// @details Output file will be truncated on open.
  /// @param outputFile File/path to which this sink should write
  /// @throws @c std::runtime_error if specified @c outputFile cannot be opened
  ///  for writing
  explicit LogSinkFile(const std::filesystem::path& outputFile);

  bool CanWrite() const override;

  void Write(
    std::string_view message,
    std::string_view file,
    const std::size_t line,
    const LogLevel level = LogLevel::INF
  ) override;

  void Flush() override;

private:

  std::ofstream fileStream_;
};

#ifdef RLS_SYSLOG
/// @brief Implementation of LogSink that logs to a POSIX syslog
class LogSinkSyslog : public LogSinkI
{
public:

  bool CanWrite() const override;

  void Write(
    std::string_view message,
    std::string_view file,
    const std::size_t line,
    const LogLevel level = LogLevel::INF
  ) override;
};
#endif

/// @brief rustLaunchSite logging facility
///
/// @details Implements the ability to log messages to a file, stdout, or (POSIX
///  only) syslog.
///
/// Logging sink must be provided at time of construction. Using the same
///  logging sink with two @c Logger instances is unsupported, and may result in
///  undefined behavior.
///
/// If the chosen sink supports I/O flushing, this will be performed once every
///  5 seconds as a compromise between performance and usability/reliability.
class Logger
{
public:

  /// @brief Constructor
  ///
  /// @param logSink Endpoint to which log messages should be written
  explicit Logger(std::shared_ptr<LogSinkI> logSink);

  /// @brief Destructor
  ~Logger();

  /// @brief Log an error message
  ///
  /// @details @c level values greater than @c LOGERR will be treated as
  ///  @c LOGERR.
  ///
  /// This method is thread safe.
  ///
  /// @param message Log message
  /// @param file Originating source file
  /// @param line Originating source line
  /// @param level Log level
  void Log(
    std::string_view message,
    std::string_view file,
    const std::size_t line,
    const LogLevel level = LogLevel::INF
  );

  /// @brief Trim the given path, leaving only the filename
  /// @details Finds and returns start of filename. Constexpr for macro use.
  /// @param path Path to trim
  /// @return Pointer to start of filename
  static constexpr auto* TrimFile(const char* const path)
  {
    const auto* startPosition(path);
    for (const auto* currentCharacter(path)
          ; *currentCharacter != '\0'
          ; ++currentCharacter)
    {
      if (*currentCharacter == '\\' || *currentCharacter == '/')
      {
        startPosition = currentCharacter;
      }
    }

    if (startPosition != path) ++startPosition;

    return startPosition;
  }

private:

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void FlushLoop();

  std::shared_ptr<LogSinkI> logSink_ = nullptr;
  std::recursive_mutex mutex_;
  bool hasFlush_ = false;
  bool doFlush_ = false;
  bool stopFlush_ = false;
  std::thread flushThread_;
  static std::stringstream* shutUpLintersTheMacroNeedsSstreamHeader_;
};

/// @brief Logging macro for use by @c LOGINF / @c LOGWRN / @c LOGERR
#define LOG(logger, level, message) \
{ \
  std::stringstream __temp_log_stream__; __temp_log_stream__ << message; \
  (logger).Log( \
    __temp_log_stream__.str() \
    , rustLaunchSite::Logger::TrimFile(__FILE__) \
    , __LINE__ \
    , level); \
}

/// @brief Log an INFO-level message to the specified logger
/// @details Must be terminated in a semicolon.
///
/// Example usage:
/// @code
/// LOGINF(myLogger, "The answer is " << x << " or something");
/// @endcode
///
/// @param logger Destination logger
/// @param message Message to log
#define LOGINF(logger, message) do { \
  LOG(logger, rustLaunchSite::LogLevel::INF, message) \
} while (false)

/// @brief Log a WARNING-level message to the specified logger
/// @details Must be terminated in a semicolon.
///
/// Example usage:
/// @code
/// LOGWRN(myLogger, "The answer is " << x << " or something");
/// @endcode
///
/// @param logger Destination logger
/// @param message Message to log
#define LOGWRN(logger, message) do { \
  LOG(logger, rustLaunchSite::LogLevel::WRN, message) \
} while (false)


/// @brief Log an ERROR-level message to the specified logger
/// @details Must be terminated in a semicolon.
///
/// Example usage:
/// @code
/// LOGERR(myLogger, "The answer is " << x << " or something");
/// @endcode
///
/// @param logger Destination logger
/// @param message Message to log
#define LOGERR(logger, message) do { \
  LOG(logger, rustLaunchSite::LogLevel::ERR, message) \
} while (false)
}

#endif
