#include "Server.h"

#include "Config.h"
#include "Logger.h"
#include "Rcon.h"

#if _MSC_VER
  // make Boost happy when building with MSVC
  #include <sdkddkver.h>
#endif

#include <boost/process/v1/args.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/exe.hpp>
#include <boost/process/v1/extend.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/start_dir.hpp>
#if _MSC_VER || defined(__MINGW32__)
  #include <boost/process/v1/windows.hpp>
#else
  #include <boost/process/v1/group.hpp>
  #include <cerrno>
  #include <csignal>
#endif
#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <thread>

namespace
{
#if _MSC_VER || defined(__MINGW32__)
// boost::process extension to launch a process in a new console window
// idea from https://stackoverflow.com/a/69774875/3171290 and
//  https://stackoverflow.com/a/68751737/3171290
struct WindowsCreationFlags : boost::process::v1::extend::handler
{
  boost::winapi::DWORD_ flags_{0};

  explicit WindowsCreationFlags(const boost::winapi::DWORD_ flags)
    : flags_(flags)
  {
  }

  // this function will be invoked at child process constructor before spawning
  //  process
  template <typename Char, typename Sequence>
  void on_setup(
    boost::process::v1::extend::windows_executor<Char, Sequence> & ex)
  {
    ex.creation_flags |= flags_;
  }
};
#endif

std::filesystem::path GetRustDedicatedPath(
  std::shared_ptr<const rustLaunchSite::Config> cfgSptr)
{
#if (_MSC_VER || defined(__MINGW32__))
  return cfgSptr->GetInstallPath() / "RustDedicated.exe";
#else
  return cfgSptr->GetInstallPath() / (
    cfgSptr->GetUpdateModFrameworkType() ==
    rustLaunchSite::Config::ModFrameworkType::CARBON ?
      "carbon.sh" : "runds.sh");
#endif
}

constexpr std::chrono::seconds GetMarkIntervalSeconds(
  const std::chrono::seconds& remainingSeconds)
{
  if (remainingSeconds.count() > 300) return std::chrono::seconds{300};
  if (remainingSeconds.count() >  60) return std::chrono::seconds{ 60};
  if (remainingSeconds.count() >  10) return std::chrono::seconds{ 10};
  return                                     std::chrono::seconds{  1};
}
}

namespace rustLaunchSite
{
struct ProcessImpl
{
  std::unique_ptr<boost::process::v1::child> processUptr_;
#if (!_MSC_VER && !defined(__MINGW32__))
  boost::process::v1::group processGroup_{};
#endif
};

Server::Server(Logger& logger, std::shared_ptr<const Config> cfgSptr)
  : rconUptr_{std::make_unique<Rcon>(
      logger
    , cfgSptr->GetRconIP()
    , cfgSptr->GetRconPort()
    , cfgSptr->GetRconPassword()
    , cfgSptr->GetRconLog()
    )}
  , rustDedicatedPath_{GetRustDedicatedPath(cfgSptr)}
  , stopDelaySeconds_{
      static_cast<std::size_t>(cfgSptr->GetProcessShutdownDelaySeconds())}
  , workingDirectory_{cfgSptr->GetInstallPath()}
  , logger_{logger}
{
  // do this here, or else Sonar badgers me to use in-class initializers, which
  //  won't work with opaque types
  processImplUptr_ = std::make_unique<ProcessImpl>();
  // validate config-driven paths
  if (!std::filesystem::exists(workingDirectory_))
  {
    throw std::invalid_argument(
      std::string("Server install path does not exist: ") + workingDirectory_.string());
  }
  if (!std::filesystem::exists(rustDedicatedPath_))
  {
    throw std::invalid_argument(
      std::string("Server launch binary does not exist: ") + rustDedicatedPath_.string());
  }
  if (
    const std::filesystem::path serverIdentityPath(
      workingDirectory_ / "server" / cfgSptr->GetInstallIdentity()
    );
    !std::filesystem::exists(serverIdentityPath)
  )
  {
    throw std::invalid_argument(
      std::string("Server identity path does not exist: ") + serverIdentityPath.string());
  }
  // set up server launch arguments
  // start with parameters directly defined in config
  //  "minus" parameters
  for (const auto& [mParamName, mParamData] : cfgSptr->GetMinusParams())
  {
    const auto isBool(mParamData.IsBool());
    // if this a boolean set to false, skip it
    if (isBool && !mParamData.GetBool()) { continue; }
    // push parameter name (prefix is already prepended)
    rustDedicatedArguments_.push_back(mParamName);
    // if it's a boolean, skip the parameter value
    if (isBool) { continue; }
    // push value
    rustDedicatedArguments_.push_back(mParamData.ToString());
  }
  //  "plus" parameters
  for (const auto& [pParamName, pParamData] : cfgSptr->GetPlusParams())
  {
    const bool isBool(pParamData.IsBool());
    // if this a boolean set to false, skip it
    if (isBool && !pParamData.GetBool()) { continue; }
    // check for parameter names whose values may be overridden by
    //  rustLaunchSite configuration
    if (
      pParamName == "+rcon.password" ||
      (pParamName == "+rcon.ip" && cfgSptr->GetRconPassthroughIP()) ||
      (pParamName == "+rcon.port" && cfgSptr->GetRconPassthroughPort()) ||
      pParamName == "+rcon.web" ||
      pParamName == "+server.identity" ||
      pParamName == "+server.seed"
    )
    {
      LOGWRN(logger_, "Ignoring configured launch parameter `" << pParamName << "` because it's value will be determined automatically by rustLaunchSite");
      continue;
    }
    // push parameter name (prefix is already prepended)
    rustDedicatedArguments_.push_back(pParamName);
    // if it's a boolean, skip the parameter value
    if (isBool) { continue; }
    // push value
    rustDedicatedArguments_.push_back(pParamData.ToString());
  }
  // now set automatically-determined parameters
  rustDedicatedArguments_.emplace_back("+rcon.password");
  rustDedicatedArguments_.push_back(cfgSptr->GetRconPassword());
  if (cfgSptr->GetRconPassthroughIP())
  {
    rustDedicatedArguments_.emplace_back("+rcon.ip");
    rustDedicatedArguments_.push_back(cfgSptr->GetRconIP());
  }
  if (cfgSptr->GetRconPassthroughPort())
  {
    rustDedicatedArguments_.emplace_back("+rcon.port");
    std::stringstream s;
    s << cfgSptr->GetRconPort();
    rustDedicatedArguments_.push_back(s.str());
  }
  rustDedicatedArguments_.emplace_back("+rcon.web");
  rustDedicatedArguments_.emplace_back("1");
  rustDedicatedArguments_.emplace_back("+server.identity");
  rustDedicatedArguments_.push_back(cfgSptr->GetInstallIdentity());
  // seed is a bit complicated
  // TODO: ...and this isn't even the final logic needed!
  rustDedicatedArguments_.emplace_back("+server.seed");
  int seed(1);
  using enum rustLaunchSite::Config::SeedStrategy;
  switch (cfgSptr->GetSeedStrategy())
  {
    case FIXED:
    {
      seed = cfgSptr->GetSeedFixed();
    }
    break;
    case LIST:
    {
      seed = cfgSptr->GetSeedList().at(0);
    }
    break;
    case RANDOM:
    {
      seed = 1;
    }
    break;
  }
  std::stringstream seedStream;
  seedStream << seed;
  rustDedicatedArguments_.push_back(seedStream.str());
}

Server::~Server()
{
  if (IsRunning())
  {
    try
    {
      Stop();
    }
    catch (const std::exception& e)
    {
      LOGWRN(logger_, "Caught exception while stopping server: " << e.what());
    }
  }
}

Server::Info Server::GetInfo()
{
  Info retVal{};
  if (!IsRunning()) { return retVal; }
  const std::string& serverInfo(SendRconCommand("serverinfo", true));
  if (serverInfo.empty()) { return retVal; }
  /*
  result should be JSON - example:
  {
    "Hostname": "My Cool Server",
    "MaxPlayers": 50,
    "Players": 0,
    "Queued": 0,
    "Joining": 0,
    "EntityCount": 114685,
    "GameTime": "05/22/2024 11:30:14",
    "Uptime": 363,
    "Map": "Procedural Map",
    "Framerate": 204.0,
    "Memory": 1638,
    "MemoryUsageSystem": 7766,
    "Collections": 151,
    "NetworkIn": 0,
    "NetworkOut": 0,
    "Restarting": false,
    "SaveCreatedTime": "04/17/2023 06:00:42",
    "Version": 2380,
    "Protocol": "2380.236.1"
  }
  */
  // need to catch exceptions because this could be invoked from destructors
  try
  {
    nlohmann::json j(nlohmann::json::parse(serverInfo));
    retVal.valid_ = (
      j.contains("Players") && j["Players"].is_number()
      && j.contains("Protocol") && j["Protocol"].is_string()
    );
    if (retVal.valid_)
    {
      retVal.players_ = j["Players"].get<std::size_t>();
      retVal.protocol_ = j["Protocol"].get<std::string>();
    }
  }
  catch (const nlohmann::json::exception& e)
  {
    LOGWRN(logger_, "Error parsing RCON serverinfo response as JSON: " << e.what() << "\nResponse contents: " << serverInfo);
    retVal.valid_ = false;
  }
  return retVal;
}

bool Server::IsRunning() const
{
  // we should always have an impl pointer
  if (!processImplUptr_)
  {
    LOGWRN(logger_, "Invalid ProcessImpl pointer");
    return false;
  }
  // if we don't have a process pointer, we're not running
  // this is not an error, so return false silently
  if (!processImplUptr_->processUptr_) { return false; }
  // TODO: do we also need to check for nonzero pid like with
  //  tiny-process-library, or is boost::process smarter?
  auto& process{*(processImplUptr_->processUptr_)};
  std::error_code errorCode{};
  return process.running(errorCode);
}

std::string Server::SendRconCommand(
  const std::string& command, const bool waitForResponse
)
{
  if (!IsRunning())
  {
    LOGWRN(logger_, "Can't send RCON command due to server not running");
    return std::string();
  }
  if (!rconUptr_)
  {
    LOGWRN(logger_, "Failed to send RCON command due to RCON not available/connected");
    return std::string();
  }
  return rconUptr_->SendCommand(command, waitForResponse ? 10000 : 0);
}

bool Server::Start()
{
  if (IsRunning())
  {
    LOGWRN(logger_, "Can't start server because it's already running");
    return true;
  }
  if (!processImplUptr_)
  {
    LOGWRN(logger_, "ProcessImpl pointer is invalid");
    return false;
  }
  if (processImplUptr_->processUptr_)
  {
    // don't warn since this happens in the case of an unexpected restart
    // LOGWRN(logger_, "Resetting defunct server process handle");
    processImplUptr_->processUptr_.reset();
  }
  std::error_code errorCode{};
  processImplUptr_->processUptr_ = std::make_unique<boost::process::v1::child>(
    boost::process::v1::exe(rustDedicatedPath_.string()),
    boost::process::v1::args(rustDedicatedArguments_),
    boost::process::v1::start_dir(workingDirectory_.string()),
    // I/O redirect options and effects:
    // - do nothing: server takes over our console and garbles it up
    // - close any/all: server spams its logs with exceptions
    // - redirect any/all to null: server logs some things twice
    boost::process::v1::std_in  < boost::process::v1::null,
    boost::process::v1::std_out > boost::process::v1::null,
    boost::process::v1::std_err > boost::process::v1::null,
    boost::process::v1::error(errorCode)
  #if _MSC_VER || defined(__MINGW32__)
    ,
    WindowsCreationFlags(
      // disconnect child process from Ctrl+C signals issued to parent
      boost::winapi::CREATE_NEW_PROCESS_GROUP_
    )
  #else
    // disconnect child process from POSIX signals issued to parent
    , processImplUptr_->processGroup_
  #endif
  );
  if (!processImplUptr_->processUptr_)
  {
    LOGWRN(logger_, "Failed to create server process handle");
    return false;
  }
  if (errorCode)
  {
    LOGWRN(logger_, "Error creating server process: " << errorCode.message());
    processImplUptr_->processUptr_.reset();
    return false;
  }
  for (std::size_t i(0); i < 10 && !IsRunning(); ++i)
  {
    LOGWRN(logger_, "Server not running - waiting...");
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!IsRunning())
  {
    LOGWRN(logger_, "Server failed to launch");
    processImplUptr_->processUptr_.reset();
    return false;
  }
  LOGINF(logger_, "Server launched successfully");
  return true;
}

void Server::Stop(const std::string& reason)
{
  if (!IsRunning())
  {
    // LOGWRN(logger_, "Can't stop server because it's not running");
    return;
  }
  LOGINF(logger_, "Stop(): Stopping server for reason: " << reason);
  if (!processImplUptr_ || !processImplUptr_->processUptr_)
  {
    LOGWRN(logger_, "Process handle/impl pointer is null");
    return;
  }
  auto& process(*processImplUptr_->processUptr_);
  // TODO: notify Discord someday?
  if (rconUptr_ && rconUptr_->IsConnected())
  {
    // delay shutdown if/as appropriate
    StopDelay(reason);
    // send RCON quit command and wait for some amount of time for shutdown
    LOGINF(logger_, "Commanding server quit via RCON");
    SendRconCommand("quit", true);
    for (std::size_t i(0); i < 10 && IsRunning(); ++i)
    {
      LOGINF(logger_, "Waiting for server to quit...");
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  else
  {
    LOGWRN(logger_, "RCON is not available; cannot issue shutdown commands");
  }
  std::error_code errorCode;
#if !_MSC_VER && !defined(__MINGW32__)
  // POSIX-only: send a SIGINT (Ctrl+C) because it will trigger a cleaner
  //  shutdown than the SIGKILL sent by terminate()
  if (IsRunning())
  {
    LOGWRN(logger_, "Server still running; interrupting process");
    if (-1 == kill(processImplUptr_->processUptr_->id(), SIGINT))
    {
      LOGWRN(logger_, "POSIX kill(SIGINT) returned error code: " << strerror(errno));
    }
    // interrupt shutdown isn't instantaneous, so allow for another wait cycle
    for (std::size_t i(0); i < 10 && IsRunning(); ++i)
    {
      LOGINF(logger_, "Waiting for server to terminate...");
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
#endif
  if (IsRunning())
  {
    LOGWRN(logger_, "Server still running; killing process");
    process.terminate(errorCode);
  }
  if (errorCode)
  {
    LOGWRN(logger_, "Process library returned error code: " << errorCode.message());
  }

  if (const auto exitCode(process.exit_code()); exitCode)
  {
    LOGWRN(logger_, "Server process returned nonzero exit code: " << exitCode);
  }
  // dump the pointer, since we can't re-launch the process at this point
  // NOTE: this invalidates local reference `process`
  processImplUptr_->processUptr_.reset();
}

void Server::StopDelay(std::string_view reason)
{
  if (!stopDelaySeconds_)
  {
    LOGINF(logger_, "Skipping shutdown delay checks");
    return;
  }

  LOGINF(logger_, "Performing shutdown delay checks");
  // latest possible shutdown time
  const std::chrono::steady_clock::time_point shutdownTime(
    std::chrono::steady_clock::now() +
    std::chrono::seconds(stopDelaySeconds_)
  );
  while (IsRunning() && std::chrono::steady_clock::now() <= shutdownTime)
  {
    // server info via RCON, which contains connected player count
    const Info& serverInfo(GetInfo());
    // if no RCON response or no players, exit loop and quit immediately
    // this is an optimization in case no players are on, or the last
    //  player quits while we're looping
    if (!serverInfo.valid_ || !serverInfo.players_) { break; }
    // maximum remaining delay time in seconds
    const auto remainingTimeSeconds(
      std::chrono::duration_cast<std::chrono::seconds>(
        shutdownTime - std::chrono::steady_clock::now()
      )
    );
    // how often notifications are occurring based on time left
    const auto& markIntervalSeconds(
      GetMarkIntervalSeconds(remainingTimeSeconds));
    // number of seconds until next notification
    const auto marksRemaining(
      remainingTimeSeconds / markIntervalSeconds
    );
    // number of seconds before shutdown time that next mark should occur
    const std::chrono::seconds markDeltaSeconds(
      markIntervalSeconds * marksRemaining
    );
    // time at which next mark should occur
    const std::chrono::steady_clock::time_point nextMarkTime(
      shutdownTime - std::chrono::seconds(markDeltaSeconds)
    );
    // notify user(s)
    // fudge the count by one second so that it looks nicer
    std::stringstream s;
    s << remainingTimeSeconds.count() + 1;
    LOGINF(logger_, serverInfo.players_ << " player(s) online; delaying shutdown by up to " << s.str() << " second(s)");
    std::string sayString("say *** Shutdown in ");
    sayString.append(s.str()).append(" second(s)");
    if (!reason.empty())
    {
      sayString.append(" for reason: ").append(reason);
    }
    // don't wait for response, as it comes in with id=-1
    SendRconCommand(sayString, false);
    // sleep until next mark
    LOGINF(logger_, "Sleeping from " << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() << " until " << std::chrono::duration_cast<std::chrono::seconds>(nextMarkTime.time_since_epoch()).count() << "; latest shutdown at " << std::chrono::duration_cast<std::chrono::seconds>(shutdownTime.time_since_epoch()).count());
    std::this_thread::sleep_until(nextMarkTime);
  }
}
}
