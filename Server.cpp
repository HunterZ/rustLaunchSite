#include "Server.h"

#include "Config.h"
#include "Rcon.h"

#if _MSC_VER
  // make Boost happy when building with MSVC
  #include <SDKDDKVer.h>
#endif

// #include <boost/winapi/show_window.hpp>
#include <boost/process.hpp>
#include <boost/process/extend.hpp>
#include <boost/process/windows.hpp>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace
{
// wrap a string in double-quotes if it contains spaces
inline std::string QuoteString(const std::string& s)
{
  return (
    // s.find(" ") == std::string::npos ?
      s //:
      // std::string("\"") + s + '\"'
  );
}

// boost::process extension to launch a process in a new console window
// idea from https://stackoverflow.com/a/69774875/3171290 and
//  https://stackoverflow.com/a/68751737/3171290
struct WindowsCreationFlags : boost::process::extend::handler
{
  boost::winapi::DWORD_ flags_{0};

  explicit WindowsCreationFlags(const boost::winapi::DWORD_ flags)
    : flags_(flags)
  {
  }

  // this function will be invoked at child process constructor before spawning process
  template <typename Char, typename Sequence>
  void on_setup(boost::process::extend::windows_executor<Char, Sequence> & ex)
  {
    ex.creation_flags |= flags_;
    // std::cout << "Modified Windows process creation flags: " << ex.creation_flags << std::endl;
    // std::cout << "Modified Windows handle inheritance: " << ex.inherit_handles << std::endl;
  }
};
}

namespace rustLaunchSite
{
struct ProcessImpl
{
  std::unique_ptr<boost::process::child> processUptr_;
};

Server::Server(std::shared_ptr<const Config> cfgSptr)
  : rconUptr_(std::make_unique<Rcon>(
      cfgSptr->GetRconIP(), cfgSptr->GetRconPort(), cfgSptr->GetRconPassword(),
      cfgSptr->GetRconLog()
    ))
  , rustDedicatedPath_(cfgSptr->GetInstallPath() / "RustDedicated.exe")
  , stopDelaySeconds_(cfgSptr->GetProcessShutdownDelaySeconds())
  , workingDirectory_(cfgSptr->GetInstallPath())
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
    const bool isBool(mParamData.boolValue_);
    // if this a boolean set to false, skip it
    if (isBool && !*mParamData.boolValue_) { continue; }
    // push parameter name (prefix is already prepended)
    rustDedicatedArguments_.push_back(QuoteString(mParamName));
    // if it's a boolean, skip the parameter value
    if (isBool) { continue; }
    // push value
    rustDedicatedArguments_.push_back(QuoteString(mParamData.ToString()));
  }
  //  "plus" parameters
  for (const auto& [pParamName, pParamData] : cfgSptr->GetPlusParams())
  {
    const bool isBool(pParamData.boolValue_);
    // if this a boolean set to false, skip it
    if (isBool && !*pParamData.boolValue_) { continue; }
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
      std::cout << "WARNING: Ignoring configured launch parameter `" << pParamName << "` because it's value will be determined automatically by rustLaunchSite" << std::endl;
      continue;
    }
    // push parameter name (prefix is already prepended)
    rustDedicatedArguments_.push_back(QuoteString(pParamName));
    // if it's a boolean, skip the parameter value
    if (isBool) { continue; }
    // push value
    rustDedicatedArguments_.push_back(QuoteString(pParamData.ToString()));
  }
  // now set automatically-determined parameters
  rustDedicatedArguments_.emplace_back("+rcon.password");
  rustDedicatedArguments_.push_back(QuoteString(cfgSptr->GetRconPassword()));
  if (cfgSptr->GetRconPassthroughIP())
  {
    rustDedicatedArguments_.emplace_back("+rcon.ip");
    rustDedicatedArguments_.push_back(QuoteString(cfgSptr->GetRconIP()));
  }
  if (cfgSptr->GetRconPassthroughPort())
  {
    rustDedicatedArguments_.emplace_back("+rcon.port");
    std::stringstream s;
    s << cfgSptr->GetRconPort();
    rustDedicatedArguments_.push_back(QuoteString(s.str()));
  }
  rustDedicatedArguments_.emplace_back("+rcon.web");
  rustDedicatedArguments_.emplace_back("1");
  rustDedicatedArguments_.emplace_back("+server.identity");
  rustDedicatedArguments_.push_back(QuoteString(cfgSptr->GetInstallIdentity()));
  // seed is a bit complicated
  // TODO: ...and this isn't even the final logic needed!
  rustDedicatedArguments_.emplace_back("+server.seed");
  int seed(1);
  switch (cfgSptr->GetSeedStrategy())
  {
    case Config::SeedStrategy::FIXED:
    {
      seed = cfgSptr->GetSeedFixed();
    }
    break;
    case Config::SeedStrategy::LIST:
    {
      seed = cfgSptr->GetSeedList().at(0);
    }
    break;
    case Config::SeedStrategy::RANDOM:
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
      Stop("Unexpected server manager failure");
    }
    catch (const std::exception& e)
    {
      std::cout << "WARNING: Caught exception while stopping server: " << e.what() << std::endl;
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
    std::cout << "ERROR: Error parsing RCON serverinfo response as JSON: " << e.what()
              << "\nResponse contents: " << serverInfo << std::endl;
    retVal.valid_ = false;
  }
  return retVal;
}

bool Server::IsRunning() const
{
  // we should always have an impl pointer
  if (!processImplUptr_)
  {
    std::cout << "ERROR: Invalid ProcessImpl pointer" << std::endl;
    return false;
  }
  // if we don't have a process pointer, we're not running
  // this is not an error, so return false silently
  if (!processImplUptr_->processUptr_) { return false; }
  // TODO: do we also need to check for nonzero pid like with
  //  tiny-process-library, or is boost::process smarter?
  boost::process::child& process(*(processImplUptr_->processUptr_));
  std::error_code errorCode;
  return (process.running(errorCode));
}

std::string Server::SendRconCommand(
  const std::string& command, const bool waitForResponse
)
{
  if (!IsRunning())
  {
    std::cout << "ERROR: Can't send RCON command due to server not running" << std::endl;
    return std::string();
  }
  if (!rconUptr_)
  {
    std::cout << "ERROR: Failed to send RCON command due to RCON not available/connected" << std::endl;
    return std::string();
  }
  return rconUptr_->SendCommand(command, waitForResponse ? 10000 : 0);
}

bool Server::Start()
{
  if (IsRunning())
  {
    std::cout << "WARNING: Can't start server because it's already running" << std::endl;
    return true;
  }
  if (!processImplUptr_)
  {
    std::cout << "WARNING: ProcessImpl pointer is invalid" << std::endl;
    return false;
  }
  if (processImplUptr_->processUptr_)
  {
    // don't warn since this happens in the case of an unexpected restart
    // std::cout << "WARNING: Resetting defunct server process handle" << std::endl;
    processImplUptr_->processUptr_.reset();
  }
  std::error_code errorCode;
/* NOTE: this mode is disabled because at best it detaches from RLS to the point
  that I can't seem to kill it
  if (false)
  {
    const auto conPath(boost::process::search_path("conhost.exe"));
    if (conPath.empty())
    {
      std::cout << "ERROR: Failed to find conhost" << std::endl;
      return false;
    }
    std::vector<std::string> args
    {
      // "/S", "/C",
      // "start",
      // "/D", workingDirectory_,
      // "/MAX",
      // "/WAIT",
      rustDedicatedPath_
    };
    args.insert(
      args.end(),
      rustDedicatedArguments_.begin(), rustDedicatedArguments_.end()
    );
    processImplUptr_->processUptr_ = std::make_unique<boost::process::child>(
      // boost::process::shell(errorCode),
      boost::process::exe(conPath),
      boost::process::args(args),
      boost::process::start_dir(workingDirectory_),
      // I/O redirect options and effects w/create group+console
      // - do nothing: broken scrolling, can't terminate
      // - close any/all: broken scrolling, can't terminate
      // - redirect any/all to null: looks OK, can't terminate?
      // ***could be create new console or process group that broke terminate
      // boost::process::std_in.close(),
      // boost::process::std_out.close(),
      // boost::process::std_err.close(),
      // I/O redirect options and effects w/create console only
      // - redirect: looks OK, can't terminate
      // I/O redirect options and effects w/detached only
      // - redirect: looks OK, can't terminate
      // I/O redirect options and effects w/create no window only
      // - redirect: no window, can't terminate
      boost::process::std_in  < boost::process::null,
      boost::process::std_out > boost::process::null,
      boost::process::std_err > boost::process::null,
      boost::process::error(errorCode),
      boost::process::windows::maximized //,
      // WindowsCreationFlags(
        // disconnect child process from Ctrl+C signals issued to parent
        // boost::winapi::CREATE_NEW_PROCESS_GROUP_ |
        // boost::winapi::CREATE_NO_WINDOW_
      // )
    );
  }
  else
  {
*/
  // std::cout << "***** ARGS BEGIN:" << std::endl;
  // for (const auto& arg : rustDedicatedArguments_)
  // {
  //   std::cout << "*****\t" << arg << std::endl;
  // }
  // std::cout << "***** ARGS END:" << std::endl;
  processImplUptr_->processUptr_ = std::make_unique<boost::process::child>(
    boost::process::exe(rustDedicatedPath_.string()),
    boost::process::args(rustDedicatedArguments_),
    boost::process::start_dir(workingDirectory_.string()),
    // I/O redirect options and effects:
    // - do nothing: server takes over our console and garbles it up
    // - close any/all: server spams its logs with exceptions
    // - redirect any/all to null: server logs some things twice
    boost::process::std_in  < boost::process::null,
    boost::process::std_out > boost::process::null,
    boost::process::std_err > boost::process::null,
    boost::process::error(errorCode),
    WindowsCreationFlags(
      // disconnect child process from Ctrl+C signals issued to parent
      boost::winapi::CREATE_NEW_PROCESS_GROUP_
    )
  );
/*
  }
*/
  if (!processImplUptr_->processUptr_)
  {
    std::cout << "ERROR: Failed to create server process handle" << std::endl;
    return false;
  }
  if (errorCode)
  {
    std::cout << "ERROR: Error creating server process: " << errorCode.message() << std::endl;
    processImplUptr_->processUptr_.reset();
    return false;
  }
  // auto& process(*processImplUptr_->process_);
  for (std::size_t i(0); i < 10 && !IsRunning(); ++i)
  {
    std::cout << "WARNING: Server not running - waiting..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!IsRunning())
  {
    std::cout << "ERROR: Server failed to launch" << std::endl;
    processImplUptr_->processUptr_.reset();
    return false;
  }
  std::cout << "Server launched successfully" << std::endl;
  // std::cout
  //   << "id=" << processImplUptr_->processUptr_->id()
  //   << ", handle=" << processImplUptr_->processUptr_->native_handle()
  //   << std::endl;
  return true;
}

void Server::Stop(const std::string& reason)
{
  if (!IsRunning())
  {
    // std::cout << "WARNING: Can't stop server because it's not running" << std::endl;
    return;
  }
  std::cout << "Stop(): Stopping server for reason: " << reason << std::endl;
  if (!processImplUptr_ || !processImplUptr_->processUptr_)
  {
    std::cout << "ERROR: Process handle/impl pointer is null" << std::endl;
    return;
  }
  boost::process::child& process(*processImplUptr_->processUptr_);
  // TODO: notify Discord someday?
  if (rconUptr_ && rconUptr_->IsConnected())
  {
    // delay shutdown if/as appropriate
    StopDelay(reason);
    // send RCON quit command and wait for some amount of time for shutdown
    std::cout << "Commanding server quit via RCON" << std::endl;
    SendRconCommand("quit", true);
    for (std::size_t i(0); i < 10 && IsRunning(); ++i)
    {
      std::cout << "Waiting for server to quit..." << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  else
  {
    std::cout << "WARNING: RCON is not available; cannot issue shutdown commands" << std::endl;
  }
  std::error_code errorCode;
  if (IsRunning())
  {
    std::cout << "WARNING: Server still running; performing process kill" << std::endl;
    process.terminate(errorCode);
  }
  if (errorCode)
  {
    std::cout << "WARNING: Process library returned error code: " << errorCode.message() << std::endl;
  }

  if (const auto exitCode(process.exit_code()); exitCode)
  {
    std::cout << "WARNING: Server process returned nonzero exit code: " << exitCode << std::endl;
  }
  // dump the pointer, since we can't re-launch the process at this point
  // NOTE: this invalidates local reference `process`
  processImplUptr_->processUptr_.reset();
}

void Server::StopDelay(std::string_view reason)
{
  if (!stopDelaySeconds_)
  {
    std::cout << "Skipping shutdown delay checks" << std::endl;
    return;
  }

  std::cout << "Performing shutdown delay checks" << std::endl;
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
    const std::chrono::seconds markIntervalSeconds(
      remainingTimeSeconds.count() > 300 ? 300 :
      remainingTimeSeconds.count() >  60 ?  60 :
      remainingTimeSeconds.count() >  10 ?  10 :
                                      1
    );
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
    std::cout
      << serverInfo.players_ << " player(s) online; delaying shutdown by up to "
      << s.str() << " second(s)" << std::endl;
    std::string sayString("say *** Shutdown in ");
    sayString.append(s.str()).append(" second(s)");
    if (!reason.empty())
    {
      sayString.append(" for reason: ").append(reason);
    }
    // don't wait for response, as it comes in with id=-1
    SendRconCommand(sayString, false);
    // sleep until next mark
    std::cout
      << "Sleeping from " << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
      << " until " << std::chrono::duration_cast<std::chrono::seconds>(nextMarkTime.time_since_epoch()).count()
      << "; latest shutdown at " << std::chrono::duration_cast<std::chrono::seconds>(shutdownTime.time_since_epoch()).count() << std::endl;
    std::this_thread::sleep_until(nextMarkTime);
  }
}
}
