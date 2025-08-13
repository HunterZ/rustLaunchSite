#include "Config.h"
#include "Downloader.h"
#include "Logger.h"
#include "MainCommon.h"
#include "Server.h"
#include "Updater.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
// timer thread state, controlled by main()
enum class TimerState
{
  RUN,    // timer thread should start/continue running normally
  PAUSE,  // timer thread should sleep until ordered to run or stop
  STOP    // timer thread should exit
};
// mutex and mutex-controlled thread data
namespace threadData
{
// mutex that controls access to sibling variables
std::mutex mutex_;
// CV on which main() waits for notifications
std::condition_variable cvMain_;
// CV on which timer thread waits for notifications
std::condition_variable cvTimer_;
// timer thread state commanded by main()
TimerState timerState_{TimerState::RUN};
// whether Stop() handler is notifying main() to shut down
bool notifyMainStop_{false};
// whether timer thread is notifying main() to check server health
bool notifyMainServer_{false};
// whether timer thread is notifying main() to check for updates
bool notifyMainUpdater_{false};
// whether main() is notifying timer thread to change state
bool notifyTimerThread_{false};
};

// (re)set start time and wake/notification times based on duration inputs
inline void ResetTimers(
  const std::size_t duration1Minutes,
  const std::size_t duration2Minutes,
  std::chrono::steady_clock::time_point& timeStart,
  std::chrono::steady_clock::time_point& time1,
  std::chrono::steady_clock::time_point& time2
)
{
  timeStart = std::chrono::steady_clock::now();
  time1 = timeStart + std::chrono::minutes(duration1Minutes);
  time2 = timeStart + std::chrono::minutes(duration2Minutes);
}

void TimerFunction(
  const std::size_t sleepDurationMinutes,
  const std::size_t updateIntervalMinutes
)
{
  const std::chrono::minutes sleepDuration(sleepDurationMinutes);
  const std::chrono::minutes updateInterval(updateIntervalMinutes);
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point wakeTime;
  std::chrono::steady_clock::time_point updateTime;
  ResetTimers(
    sleepDurationMinutes, updateIntervalMinutes,
    startTime, wakeTime, updateTime
  );
  while (true)
  {
    // grab mutex for safe state variable access in loop when awake
    std::unique_lock lock(threadData::mutex_);
    // release mutex and sleep, unless or until one of the following:
    // - wake time reached
    // - notification received from main()
    // if notified, handle new timer state
    if
    (
      const auto notified(threadData::cvTimer_.wait_until(
        lock, wakeTime,
        [](){return threadData::notifyTimerThread_;}
      ));
      notified
    )
    {
      // mark notification as processed
      threadData::notifyTimerThread_ = false;
      if (threadData::timerState_ == TimerState::RUN)
      {
        // assume PAUSE->RUN
        // reset notification target times and loop back around
        ResetTimers(
          sleepDurationMinutes, updateIntervalMinutes,
          startTime, wakeTime, updateTime
        );
        continue;
      }
      // don't care about PAUSE here
      if (threadData::timerState_ == TimerState::STOP)
      {
        // assume RUN->STOP or PAUSE->STOP
        // break out of loop
        break;
      }
    }
    // wait time elapsed, or got PAUSE notification
    const bool notifyMain(threadData::timerState_ != TimerState::PAUSE);
    const bool updateTimeElapsed(
      updateIntervalMinutes && std::chrono::steady_clock::now() >= updateTime
    );
    // update target times
    wakeTime += sleepDuration;
    if (updateTimeElapsed) { updateTime += updateInterval; }
    // skip notifying main() if "paused"
    if (!notifyMain) { continue; }
    // set notification flags
    threadData::notifyMainServer_ = true;
    threadData::notifyMainUpdater_ = updateTimeElapsed;
    // notify main()
    // it will wake up and grab mutex when we loop around and go back to sleep
    threadData::cvMain_.notify_all();
    // end of loop body
  }
}

// change timer state and notify timer thread of change
// meant to be called by main() under mutex lock
void SetTimerState(const TimerState ts)
{
  threadData::timerState_ = ts;
  threadData::notifyTimerThread_ = true;
  threadData::cvTimer_.notify_all();
}

// check for updates according to provided options
// return pair indicating whether server and/or mod framework needs updating,
//  respectively
std::pair<bool, bool> UpdateCheck(
  rustLaunchSite::Logger& logger
, const rustLaunchSite::Updater& updater
, const bool checkServer
, const bool checkModFramework
, const bool updateModFrameworkOnServer)
{
  std::pair<bool, bool> retVal{false, false};
  if (checkServer)
  {
    LOG_INFO(logger, "Performing server update check");
    retVal.first = updater.CheckServer();
  }
  if (const bool forceCheck{updateModFrameworkOnServer && retVal.first};
      checkModFramework || forceCheck)
  {
    LOG_INFO(logger, "Performing mod framework update check");
    retVal.second = updater.CheckFramework();
  }
  return retVal;
}

// wrapper around Updater::UpdateFramework() to loop until update succeeds
void UpdateFramework(
  rustLaunchSite::Logger& logger,
  const rustLaunchSite::Updater& updater,
  const int retryDelaySeconds = 0, const bool suppressWarning = false)
{
  LOG_INFO(logger, "Entering plugin framework update loop");
  bool firstTry{true};
  for(bool update{true}; update; update = updater.CheckFramework())
  {
    if (!firstTry)
    {
      LOG_WARNING(logger, "Detected plugin framework version mismatch after update attempt...");
      if (retryDelaySeconds > 0)
      {
        LOG_WARNING(logger, "\t...waiting for " << retryDelaySeconds << " second(s) before trying again");
        std::this_thread::sleep_for(std::chrono::seconds(retryDelaySeconds));
      }
      else
      {
        LOG_WARNING(logger, "\t...trying again immediately");
      }
    }

    updater.UpdateFramework(suppressWarning);

    firstTry = false;
  }
  LOG_INFO(logger, "Completed plugin framework update loop");
}

// wrapper around Updater::UpdateServer() to loop until update succeeds
void UpdateServer(
  rustLaunchSite::Logger& logger,
  const rustLaunchSite::Updater& updater,
  const int retryDelaySeconds = 0)
{
  LOG_INFO(logger, "Entering server update loop");
  bool firstTry{true};
  for(bool update{true}; update; update = updater.CheckServer())
  {
    if (!firstTry)
    {
      LOG_WARNING(logger, "Detected server version mismatch after update attempt...");
      if (retryDelaySeconds > 0)
      {
        LOG_WARNING(logger, "\t...waiting for " << retryDelaySeconds << " second(s) before trying again");
        std::this_thread::sleep_for(std::chrono::seconds(retryDelaySeconds));
      }
      else
      {
        LOG_WARNING(logger, "\t...trying again immediately");
      }
    }

    updater.UpdateServer();

    firstTry = false;
  }
  LOG_INFO(logger, "Completed server update loop");
}

// get shutdown reason from text file if one exists
std::string GetShutdownReason(
  rustLaunchSite::Logger& logger
, const std::filesystem::path& reasonPath
, std::string_view fallbackReason = {})
{
  if (!std::filesystem::exists(reasonPath))
  {
    LOG_INFO(logger, "No reason file at reasonPath=" << reasonPath);
    return std::string{fallbackReason};
  }

  std::string retVal{};
  std::ifstream reasonStream{reasonPath};
  if (reasonStream)
  {
    // read the file
    // this is done line-by-line to normalize EOL characters
    std::string line{};
    std::size_t lineCount{0};
    std::size_t nonEmptyLineCount{0};
    while (std::getline(reasonStream, line))
    {
      ++lineCount;
      if (!line.empty()) ++nonEmptyLineCount;
      if (!retVal.empty()) retVal.append("\n");
      retVal.append(line);
    }
    LOG_INFO(logger, "Read " << lineCount << " line(s) from reasonPath=" << reasonPath);
    // if more than one nonempty line read, ensure reason starts with a newline
    if (nonEmptyLineCount > 1 && !retVal.empty() && retVal.at(0) != '\n')
    {
      retVal.insert(0, "\n");
    }
  }
  else
  {
    LOG_WARNING(logger, "Failed to open reason file at reasonPath=" << reasonPath);
    // don't return, because we still want to try to remove the file
    retVal = fallbackReason;
  }

  reasonStream.close();
  if (std::filesystem::remove(reasonPath))
  {
    LOG_INFO(logger, "Deleted reason file at reasonPath=" << reasonPath);
  }
  else
  {
    LOG_WARNING(logger, "Failed to delete reason file at reasonPath=" << reasonPath);
  }

  return retVal;
}
}

namespace rustLaunchSite
{
int Start(Logger& logger, int argc, char* argv[])
{
  LOG_INFO(logger, "Starting");

  if (argc <= 1)
  {
    LOG_ERROR(logger, "Configuration file/path must be specified as an argument");
    return RLS_EXIT_ARG;
  }

  // create null pointers for all facilities we'll be instantiating, so that we
  //  can clean them up if an exception is caught
  std::shared_ptr<rustLaunchSite::Config> configSptr;
  std::unique_ptr<rustLaunchSite::Server> serverUptr;
  std::unique_ptr<rustLaunchSite::Updater> updaterUptr;
  std::unique_ptr<std::thread> timerThreadUptr;

  int retVal(RLS_EXIT_SUCCESS);
  try
  {
    // load config file
    configSptr = std::make_shared<rustLaunchSite::Config>(logger, argv[1]);
    // instantiate server manager
    serverUptr = std::make_unique<rustLaunchSite::Server>(logger, configSptr);
    // instantiate update manager
    updaterUptr = std::make_unique<rustLaunchSite::Updater>(
      logger
    , configSptr
    , std::make_shared<rustLaunchSite::Downloader>(logger)
    );

    {
      const auto [updateServerOnStartup, updateModFrameworkOnStartup] =
        UpdateCheck(
          logger
        , *updaterUptr
        , configSptr->GetUpdateServerOnStartup()
        , configSptr->GetUpdateModFrameworkOnStartup()
        , configSptr->GetUpdateModFrameworkOnServerUpdate())
      ;
      if (updateServerOnStartup)
      {
        UpdateServer(
          logger
        , *updaterUptr
        , configSptr->GetUpdateServerRetryDelaySeconds());
      }
      if (updateModFrameworkOnStartup)
      {
        UpdateFramework(
          logger
        , *updaterUptr
        , configSptr->GetUpdateModFrameworkRetryDelaySeconds()
        , updateServerOnStartup);
      }
    }

    // launch server
    LOG_INFO(logger, "Starting server");
    if (!serverUptr->Start())
    {
      LOG_ERROR(logger, "Server failed to start");
      // okay to just abort at this point
      return RLS_EXIT_START;
    }

    // start timer thread
    LOG_INFO(logger, "Starting timer thread");
    timerThreadUptr = std::make_unique<std::thread>(
      &TimerFunction, 1, configSptr->GetUpdateIntervalMinutes()
    );
    if (!timerThreadUptr)
    {
      LOG_ERROR(logger, "Failed to instantiate timer thread");
      return RLS_EXIT_THREAD;
    }

    // main loop
    LOG_INFO(logger, "Starting main event loop");
    while (true)
    {
      // grab mutex for safe state variable access in loop when awake
      std::unique_lock lock(threadData::mutex_);
      // sleep until we get a notification from the timer thread
      // LOG_INFO(logger, "Waiting for events");
      threadData::cvMain_.wait
      (
        lock,
        [](){
          return (
            threadData::notifyMainStop_ ||
            threadData::notifyMainServer_ ||
            threadData::notifyMainUpdater_
          );
        }
      );
      // handle Stop() notification
      if (threadData::notifyMainStop_)
      {
        // attempt an orderly shutdown
        LOG_INFO(logger, "Server manager stop requested; stopping server");
        ::SetTimerState(TimerState::STOP);
        serverUptr->Stop(GetShutdownReason(
          logger
        , configSptr->GetProcessReasonPath()
        , "Server manager stopped"));
        // as Stop() is the only orderly shutdown stimulus, we want to report a
        //  successful exit
        retVal = RLS_EXIT_SUCCESS;
        break;
      }
      // handle update check timer notification
      if (threadData::notifyMainUpdater_)
      {
        threadData::notifyMainUpdater_ = false;
        // check for updates
        const auto [updateServerOnInterval, updateModFrameworkOnInterval] =
          UpdateCheck(
            logger
          , *updaterUptr
          , configSptr->GetUpdateServerOnInterval()
          , configSptr->GetUpdateModFrameworkOnInterval()
          , configSptr->GetUpdateModFrameworkOnServerUpdate())
        ;
        // if any are needed: take server down, install updates, relaunch server
        if (updateServerOnInterval || updateModFrameworkOnInterval)
        {
          // pause timer thread
          // ...although this probably doesn't matter, since we hold the mutex
          ::SetTimerState(TimerState::PAUSE);
          // derive a reason string
          std::string updateReason{};
          if (updateServerOnInterval)
          {
            updateReason.append("Facepunch");
          }
          if (updateModFrameworkOnInterval)
          {
            if (!updateReason.empty())
            {
              updateReason.append(" + ");
            }
            updateReason.append(rustLaunchSite::Config::ToString(
              configSptr->GetUpdateModFrameworkType()));
          }
          // stop server
          LOG_INFO(logger, "Update(s) required: " << updateReason << "; stopping server");
          // install updates
          serverUptr->Stop("Installing update(s): " + updateReason);
          if (updateServerOnInterval)
          {
            UpdateServer(
              logger
            , *updaterUptr, configSptr->GetUpdateServerRetryDelaySeconds());
          }
          if (updateModFrameworkOnInterval)
          {
            UpdateFramework(
              logger
            , *updaterUptr
            , configSptr->GetUpdateModFrameworkRetryDelaySeconds()
            , updateServerOnInterval);
          }
          LOG_INFO(logger, "Update(s) complete; starting server");
          if (!serverUptr->Start())
          {
            LOG_ERROR(logger, "Server failed to start");
            retVal = RLS_EXIT_UPDATE;
            break;
          }
          // resume timer thread
          ::SetTimerState(TimerState::RUN);
        }
      }
      // handle server health check timer notification
      if (threadData::notifyMainServer_)
      {
        threadData::notifyMainServer_ = false;
        // check if server is running
        if (serverUptr->IsRunning())
        {
          // server is running; check for protocol via RCON
          // just poll every time, as RCON connection seems to die if we don't
          //  use it?
          if (const auto& serverInfo(serverUptr->GetInfo());
              serverInfo.valid_)
          {
            LOG_INFO(logger, "rustLaunchSite: Got server info via RCON: players=" << serverInfo.players_ << ", protocol=" << serverInfo.protocol_);
          }
        }
        // server is not running
        else if (configSptr->GetProcessAutoRestart())
        {
          // configured to automatically restart
          // pause timers during server restart
          ::SetTimerState(TimerState::PAUSE);
          LOG_INFO(logger, "Server stopped unexpectedly");
          // check for updates while the server is down
          const auto [updateServerOnRelaunch, updateModFrameworkOnRelaunch] =
            UpdateCheck(
              logger
            , *updaterUptr
            , configSptr->GetUpdateServerOnRelaunch()
            , configSptr->GetUpdateModFrameworkOnRelaunch()
            , configSptr->GetUpdateModFrameworkOnServerUpdate())
          ;
          if (updateServerOnRelaunch)
          {
            UpdateServer(
              logger
            , *updaterUptr
            , configSptr->GetUpdateServerRetryDelaySeconds());
          }
          if (updateModFrameworkOnRelaunch)
          {
            UpdateFramework(
              logger
            , *updaterUptr
            , configSptr->GetUpdateModFrameworkRetryDelaySeconds()
            , updateServerOnRelaunch);
          }
          // relaunch server
          LOG_INFO(logger, "Relaunching server");
          if (!serverUptr->Start())
          {
            LOG_ERROR(logger, "Server failed to relaunch");
            retVal = RLS_EXIT_RESTART;
            break;
          }
          ::SetTimerState(TimerState::RUN);
        }
        else
        {
          // configured to shutdown on unexpected server stop
          ::SetTimerState(TimerState::STOP);
          LOG_ERROR(logger, "Server stopped unexpectedly");
          retVal = RLS_EXIT_RESTART;
          break;
        }
      }
      // end of main loop
    }

    LOG_INFO(logger, "Exited main loop; beginning shutdown process");
    LOG_INFO(logger, "Stopping timer thread");
    ::SetTimerState(TimerState::STOP);
    timerThreadUptr->join();
    LOG_INFO(logger, "Stopping server (if running)");
    serverUptr->Stop(GetShutdownReason(
      logger
    , configSptr->GetProcessReasonPath(), "Server manager shutting down"));
  }
  catch (const std::exception& e)
  {
    LOG_ERROR(logger, "ERROR: Unhandled exception: " << e.what());
    retVal = RLS_EXIT_EXCEPTION;
  }
  catch(...)
  {
    LOG_ERROR(logger, "ERROR: Unknown exception");
    retVal = RLS_EXIT_EXCEPTION;
  }

  // std::thread blows up the application if not joined before exit, so check
  //  again in case we're here due to catching an exception
  if (timerThreadUptr && timerThreadUptr->joinable())
  {
    ::SetTimerState(TimerState::STOP);
    timerThreadUptr->join();
  }

  LOG_INFO(logger, "Exiting");

  return retVal;
}

void Stop()
{
  // attempt to signal main()
  std::unique_lock lock{threadData::mutex_};
  threadData::notifyMainStop_ = true;
  threadData::cvMain_.notify_all();
}
}
