#include "Logger.h"

#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
  const std::vector<std::string> LOG_LEVELS
  {
    "INF",
    "WRN",
    "ERR"
  };
  constexpr auto LOG_SEPARATOR("|");
  constexpr auto TIME_FORMAT("{0:%F}T{0:%T%z}");
  constexpr auto TIME_SEPARATOR(":");

  const std::string& ToString(const rustLaunchSite::Logger::Level level)
  {
    return LOG_LEVELS.at(static_cast<std::size_t>(level));
  }

  std::string ToString(
    std::string_view message,
    std::string_view file,
    const std::size_t line,
    const rustLaunchSite::Logger::Level level)
  {
    // use a static buffer so that it can grow internally to a high water mark
    static std::string retVal;
    retVal.clear();
    retVal += ToString(level);
    retVal += LOG_SEPARATOR;
    retVal += std::format(TIME_FORMAT, std::chrono::system_clock::now());
    retVal += LOG_SEPARATOR;
    retVal += file;
    retVal += TIME_SEPARATOR;
    retVal += std::to_string(line);
    retVal += LOG_SEPARATOR;
    retVal += message;
    if ('\n' != retVal.back()) retVal += '\n';
    return retVal;
  }

  std::unique_ptr<std::ofstream> MakeFileUptr(
    const std::filesystem::path& outputFile)
  {
    if (outputFile.empty()) return nullptr;
    std::unique_ptr<std::ofstream> retVal{std::make_unique<std::ofstream>(
        outputFile.c_str(), std::ios_base::out | std::ios_base::trunc)};
    if (!retVal || !retVal->good())
    {
      throw std::runtime_error(std::string("Failed to open log file for output: ") + outputFile.string());
    }
    return std::move(retVal);
  }
}

namespace rustLaunchSite
{
Logger::Logger(const std::filesystem::path& outputFile)
  : fileUptr_{MakeFileUptr(outputFile)}
  , outStream_{fileUptr_ ? *fileUptr_ : std::cout}
  , flushThread_{[this](){FlushLoop();}}
{
}

Logger::~Logger()
{
  // stop and join flush thread
  std::unique_lock lock{mutex_};
  stopFlush_ = true;
  lock.unlock();
  flushThread_.join();
}

void Logger::Log(
  std::string_view message,
  std::string_view file,
  const std::size_t line,
  const Level level)
{
  std::lock_guard lock{mutex_};

  if (fileUptr_ && !fileUptr_->good()) return;

  const auto& logString(ToString(message, file, line, level));

  outStream_.write(logString.c_str(), logString.length());

  if ('\n' != logString.back())
  {
    outStream_.put('\n');
  }

  doFlush_ = true;
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
      // break out of inner loop
      if (stopFlush_) break;
    }

    // if we need to flush, do it now and clear the flag
    std::lock_guard lock{mutex_};
    if (doFlush_)
    {
      outStream_.flush();
      doFlush_ = false;
    }

    // break out of outer loop
    if (stopFlush_) break;
  }
}
}
