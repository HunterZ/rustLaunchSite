#ifndef LOGGER_H
#define LOGGER_H

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <thread>

namespace rustLaunchSite
{
/// @brief rustLaunchSite logging facility
///
/// @details Implements the ability to log messages to a file or stdout. Logging
///  output must be specified at time of construction.
class Logger
{
public:

  /// @brief Logging levels
  enum class Level { INF = 0, WRN = 1, ERR = 2 };

  /// @brief Constructor
  ///
  /// @details Logs to @c std::cout if no path specified. If specified path
  ///  already exists, it will be overwritten.
  ///
  /// @param outputFile Optional path to which logs should be written
  ///
  /// @throws @c std::runtime_error if specified @c outputFile cannot be opened
  ///  for writing
  explicit Logger(const std::filesystem::path& outputFile = {});

  /// @brief Destructor
  ~Logger();

  /// @brief Log an error message
  ///
  /// @details @c level values greater than @c LOG_ERROR will be treated as
  ///  @c LOG_ERROR.
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
    const Level level = Level::INF
  );

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

      if (startPosition != path)
      {
        ++startPosition;
      }

      return startPosition;
  }

private:

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void FlushLoop();

  std::unique_ptr<std::ofstream> fileUptr_ = nullptr;
  std::recursive_mutex mutex_ = {};
  std::ostream& outStream_;
  bool doFlush_ = false;
  bool stopFlush_ = false;
  std::thread flushThread_;
  static std::stringstream* s_; // make clangd stop complaining about <sstream>
};

/// @brief Logging macro for use by @c LOG_INFO / @c LOG_WARNING / @c LOG_ERROR
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
/// LOG_INFO(myLogger, "The answer is " << x << " or something");
/// @endcode
///
/// @param logger Destination logger
/// @param message Message to log
#define LOG_INFO(logger, message) do { \
  LOG(logger, rustLaunchSite::Logger::Level::INF, message) \
} while (false)

/// @brief Log a WARNING-level message to the specified logger
/// @details Must be terminated in a semicolon.
///
/// Example usage:
/// @code
/// LOG_WARNING(myLogger, "The answer is " << x << " or something");
/// @endcode
///
/// @param logger Destination logger
/// @param message Message to log
#define LOG_WARNING(logger, message) do { \
  LOG(logger, rustLaunchSite::Logger::Level::WRN, message) \
} while (false)


/// @brief Log an ERROR-level message to the specified logger
/// @details Must be terminated in a semicolon.
///
/// Example usage:
/// @code
/// LOG_ERROR(myLogger, "The answer is " << x << " or something");
/// @endcode
///
/// @param logger Destination logger
/// @param message Message to log
#define LOG_ERROR(logger, message) do { \
  LOG(logger, rustLaunchSite::Logger::Level::ERR, message) \
} while (false)
}

#endif
