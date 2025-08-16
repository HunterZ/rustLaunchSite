#include "Logger.h"

#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef RLS_SYSLOG
  #include <syslog.h>
#endif

namespace
{
const std::vector<std::string> LOG_LEVELS{ "INF", "WRN", "ERR" };
constexpr auto SEPARATOR_COLON{":"};
constexpr auto SEPARATOR_PIPE{"|"};
constexpr auto TIME_FORMAT{"{0:%F}T{0:%T%z}"};

const std::string& ToString(const rustLaunchSite::LogLevel level) noexcept
{
  const auto levelNum(static_cast<std::size_t>(level));
  return levelNum >= LOG_LEVELS.size() ?
    LOG_LEVELS.back() : LOG_LEVELS.at(levelNum);
}

// convert log info to a string suitable for stream output
std::string ToString(
  std::string_view message,
  std::string_view file,
  const std::size_t line,
  const rustLaunchSite::LogLevel level)
{
  // use a static buffer so that it can grow internally to a high water mark
  static std::string retVal;
  retVal.clear();
  retVal += ToString(level);
  retVal += SEPARATOR_PIPE;
  retVal += std::format(TIME_FORMAT, std::chrono::system_clock::now());
  retVal += SEPARATOR_PIPE;
  retVal += file;
  retVal += SEPARATOR_COLON;
  retVal += std::to_string(line);
  retVal += SEPARATOR_PIPE;
  retVal += message;
  if ('\n' != retVal.back()) retVal += '\n';
  return retVal;
}

#ifdef RLS_SYSLOG
// convert log info to a string suitable for syslog output
// NOTE: log level and time are handled externally to this
std::string ToString(
  std::string_view message,
  std::string_view file,
  const std::size_t line)
{
  // use a static buffer so that it can grow internally to a high water mark
  static std::string retVal;
  retVal.clear();
  retVal += file;
  retVal += SEPARATOR_COLON;
  retVal += std::to_string(line);
  retVal += SEPARATOR_PIPE;
  retVal += message;
  // newline not required
  // if ('\n' != retVal.back()) retVal += '\n';
  return retVal;
}
#endif
}

namespace rustLaunchSite
{
bool LogSinkStdout::CanWrite() const
{
  return std::cout.good();
}

void LogSinkStdout::Write(
  std::string_view message,
  std::string_view file,
  const std::size_t line,
  const LogLevel level)
{
  const auto& logString(ToString(message, file, line, level));
  std::cout.write(logString.c_str(), logString.length());
}

void LogSinkStdout::Flush()
{
  std::cout.flush();
}


LogSinkFile::LogSinkFile(const std::filesystem::path& outputFile)
  : fileStream_{outputFile.c_str(), std::ios_base::out | std::ios_base::trunc}
{
  if (!fileStream_.good())
  {
    throw std::runtime_error(std::string("Failed to open log file for output: ") + outputFile.string());
  }
}

bool LogSinkFile::CanWrite() const
{
  return fileStream_.good();
}

void LogSinkFile::Write(
  std::string_view message,
  std::string_view file,
  const std::size_t line,
  const LogLevel level)
{
  const auto& logString(ToString(message, file, line, level));
  fileStream_.write(logString.c_str(), logString.length());
}

void LogSinkFile::Flush()
{
  fileStream_.flush();
}


#ifdef RLS_SYSLOG
bool LogSinkSyslog::CanWrite() const
{
  return true;
}

void LogSinkSyslog::Write(
  std::string_view message,
  std::string_view file,
  const std::size_t line,
  const LogLevel level)
{
  const auto& logString(ToString(message, file, line));
  auto prio(LOG_DEBUG);
  switch (level)
  {
    // SrvLib filters anything above LOG_NOTICE, so use that in lieu of LOG_INFO
    case LogLevel::INF: prio = LOG_NOTICE;  break;
    case LogLevel::WRN: prio = LOG_WARNING; break;
    case LogLevel::ERR: prio = LOG_ERR;     break;
  }
  syslog(prio, "%s", logString.c_str());
}
#endif


Logger::Logger(std::shared_ptr<LogSinkI> logSink)
  : logSink_{logSink}
  , hasFlush_{std::dynamic_pointer_cast<LogFlushI>(logSink_)}
{
  // if sink is flushable, start a flusher thread
  if (hasFlush_)
  {
    flushThread_ = std::thread{[this](){FlushLoop();}};
  }
}

Logger::~Logger()
{
  if (hasFlush_)
  {
    // stop and join flush thread
    std::unique_lock lock{mutex_};
    stopFlush_ = true;
    lock.unlock();
    flushThread_.join();
  }
}

void Logger::Log(
  std::string_view message,
  std::string_view file,
  const std::size_t line,
  const LogLevel level)
{
  std::lock_guard lock{mutex_};
  if (!logSink_ || !logSink_->CanWrite()) return;
  logSink_->Write(message, file, line, level);
  doFlush_ = hasFlush_;
}

void Logger::FlushLoop()
{
  // loop until we break
  while (true)
  {
    // sleep for 5 seconds, but check for stop at 1Hz
    for (std::size_t i{0}; i < 5; ++i)
    {
      std::this_thread::sleep_for(std::chrono::seconds{1});
      std::lock_guard lock{mutex_};
      // break out of inner loop on stop flag set
      if (stopFlush_) break;
    }

    // if we need to flush, do it now and clear the flag
    std::lock_guard lock{mutex_};
    if (doFlush_ && logSink_)
    {
      logSink_->Flush();
      doFlush_ = false;
    }

    // break out of outer loop on stop flag set
    if (stopFlush_) break;
  }
}
}
