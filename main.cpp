#include "Config.h"
#include "Downloader.h"
#include "Server.h"
#include "Updater.h"

#include "ctrl-c.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
// exit codes
// TODO: use standard codes instead?
//  see https://en.cppreference.com/w/cpp/error/errc
enum RLS_EXIT
{
  SUCCESS = 0,
  ARG,      // invalid_argument
  HANDLER,  // io_error / state_not_recoverable
  START,    // no_child_process
  UPDATE,   // no_child_process
  RESTART,  // no_child_process
  EXCEPTION // interrupted
};

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
  // whether Ctrl+C handler is notifying main() to shut down
  bool notifyMainCtrlC_{false};
  // whether timer thread is notifying main() to check server health
  bool notifyMainServer_{false};
  // whether timer thread is notifying main() to check for updates
  bool notifyMainUpdater_{false};
  // whether main() is notifying timer thread to change state
  bool notifyTimerThread_{false};
};

bool HandleCtrlC(CtrlCLibrary::CtrlSignal s)
{
  if (s != CtrlCLibrary::kCtrlCSignal)
  {
    std::cout << "rustLaunchSite: WARNING: Ignoring unknown signal" << std::endl;
    return false;
  }
  // attempt to signal main()
  // CtrlCLibrary is pretty dodgy in terms of threading, so I don't know how
  //  safe/robust a solution this will be
  std::unique_lock lock{threadData::mutex_};
  threadData::notifyMainCtrlC_ = true;
  threadData::cvMain_.notify_all();
  return true;
}

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
  const rustLaunchSite::Updater& updater
, const bool checkServer
, const bool checkModFramework
, const bool updateModFrameworkOnServer)
{
  std::pair<bool, bool> retVal{false, false};
  if (checkServer)
  {
    std::cout << "rustLaunchSite: Performing server update check" << std::endl;
    retVal.first = updater.CheckServer();
  }
  const bool forceCheck{updateModFrameworkOnServer && retVal.first};
  if (checkModFramework || forceCheck)
  {
    std::cout << "rustLaunchSite: Performing mod framework update check" << std::endl;
    retVal.second = updater.CheckFramework();
  }
  return retVal;
}

// wrapper around Updater::UpdateFramework() to loop until update succeeds
void UpdateFramework(
  const rustLaunchSite::Updater& updater,
  const int retryDelaySeconds = 0, const bool suppressWarning = false)
{
  std::cout << "rustLaunchSite: Entering plugin framework update loop" << std::endl;
  bool firstTry{true};
  for(bool update{true}; update; update = updater.CheckFramework())
  {
    if (!firstTry)
    {
      std::cout << "rustLaunchSite: WARNING: Detected plugin framework version mismatch after update attempt; ";
      if (retryDelaySeconds > 0)
      {
        std::cout << "waiting for " << retryDelaySeconds << " second(s) and then trying again..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(retryDelaySeconds));
      }
      else
      {
        std::cout << "trying again..." << std::endl;
      }
    }

    updater.UpdateFramework(suppressWarning);

    firstTry = false;
  }
  std::cout << "rustLaunchSite: Completed plugin framework update loop" << std::endl;
}

// wrapper around Updater::UpdateServer() to loop until update succeeds
void UpdateServer(
  const rustLaunchSite::Updater& updater,
  const int retryDelaySeconds = 0)
{
  std::cout << "rustLaunchSite: Entering server update loop" << std::endl;
  bool firstTry{true};
  for(bool update{true}; update; update = updater.CheckServer())
  {
    if (!firstTry)
    {
      std::cout << "rustLaunchSite: WARNING: Detected server version mismatch after update attempt; " << std::endl;
      if (retryDelaySeconds > 0)
      {
        std::cout << "waiting for " << retryDelaySeconds << " second(s) and then trying again..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(retryDelaySeconds));
      }
      else
      {
        std::cout << "trying again..." << std::endl;
      }
    }

    updater.UpdateServer();

    firstTry = false;
  }
  std::cout << "rustLaunchSite: Completed server update loop" << std::endl;
}
}

int main(int argc, char* argv[])
{
  std::cout << "rustLaunchSite: Starting" << std::endl;

  if (argc <= 1)
  {
    std::cout << "rustLaunchSite: ERROR: Configuration file/path must be specified as an argument" << std::endl;
    return RLS_EXIT::ARG;
  }

  // install Ctrl+C handler
  // TODO: change this to an RAII wrapper so that we clean up at the end
  const auto handlerId(CtrlCLibrary::SetCtrlCHandler(HandleCtrlC));
  if (handlerId == CtrlCLibrary::kErrorID)
  {
    std::cout << "rustLaunchSite: ERROR: Failed to install Ctrl+C handler" << std::endl;
    return RLS_EXIT::HANDLER;
  }

  // create null pointers for all facilities we'll be instantiating, so that we
  //  can clean them up if an exception is caught
  std::shared_ptr<rustLaunchSite::Config> configSptr;
  std::unique_ptr<rustLaunchSite::Server> serverUptr;
  std::unique_ptr<rustLaunchSite::Updater> updaterUptr;
  std::unique_ptr<std::thread> timerThreadUptr;

  RLS_EXIT retVal(RLS_EXIT::SUCCESS);
  try
  {
    // load config file
    configSptr = std::make_shared<rustLaunchSite::Config>(argv[1]);
    // instantiate server manager
    serverUptr = std::make_unique<rustLaunchSite::Server>(configSptr);
    // instantiate update manager
    updaterUptr = std::make_unique<rustLaunchSite::Updater>(
      configSptr, std::make_shared<rustLaunchSite::Downloader>()
    );

    {
      const auto [updateServerOnStartup, updateModFrameworkOnStartup] =
        UpdateCheck(
          *updaterUptr
        , configSptr->GetUpdateServerOnStartup()
        , configSptr->GetUpdateModFrameworkOnStartup()
        , configSptr->GetUpdateModFrameworkOnServerUpdate())
      ;
      if (updateServerOnStartup)
      {
        UpdateServer(
          *updaterUptr, configSptr->GetUpdateServerRetryDelaySeconds());
      }
      if (updateModFrameworkOnStartup)
      {
        UpdateFramework(
          *updaterUptr
        , configSptr->GetUpdateModFrameworkRetryDelaySeconds()
        , updateServerOnStartup);
      }
    }

    // launch server
    std::cout << "rustLaunchSite: Starting server" << std::endl;
    if (!serverUptr->Start())
    {
      std::cout << "rustLaunchSite: Server failed to start; shutting down" << std::endl;
      // okay to just abort at this point
      return RLS_EXIT::START;
    }

    // start timer thread
    std::cout << "rustLaunchSite: Starting timer thread" << std::endl;
    timerThreadUptr = std::make_unique<std::thread>(
      &TimerFunction, 1, configSptr->GetUpdateIntervalMinutes()
    );
    if (!timerThreadUptr)
    {
      throw std::runtime_error("rustLaunchSite: Failed to instantiate timer thread");
    }

    // main loop
    // bool gotProtocol(false);
    std::cout << "rustLaunchSite: Starting main event loop" << std::endl;
    while (true)
    {
      // grab mutex for safe state variable access in loop when awake
      std::unique_lock lock(threadData::mutex_);
      // sleep until we get a notification from the timer thread
      // std::cout << "rustLaunchSite: Waiting for events" << std::endl;
      threadData::cvMain_.wait
      (
        lock,
        [](){
          return (
            threadData::notifyMainCtrlC_ ||
            threadData::notifyMainServer_ ||
            threadData::notifyMainUpdater_
          );
        }
      );
      // std::cout << "rustLauchSite: Woke up with state:"
      //   << " CtrlC=" << threadData::notifyMainCtrlC_
      //   << ", Server=" << threadData::notifyMainServer_
      //   << ", Updater=" << threadData::notifyMainUpdater_
      //   << std::endl;
      // handle Ctrl+C notification
      if (threadData::notifyMainCtrlC_)
      {
        // attempt an orderly shutdown
        std::cout << "rustLaunchSite: Ctrl+C signal caught; stopping server" << std::endl;
        ::SetTimerState(TimerState::STOP);
        serverUptr->Stop("Server manager terminated");
        // as Ctrl+C is the only orderly shutdown stimulus, we want to report a
        //  successful exit
        retVal = RLS_EXIT::SUCCESS;
        break;
      }
      // handle update check timer notification
      if (threadData::notifyMainUpdater_)
      {
        threadData::notifyMainUpdater_ = false;
        // check for updates
        const auto [updateServerOnInterval, updateModFrameworkOnInterval] =
          UpdateCheck(
            *updaterUptr
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
          // stop server
          std::cout << "rustLaunchSite: Update(s) required; stopping server" << std::endl;
          // install updates
          serverUptr->Stop("Installing updates");
          if (updateServerOnInterval)
          {
            UpdateServer(
              *updaterUptr, configSptr->GetUpdateServerRetryDelaySeconds());
          }
          if (updateModFrameworkOnInterval)
          {
            UpdateFramework(
              *updaterUptr
            , configSptr->GetUpdateModFrameworkRetryDelaySeconds()
            , updateServerOnInterval);
          }
          std::cout << "rustLaunchSite: Update(s) complete; starting server" << std::endl;
          if (!serverUptr->Start())
          {
            std::cout << "rustLaunchSite: Server failed to start; shutting down" << std::endl;
            retVal = RLS_EXIT::UPDATE;
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
          // if (!gotProtocol)
          // {
          if
          (
            const auto& serverInfo(serverUptr->GetInfo());
            serverInfo.valid_
          )
          {
            // gotProtocol = true;
            std::cout
              << "rustLaunchSite: Got server info via RCON:"
              << "\n\tplayers=" << serverInfo.players_
              << "\n\tprotocol=" << serverInfo.protocol_
              << std::endl;
    // TODO: poll server for protocol version via RCON, triggering wipe
    //  processing if a change is detected since last run
            // }
          }
        }
        // server is not running
        else if (configSptr->GetProcessAutoRestart())
        {
          // configured to automatically restart
          // pause timers during server restart
          ::SetTimerState(TimerState::PAUSE);
          std::cout << "rustLaunchSite: Server stopped unexpectedly" << std::endl;
          // check for updates while the server is down
          const auto [updateServerOnRelaunch, updateModFrameworkOnRelaunch] =
            UpdateCheck(
              *updaterUptr
            , configSptr->GetUpdateServerOnRelaunch()
            , configSptr->GetUpdateModFrameworkOnRelaunch()
            , configSptr->GetUpdateModFrameworkOnServerUpdate())
          ;
          if (updateServerOnRelaunch)
          {
            UpdateServer(
              *updaterUptr, configSptr->GetUpdateServerRetryDelaySeconds());
          }
          if (updateModFrameworkOnRelaunch)
          {
            UpdateFramework(
              *updaterUptr
            , configSptr->GetUpdateModFrameworkRetryDelaySeconds()
            , updateServerOnRelaunch);
          }
          // relaunch server
          std::cout << "rustLaunchSite: Relaunching server" << std::endl;
          if (!serverUptr->Start())
          {
            std::cout << "rustLaunchSite: Server failed to relaunch; shutting down" << std::endl;
            retVal = RLS_EXIT::RESTART;
            break;
          }
          ::SetTimerState(TimerState::RUN);
        }
        else
        {
          // configured to shutdown on unexpected server stop
          ::SetTimerState(TimerState::STOP);
          std::cout << "rustLaunchSite: Server stopped unexpectedly; shutting down" << std::endl;
          retVal = RLS_EXIT::RESTART;
          break;
        }
      }
      // end of main loop
    }

    std::cout << "rustLaunchSite: Exited main loop; beginning shutdown process" << std::endl;
    std::cout << "rustLaunchSite: Stopping timer thread" << std::endl;
    ::SetTimerState(TimerState::STOP);
    timerThreadUptr->join();
    std::cout << "rustLaunchSite: Stopping server (if running)" << std::endl;
    serverUptr->Stop("Server manager shutting down");
  }
  catch (const std::exception& e)
  {
    std::cout << "rustLaunchSite: ERROR: Unhandled exception: " << e.what() << std::endl;
    retVal = RLS_EXIT::EXCEPTION;
  }
  catch(...)
  {
    std::cout << "rustLaunchSite: ERROR: Unknown exception" << std::endl;
    retVal = RLS_EXIT::EXCEPTION;
  }

  // std::thread blows up the application if not joined before exit, so check
  //  again in case we're here due to catching an exception
  if (timerThreadUptr && timerThreadUptr->joinable())
  {
    ::SetTimerState(TimerState::STOP);
    timerThreadUptr->join();
  }

  std::cout << "rustLaunchSite: Exiting" << std::endl;

  return retVal;
}
