#include "Logger.h"
#include "MainCommon.h"
#include "Service.h"

#include <boost/dll.hpp>
#include <boost/process/v1/environment.hpp>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace
{
/// Environment variable to check for config file path+name
const std::string ENV_CONFIG_PATH{"RLS_CONFIG_PATH"};
/// Environment variable to check for log file path+name
const std::string ENV_LOG_PATH{"RLS_LOG_PATH"};
/// Default config file name when not specified via config file
const std::string DEFAULT_CONFIG_FILE{"rustLaunchSite.jsonc"};
const std::string DEFAULT_LOG_FILE{"rustLaunchSite.log"};

/// @brief Try to locate config file and return full path to it
///
/// @details Returns the full path for the first location in the list below at
///  which the file actually exists, or empty path if not found in any of them:
/// 1. Value of environment variable specified by @c ENV_CONFIG_PATH
/// 2.a.i. (Windows) @c <$LOCALAPPDATA>/rustLaunchSite/<DEFAULT_CONFIG_FILE>
/// 2.b.i. (non-Windows) @c <$XDG_CONFIG_HOME>/<DEFAULT_CONFIG_FILE>
/// 2.b.ii. (non-Windows) @c <$HOME>/.config/<DEFAULT_CONFIG_FILE>
/// 2.b.iii. (non-Windows) @c /etc/<DEFAULT_CONFIG_FILE>
/// 3. @c ./<DEFAULT_CONFIG_FILE> (i.e. in current working directory)
/// 4. @c <DEFAULT_CONFIG_FILE> in the application binary directory
///
/// @return Full path to config file, or empty if none found
std::filesystem::path GetConfigPath()
{
  // check environment variable
  const auto& myEnv(boost::this_process::environment());
  const auto& envIter(myEnv.find(ENV_CONFIG_PATH));
  if (myEnv.end() != envIter)
  {
    const auto& envPath(
      std::filesystem::path{envIter->to_string()} / DEFAULT_CONFIG_FILE);
    if (std::filesystem::exists(envPath)) return envPath;
  }

#if defined(_WIN32) || defined(_WIN64)
  // check LOCALAPPDATA
  const auto& ladIter(myEnv.find("LOCALAPPDATA"));
  if (myEnv.end() != ladIter)
  {
    const auto& ladPath(
      std::filesystem::path{ladIter->to_string()} / "rustLaunchSite" /
      DEFAULT_CONFIG_FILE);
    if (std::filesystem::exists(ladPath)) return ladPath;
  }
#else
  // check XDG_CONFIG_HOME
  const auto& xchIter(myEnv.find("XDG_CONFIG_HOME"));
  if (myEnv.end() != xchIter)
  {
    const auto& xchPath(
      std::filesystem::path{xchIter->to_string()} / DEFAULT_CONFIG_FILE);
    if (std::filesystem::exists(xchPath)) return xchPath;
  }

  // check ~/.config
  const auto& homeIter(myEnv.find("HOME"));
  if (myEnv.end() != homeIter)
  {
    const auto& homePath(
      std::filesystem::path{homeIter->to_string()} / ".config" /
      DEFAULT_CONFIG_FILE);
    if (std::filesystem::exists(homePath)) return homePath;
  }

  // check /etc
  const auto& etcPath(std::filesystem::path{"/etc"} / DEFAULT_CONFIG_FILE);
  if (std::filesystem::exists(etcPath)) return etcPath;
#endif

  // check working directory
  const auto& cwdPath(std::filesystem::current_path() / DEFAULT_CONFIG_FILE);
  if (std::filesystem::exists(cwdPath)) return cwdPath;

  // check binary directory
  const auto& binPath(std::filesystem::path{
    boost::dll::program_location().remove_filename_and_trailing_separators().string()}
    / DEFAULT_CONFIG_FILE);
  if (std::filesystem::exists(binPath)) return binPath;

  // failure
  return {};
}

/// @brief Try to determine a suitable log file path
///
/// @details Returns the full path for the first location in the list below that
///  applies, or empty path if none:
/// 1. @c ENV_LOG_PATH environment variable value (directory portion must exist)
/// 2. (Windows) @c <$LOCALAPPDATA>/rustLaunchSite/<DEFAULT_LOG_FILE>
/// 3. @c ./<DEFAULT_LOG_FILE> (i.e. in current working directory)
///
/// @note This really isn't meant to be called on Linux, as it should be
///  possible to log to @c std::cout and have it be redirected by the OS.
///
/// @return Full path to log file, or empty if none
std::filesystem::path GetLogPath()
{
  // check environment variable
  const auto& myEnv(boost::this_process::environment());
  const auto& envIter(myEnv.find(ENV_LOG_PATH));
  if (myEnv.end() != envIter)
  {
    std::filesystem::path envPath{envIter->to_string()};
    // if the containing directory exists, consider this valid
    auto envDir(envPath); envDir.remove_filename();
    if (std::filesystem::exists(envDir))
    {
      return envPath;
    }
  }

#if defined(_WIN32) || defined(_WIN64)
  // check LOCALAPPDATA
  const auto& ladIter(myEnv.find("LOCALAPPDATA"));
  if (myEnv.end() != ladIter)
  {
    const std::filesystem::path ladBasePath{ladIter->to_string()};
    auto ladAppPath(ladBasePath / "rustLaunchSite");
    if (std::filesystem::exists(ladBasePath) &&
        !std::filesystem::exists(ladAppPath))
    {
      // create rustLaunchSite subdir
      std::filesystem::create_directory(ladAppPath);
    }
    ladAppPath /= DEFAULT_LOG_FILE;
    return ladAppPath;
  }
#endif

  // fallback to working directory
  return std::filesystem::current_path() / DEFAULT_LOG_FILE;
}
}

/// Main entry point for service flavor
int main(int argc, char* argv[])
{
  // allocate logger on the stack so that it auto-destructs at end of main()
  rustLaunchSite::Logger logger
  {
#if defined(_WIN32) || defined(_WIN64)
    // only Windows logs to file for now
    GetLogPath()
#endif
  };

  // derive a config file path and create a fake config vector to pass it in
  const auto& configPath(GetConfigPath().string());
  std::vector<char*> fakeArgv{argv[0], const_cast<char*>(configPath.c_str())};

  // we need to launch the main loop in a secondary thread, because SrvLib calls
  //  the start lambda from the main thread, and then tries to use the latter
  //  to listen for a stop signal (oof)
  std::thread rlsThread;

  return ServiceMain(
    argc
  , argv
  , {
#if defined(_WIN32) || defined(_WIN64)
      // Servicename in Service control manager of windows
      L"Rust Launch Site",
      // Description in Service control manager of windows
      L"Rust dedicated server manager",
#endif
      // Service name (service id)
      L"rustLaunchSite",
      [&]()
      {
        LOG_INFO(logger, "Starting RLS thread");
        rlsThread = std::thread{[&]()
        {
          LOG_INFO(logger, "Starting RLS core");
          rustLaunchSite::Start(
            logger
          , static_cast<int>(fakeArgv.size())
          , fakeArgv.data());
          LOG_INFO(logger, "RLS core returned");
        }};
        LOG_INFO(logger, "RLS thread started");
      },
      [&]()
      {
        LOG_INFO(logger, "Stopping RLS core");
        rustLaunchSite::Stop();
        LOG_INFO(logger, "Joining RLS thread");
        rlsThread.join();
        LOG_INFO(logger, "RLS thread joined");
      },
      []()
      {
        // what ever you do with this callback, maybe reload the configuration
      }
    }
  );
}
