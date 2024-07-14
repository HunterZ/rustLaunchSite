#include "Config.h"

#include <iostream>
#include <libconfig.h++>
#include <stdexcept>
#include <sstream>

namespace
{
// helper function for getting optional Config values, when we want to fall back
//  to some default value when no explicit one exists
// NOTES:
// - path is std::string rather than std::string_view because libconfig is
//    internally a C library that ultimately needs a guaranteed null-terminated
//    string, so we'd have to launder a std::string_view through a temporary
//    std::string anyway
// - assumes that T has a copy constructor, as libconfig++ lookup() depends on
//    overloaded function operators to convert settings to the desired type
template<typename T>
T GetConfigValue(const libconfig::Config& cfg, const std::string& path, const T& defaultValue = {})
{
  if (!cfg.exists(path)) { return defaultValue; }
  return cfg.lookup(path).operator T();
}

// internal logic to populate parameter map with config settings under path:
// - scalar setting names are appended to `prefix` to form key name
// - when a group setting is encountered, a recursive call is made to process
//   the group's sub-settings, with:
//   - groupPath set to the group setting's full path
//   - prefix set to passed-in prefix with group name and a period appended
void GetParameters(
  rustLaunchSite::Config::ParameterMapType& pMap
, const libconfig::Config& cfg
, const std::string& groupPath
, const std::string& prefix
)
{
  // std::cout << "GetParameters(_, _, " << groupPath << ", " << prefix << ")" << std::endl;
  if (!cfg.exists(groupPath)) { return; }

  const auto& pGroup(cfg.lookup(groupPath));
  for (const auto& setting : pGroup)
  {
    const std::string& name(prefix + setting.getName());
    switch (setting.getType())
    {
      case libconfig::Setting::TypeNone:
      {
        // treat settings with no value as false booleans
        // std::cout << "Setting " << setting.getPath() << ", TypeNone => Name " << name << ", bool, Value false" << std::endl;
        pMap.try_emplace(name, false);
      }
      break;
      case libconfig::Setting::TypeInt:
      case libconfig::Setting::TypeInt64:
      {
        // std::cout << "Setting " << setting.getPath() << ", TypeInt* => Name " << name << ", int, Value " << static_cast<int>(setting) << std::endl;
        pMap.try_emplace(name, setting.operator int());
      }
      break;
      case libconfig::Setting::TypeFloat:
      {
        // std::cout << "Setting " << setting.getPath() << ", TypeFloat => Name " << name << ", double, Value " << static_cast<double>(setting) << std::endl;
        pMap.try_emplace(name, setting.operator double());
      }
      break;
      case libconfig::Setting::TypeString:
      {
        // std::cout << "Setting " << setting.getPath() << ", TypeString => Name " << name << ", string, Value " << static_cast<std::string>(setting) << std::endl;
        pMap.try_emplace(name, setting.operator std::string());
      }
      break;
      case libconfig::Setting::TypeBoolean:
      {
        // std::cout << "Setting " << setting.getPath() << ", TypeBoolean => Name " << name << ", bool, Value " << static_cast<bool>(setting) << std::endl;
        pMap.try_emplace(name, setting.operator bool());
      }
      break;
      case libconfig::Setting::TypeGroup:
      {
        // std::cout << "Recursing " << setting.getPath() << ", TypeGroup => Prefix " << name << "." << std::endl;
        // perform a recursive call to process group's sub-settings
        GetParameters(pMap, cfg, setting.getPath(), name + ".");
      }
      break;
      case libconfig::Setting::TypeArray:
      case libconfig::Setting::TypeList:
      {
        throw std::invalid_argument(
          std::string("Unsupported type for setting: ") + setting.getPath()
        );
      }
      break;
    }
  }
}
}

namespace rustLaunchSite
{
Config::Config(std::filesystem::path configFile)
{
  configFile.make_preferred();
  // attempt to parse configFile via libconfig
  libconfig::Config cfg;
  try
  {
    cfg.readFile(configFile.string());
  }
  catch (const libconfig::FileIOException& e)
  {
    throw std::invalid_argument(
      std::string("Exception reading config file '") + configFile.string() + "': "
      + e.what()
    );
  }
  catch (const libconfig::ParseException& e)
  {
    std::stringstream s;
    s << e.getLine();
    throw std::invalid_argument(
      std::string("Parse failure at ") + e.getFile() + ":" + s.str() + ": "
      + e.getError()
    );
  }
  // at this point the parse has succeeded
  // grab and validate settings

  try
  {
    // *** rustLaunchSite settings ***

    // just look up required settings
    // if they don't exist, an exception will be thrown

    // install
    installPath_ = cfg.lookup("rustLaunchSite.install.path").c_str();
    installPath_.make_preferred();
    installIdentity_ = cfg.lookup("rustLaunchSite.install.identity").c_str();

    // paths
    pathsCache_ = cfg.lookup("rustLaunchSite.paths.cache").c_str();
    pathsCache_.make_preferred();
    pathsDownload_ = cfg.lookup("rustLaunchSite.paths.download").c_str();
    pathsDownload_.make_preferred();

    // process
    processAutoRestart_ = GetConfigValue<bool>(
      cfg, "rustLaunchSite.process.autoRestart");
    // default optional integer to zero
    processShutdownDelaySeconds_ = GetConfigValue<int>(
      cfg, "rustLaunchSite.process.shutdownDelaySeconds");
    // collapse other possible "disable" values to zero
    if (processShutdownDelaySeconds_ < 0) { processShutdownDelaySeconds_ = 0; }

    // rcon
    rconPassword_ = cfg.lookup("rustLaunchSite.rcon.password").c_str();
    rconIP_ = cfg.lookup("rustLaunchSite.rcon.ip").c_str();
    rconPort_ = cfg.lookup("rustLaunchSite.rcon.port");
    // check for optional settings existence before looking up
    // bools that default to false can just "and" together the existence and
    //  value
    rconPassthroughIP_ = GetConfigValue<bool>(
      cfg, "rustLaunchSite.rcon.passthrough.ip");
    rconPassthroughPort_ = GetConfigValue<bool>(
      cfg, "rustLaunchSite.rcon.passthrough.port");
    rconLog_ = GetConfigValue<bool>(cfg, "rustLaunchSite.rcon.log");

    // seed
    // string that needs to be converted to an enum
    seedStrategy_ = SeedStrategy::RANDOM;
    const auto& seedStrategy{GetConfigValue<std::string>(
      cfg, "rustLaunchSite.seed.strategy")};
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
    seedFixed_ = 0;
    switch (seedStrategy_)
    {
      case SeedStrategy::FIXED:
      {
        seedFixed_ = cfg.lookup("rustLaunchSite.seed.fixed");
      }
      break;
      case SeedStrategy::LIST:
      {
        const auto& list(cfg.lookup("rustLaunchSite.seed.list"));
        const int listLength(list.getLength());
        if (listLength < 1)
        {
          throw std::invalid_argument(
            "Invalid rustLaunchSite.seed.list array"
          );
        }
        seedList_.reserve(listLength);
        for (int i(0); i < listLength; ++i) { seedList_.push_back(list[i]); }
      }
      break;
      case SeedStrategy::RANDOM:
      {
        // no supplemental settings
      }
      break;
    }

    // update
    if (cfg.exists("rustLaunchSite.update"))
    {
      //  server
      if (cfg.exists("rustLaunchSite.update.server"))
      {
        updateServerOnInterval_ = GetConfigValue<bool>(
          cfg, "rustLaunchSite.update.server.onInterval");
        updateServerOnRelaunch_ = GetConfigValue<bool>(
          cfg, "rustLaunchSite.update.server.onRelaunch");
        updateServerOnStartup_ = GetConfigValue<bool>(
          cfg, "rustLaunchSite.update.server.onStartup");
      }
      //  modFramework
      updateModFrameworkType_ = ModFrameworkType::NONE;
      if (cfg.exists("rustLaunchSite.update.modFramework"))
      {
        const auto& modFrameworkType{GetConfigValue<std::string>(
          cfg, "rustLaunchSite.update.modFramework.type")};
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
          std::cout << "WARNING: Interpreting unsupported modFramework type value as empty: '" << modFrameworkType << "'" << std::endl;
        }
      }
      //   check type first, because we want to force other values to false if it
      //    does not resolve to a valid value
      if (updateModFrameworkType_ != ModFrameworkType::NONE)
      {
        updateModFrameworkOnInterval_ = GetConfigValue<bool>(
          cfg, "rustLaunchSite.update.modFramework.onInterval");
        updateModFrameworkOnRelaunch_ = GetConfigValue<bool>(
          cfg, "rustLaunchSite.update.modFramework.onRelaunch");
        updateModFrameworkOnServerUpdate_ = GetConfigValue<bool>(
          cfg, "rustLaunchSite.update.modFramework.onServerUpdate");
        updateModFrameworkOnStartup_ = GetConfigValue<bool>(
          cfg, "rustLaunchSite.update.modFramework.onStartup");
      }
      //  local value(s)
      updateIntervalMinutes_ = GetConfigValue<int>(
        cfg, "rustLaunchSite.update.intervalMinutes");
      //   enforce validity & consistency here, to simplify dependent logic
      if (updateIntervalMinutes_ < 0)
      {
        std::cout << "WARNING: Interpreting unsupported negative update.intervalMinutes value as 0: '" << updateIntervalMinutes_ << "'" << std::endl;
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
    wipeOnProtocolChange_ = GetConfigValue<bool>(
        cfg, "rustLaunchSite.wipe.onProtocolChange");
    wipeBlueprints_ = GetConfigValue<bool>(
        cfg, "rustLaunchSite.wipe.blueprints");

    // *** rustDedicated settings ***

    GetParameters(minusParams_, cfg, "rustDedicated.minusParams", "-");
    GetParameters(plusParams_, cfg, "rustDedicated.plusParams", "+");
  }
  catch (const libconfig::SettingTypeException& e)
  {
    throw std::invalid_argument(
      std::string("Config setting has unsupported type: ") + e.getPath()
    );
  }
  catch (const libconfig::SettingNotFoundException& e)
  {
    throw std::invalid_argument(
      std::string("Required config setting not found: ") + e.getPath()
    );
  }
}
}
