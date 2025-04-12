#include "Config.h"

#include <boost/process.hpp>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>

namespace
{
// return value for the given key under the given JSON object if it exists, or a
//  default value otherwise
// NOTE: assumes that T has a copy constructor, plus a default constructor if no
//  default value is provided
template<typename T>
T GetOptionalValue(
  const nlohmann::json& j, std::string_view key, const T& defaultValue = {})
{
  if (j.contains(key)) { return j.at(key).template get<T>(); }
  return defaultValue;
}

// assign to the given object the value for the given key under the given JSON
//  object if the key exists, or assign the default value otherwise
// NOTE: assumes that T has an assignment operator, plus a default constructor
//  if no default value is provided
template<typename T>
void GetOptionalValueTo(
  T& dest
, const nlohmann::json& j
, std::string_view key
, const T& defaultValue = {})
{
  if (j.contains(key))
  {
    j.at(key).get_to(dest);
  }
  else
  {
    dest = defaultValue;
  }
}

// populate given parameter map with config settings under given JSON tree
// NOTES:
// - this is called recursively to walk the tree
// - tree structure is flattened by concatenating successive levels' key names
//    to `path`, using `.` as a separator
// - it is expected that the JSON node that represents the initial `path`
//    starting point will be passed in, and `path` will be used as its name in
//    place of its actual key in order to support a custom parameter prefix
void GetParametersTo(
  rustLaunchSite::Config::ParameterMapType& pMap
, const nlohmann::json& j
, const std::string& path
)
{
  // std::cout << "GetParametersTo(): Called with path=" << path << std::endl;

  // iterate over all items under j
  for (const auto& [key, value] : j.items())
  {
    // add item's name to current level starting path to get its full path
    const std::string& itemPath{path + key};
    // store data values in map, or recruse into objects
    switch (value.type())
    {
      case nlohmann::json::value_t::boolean:
      {
        pMap.try_emplace(itemPath, value.template get<bool>());
        // std::cout << "GetParametersTo(): Emplaced bool " << itemPath << " = " << pMap.at(itemPath).ToString() << std::endl;
      }
      break;
      case nlohmann::json::value_t::number_float:
      {
        pMap.try_emplace(itemPath, value.template get<double>());
        // std::cout << "GetParametersTo(): Emplaced double " << itemPath << " = " << pMap.at(itemPath).ToString() << std::endl;
      }
      break;
      case nlohmann::json::value_t::number_integer:
      case nlohmann::json::value_t::number_unsigned:
      {
        pMap.try_emplace(itemPath, value.template get<int>());
        // std::cout << "GetParametersTo(): Emplaced int " << itemPath << " = " << pMap.at(itemPath).ToString() << std::endl;
      }
      break;
      case nlohmann::json::value_t::object:
      {
        // std::cout << "GetParametersTo(): Recursing into " << itemPath << std::endl;
        GetParametersTo(pMap, value, itemPath + ".");
      }
      break;
      case nlohmann::json::value_t::string:
      {
        pMap.try_emplace(itemPath, value.template get<std::string>());
        // std::cout << "GetParametersTo(): Emplaced string " << itemPath << " = " << pMap.at(itemPath).ToString() << std::endl;
      }
      break;
      default:
      {
        std::cout << "WARNING: Ignoring JSON itemPath='" << itemPath << "' with unsupported type" << std::endl;
      }
    }
  }
}
}
namespace rustLaunchSite
{
Config::Config(std::filesystem::path configFile)
{
  configFile.make_preferred();
  // attempt to parse configFile via nlohmann/json
  nlohmann::json j{};
  try
  {
    j = nlohmann::json::parse(std::ifstream{configFile}, nullptr, true, true);
  }
  catch (const nlohmann::json::parse_error& e)
  {
    std::stringstream s;
    s << e.byte;
    throw std::invalid_argument(
      std::string("JSON parsing exception at byte ") + s.str()
      + " of config file '" + configFile.string() + "': " + e.what()
    );
  }
  catch (const nlohmann::json::exception& e)
  {
    throw std::invalid_argument(
      std::string("JSON general exception while parsing config file '")
      + configFile.string() + "': " + e.what()
    );
  }
  catch (const std::exception& e)
  {
    throw std::invalid_argument(
      std::string("C++ general exception while parsing config file '")
      + configFile.string() + "': " + e.what()
    );
  }
  catch (...)
  {
    throw std::invalid_argument(
      std::string("Unknown exception while parsing config file '")
      + configFile.string() + "'"
    );
  }
  // at this point the parse has succeeded
  // grab and validate settings

  try
  {
    // *** rustLaunchSite settings ***
    const auto& jRls{j.at("rustLaunchSite")};

    // just look up required settings
    // if they don't exist, an exception will be thrown

    // install
    const auto& jRlsInstall{jRls.at("install")};
    jRlsInstall.at("path").get_to(installPath_);
    installPath_.make_preferred();
    jRlsInstall.at("identity").get_to(installIdentity_);

    // paths
    const auto& jRlsPaths{jRls.at("paths")};
    jRlsPaths.at("cache").get_to(pathsCache_);
    pathsCache_.make_preferred();
    jRlsPaths.at("download").get_to(pathsDownload_);
    pathsDownload_.make_preferred();

    // process
    if (jRls.contains("process"))
    {
      const auto& jRlsProcess{jRls.at("process")};
      GetOptionalValueTo(processAutoRestart_, jRlsProcess, "autoRestart");
      // default optional integer to zero
      GetOptionalValueTo(
        processShutdownDelaySeconds_, jRlsProcess, "shutdownDelaySeconds");
      // collapse other possible "disable" values to zero
      if (processShutdownDelaySeconds_ < 0)
      {
        processShutdownDelaySeconds_ = 0;
      }
    }

    // rcon
    const auto& jRlsRcon{jRls.at("rcon")};
    jRlsRcon.at("password").get_to(rconPassword_);
    jRlsRcon.at("ip").get_to(rconIP_);
    jRlsRcon.at("port").get_to(rconPort_);
    if (jRlsRcon.contains("passthrough"))
    {
      const auto& jRlsRconPassthrough{jRlsRcon.at("passthrough")};
      GetOptionalValueTo(rconPassthroughIP_, jRlsRconPassthrough, "ip");
      GetOptionalValueTo(rconPassthroughPort_, jRlsRconPassthrough, "port");
    }
    GetOptionalValueTo(rconLog_, jRlsRcon, "log");

    // seed
    if (jRls.contains("seed"))
    {
      const auto& jRlsSeed{jRls.at("seed")};
      // string that needs to be converted to an enum
      seedStrategy_ = SeedStrategy::RANDOM;
      const auto& seedStrategy{
        GetOptionalValue<std::string>(jRlsSeed, "strategy")};
      if (seedStrategy == "fixed") { seedStrategy_ = SeedStrategy::FIXED; }
      else if (seedStrategy == "list") { seedStrategy_ = SeedStrategy::LIST; }
      else if (seedStrategy == "random") { seedStrategy_ = SeedStrategy::RANDOM; }
      else if (!seedStrategy.empty())
      {
        throw std::invalid_argument(
          std::string("Invalid rustLaunchSite.seed.strategy value: ")
          + seedStrategy
        );
      }
      // supplemental settings may be required depending on seed strategy
      switch (seedStrategy_)
      {
        case SeedStrategy::FIXED:
        {
          jRlsSeed.at("fixed").get_to(seedFixed_);
        }
        break;
        case SeedStrategy::LIST:
        {
          jRlsSeed.at("list").get_to(seedList_);
          if (seedList_.empty())
          {
            throw std::invalid_argument(
              "Invalid rustLaunchSite.seed.list array");
          }
        }
        break;
        case SeedStrategy::RANDOM:
        {
          // no supplemental settings
        }
        break;
      }
    }

    // steamcmd
    //  prefer configured value if present
    if (jRls.contains("steamcmd"))
    {
      const auto& jRlsSteamcmd{jRls.at("steamcmd")};
      if (jRlsSteamcmd.contains("path"))
      {
        jRlsSteamcmd.at("path").get_to(steamcmdPath_);
        if (!std::filesystem::exists(steamcmdPath_))
        {
          std::cout << "WARNING: steamcmd not found at configured path " << steamcmdPath_ << "; will attempt to get from environment\n";
        }
      }
    }
    //  fall back to environment search (e.g. PATH)
    if (!std::filesystem::exists(steamcmdPath_))
    {
      steamcmdPath_ =
        boost::process::search_path("steamcmd").generic_wstring();
    }
    steamcmdPath_.make_preferred();
    if (std::filesystem::exists(steamcmdPath_))
    {
      std::cout << "INFO: using steamcmd at path: " << steamcmdPath_ << "\n";
    }
    else
    {
      std::cout << "WARNING: steamcmd not found; dependent features may not work\n";
    }

    // update
    if (jRls.contains("update"))
    {
      const auto& jRlsUpdate{jRls.at("update")};
      //  server
      if (jRlsUpdate.contains("server"))
      {
        const auto& jRlsUpdateServer{jRlsUpdate.at("server")};
        GetOptionalValueTo(
          updateServerOnInterval_, jRlsUpdateServer, "onInterval");
        GetOptionalValueTo(
          updateServerOnRelaunch_, jRlsUpdateServer, "onRelaunch");
        GetOptionalValueTo(
          updateServerOnStartup_, jRlsUpdateServer, "onStartup");
        GetOptionalValueTo(
          updateServerRetryDelaySeconds_, jRlsUpdateServer,
          "updateServerRetryDelaySeconds"
        );
        if (updateServerRetryDelaySeconds_ < 0)
        {
          updateServerRetryDelaySeconds_ = 0;
        }
      }
      //  modFramework
      if (jRlsUpdate.contains("modFramework"))
      {
        const auto& jRlsUpdateModFramework{jRlsUpdate.at("modFramework")};
        // process type first, because we want to force other values to false if
        //  it does not resolve to a valid value
        const auto& modFrameworkType{GetOptionalValue<std::string>(
          jRlsUpdateModFramework, "type")};
        if ("carbon" == modFrameworkType)
        {
          updateModFrameworkType_ = ModFrameworkType::CARBON;
        }
        else if ("oxide" == modFrameworkType)
        {
          updateModFrameworkType_ = ModFrameworkType::OXIDE;
        }
        else if (!modFrameworkType.empty())
        {
          std::cout << "WARNING: Ignoring unsupported modFramework.type value: '" << modFrameworkType << "'" << std::endl;
        }
        if (updateModFrameworkType_ != ModFrameworkType::NONE)
        {
          GetOptionalValueTo(updateModFrameworkOnInterval_,
            jRlsUpdateModFramework, "onInterval");
          GetOptionalValueTo(updateModFrameworkOnRelaunch_,
            jRlsUpdateModFramework, "onRelaunch");
          GetOptionalValueTo(updateModFrameworkOnServerUpdate_,
            jRlsUpdateModFramework, "onServerUpdate");
          GetOptionalValueTo(updateModFrameworkOnStartup_,
            jRlsUpdateModFramework, "onStartup");
          GetOptionalValueTo(
            updateModFrameworkRetryDelaySeconds_, jRlsUpdateModFramework,
            "updateModFrameworkRetryDelaySeconds"
          );
          if (updateModFrameworkRetryDelaySeconds_ < 0)
          {
            updateModFrameworkRetryDelaySeconds_ = 0;
          }
        }
      }
      GetOptionalValueTo(updateIntervalMinutes_, jRlsUpdate, "intervalMinutes");
      // enforce validity & consistency here, to simplify dependent logic
      if (updateIntervalMinutes_ < 0)
      {
        updateIntervalMinutes_ = 0;
      }
      else if (updateIntervalMinutes_
        && !updateServerOnInterval_ && !updateModFrameworkOnInterval_)
      {
        std::cout << "WARNING: Ignoring update.intervalMinutes value because update.server and update.modFramework onInterval are both false: '" << updateIntervalMinutes_ << "'" << std::endl;
        updateIntervalMinutes_ = 0;
      }
      if (!updateIntervalMinutes_)
      {
        if (updateServerOnInterval_)
        {
          std::cout << "WARNING: Ignoring update.server.onInterval=true because update.intervalMinutes=0" << std::endl;
          updateServerOnInterval_ = false;
        }
        if (updateModFrameworkOnInterval_)
        {
          std::cout << "WARNING: Ignoring update.modFramework.onInterval=true because update.intervalMinutes=0" << std::endl;
          updateModFrameworkOnInterval_ = false;
        }
      }
    }

    // wipe
    if (jRls.contains("wipe"))
    {
      const auto& jRlsWipe{jRls.at("wipe")};
      GetOptionalValueTo(wipeOnProtocolChange_, jRlsWipe, "onProtocolChange");
      GetOptionalValueTo(wipeBlueprints_, jRlsWipe, "blueprints");
    }

    // *** rustDedicated settings ***
    if (j.contains("rustDedicated"))
    {
      const auto& jRd{j.at("rustDedicated")};
      if (jRd.contains("minusParams"))
      {
        GetParametersTo(minusParams_, jRd.at("minusParams"), "-");
      }
      if (jRd.contains("plusParams"))
      {
        GetParametersTo(plusParams_, jRd.at("plusParams"), "+");
      }
    }
  }
  catch (const nlohmann::json::exception& e)
  {
    throw std::invalid_argument(
      std::string("JSON general exception while processing config data: ")
      + e.what()
    );
  }
  catch (const std::exception& e)
  {
    throw std::invalid_argument(
      std::string("C++ general exception while processing config data: ")
      + e.what()
    );
  }
  catch (...)
  {
    throw std::invalid_argument(
      "Unknown exception while processing config data");
  }
}
}
