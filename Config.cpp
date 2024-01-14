#include "Config.h"

#include <iostream>
#include <libconfig.h++>
#include <stdexcept>
#include <sstream>

namespace
{
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
        pMap.try_emplace(name, static_cast<int>(setting));
      }
      break;
      case libconfig::Setting::TypeFloat:
      {
        // std::cout << "Setting " << setting.getPath() << ", TypeFloat => Name " << name << ", double, Value " << static_cast<double>(setting) << std::endl;
        pMap.try_emplace(name, static_cast<double>(setting));
      }
      break;
      case libconfig::Setting::TypeString:
      {
        // std::cout << "Setting " << setting.getPath() << ", TypeString => Name " << name << ", string, Value " << static_cast<std::string>(setting) << std::endl;
        pMap.try_emplace(name, static_cast<std::string>(setting));
      }
      break;
      case libconfig::Setting::TypeBoolean:
      {
        // std::cout << "Setting " << setting.getPath() << ", TypeBoolean => Name " << name << ", bool, Value " << static_cast<bool>(setting) << std::endl;
        pMap.try_emplace(name, static_cast<bool>(setting));
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
Config::Config(const std::string& configFile)
{
  // attempt to parse configFile via libconfig
  libconfig::Config cfg;
  try
  {
    cfg.readFile(configFile);
  }
  catch (const libconfig::FileIOException& e)
  {
    throw std::invalid_argument(
      std::string("Unable to read config file: ") + configFile
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
    if (!installPath_.empty() && installPath_.back() != '\\')
    {
      std::cout << "WARNING: Configured install path is missing a trailing slash" << std::endl;
      installPath_ += '\\';
    }
    installIdentity_ = cfg.lookup("rustLaunchSite.install.identity").c_str();

    // paths
    pathsCache_ = cfg.lookup("rustLaunchSite.paths.cache").c_str();
    pathsDownload_ = cfg.lookup("rustLaunchSite.paths.download").c_str();
    if (!pathsDownload_.empty() && pathsDownload_.back() != '\\')
    {
      std::cout << "WARNING: Configured download path is missing a trailing slash" << std::endl;
      pathsDownload_ += '\\';
    }

    // process
    processAutoRestart_ = (
      cfg.exists("rustLaunchSite.process.autoRestart")
      && cfg.lookup("rustLaunchSite.process.autoRestart")
    );
    // default optional integer to zero
    processShutdownDelaySeconds_ = 0;
    if (cfg.exists("rustLaunchSite.process.shutdownDelaySeconds"))
    {
      processShutdownDelaySeconds_ = cfg.lookup("rustLaunchSite.process.shutdownDelaySeconds");
      // collapse other possible "disable" values to zero
      if (processShutdownDelaySeconds_ < 0) { processShutdownDelaySeconds_ = 0; }
    }

    // rcon
    rconPassword_ = cfg.lookup("rustLaunchSite.rcon.password").c_str();
    rconIP_ = cfg.lookup("rustLaunchSite.rcon.ip").c_str();
    rconPort_ = cfg.lookup("rustLaunchSite.rcon.port");
    // check for optional settings existence before looking up
    // bools that default to false can just "and" together the existence and
    //  value
    rconPassthroughIP_ = (
      cfg.exists("rustLaunchSite.rcon.passthrough.ip")
      && cfg.lookup("rustLaunchSite.rcon.passthrough.ip")
    );
    rconPassthroughPort_ = (
      cfg.exists("rustLaunchSite.rcon.passthrough.port")
      && cfg.lookup("rustLaunchSite.rcon.passthrough.port")
    );
    rconLog_ = (
      cfg.exists("rustLaunchSite.rcon.log")
      && cfg.lookup("rustLaunchSite.rcon.log")
    );

    // seed
    // string that needs to be converted to an enum
    seedStrategy_ = SeedStrategy::RANDOM;
    if (cfg.exists("rustLaunchSite.seed.strategy"))
    {
      const std::string& seedStrategy(cfg.lookup("rustLaunchSite.seed.strategy"));
      if (seedStrategy == "fixed") { seedStrategy_ = SeedStrategy::FIXED; }
      else if (seedStrategy == "list") { seedStrategy_ = SeedStrategy::LIST; }
      else if (seedStrategy == "random") { seedStrategy_ = SeedStrategy::RANDOM; }
      else
      {
        throw std::invalid_argument(
          std::string("Invalid rustLaunchSite.seed.strategy value: ")
          + seedStrategy
        );
      }
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
    updateOnLaunch_ = (
      cfg.exists("rustLaunchSite.update.onLaunch")
      && cfg.lookup("rustLaunchSite.update.onLaunch")
    );
    updateServer_ = (
      cfg.exists("rustLaunchSite.update.server")
      && cfg.lookup("rustLaunchSite.update.server")
    );
    updateOxide_ = (
      cfg.exists("rustLaunchSite.update.oxide")
      && cfg.lookup("rustLaunchSite.update.oxide")
    );
    updateIntervalMinutes_ = 0;
    if (cfg.exists("rustLaunchSite.update.intervalMinutes"))
    {
      updateIntervalMinutes_ = cfg.lookup("rustLaunchSite.update.intervalMinutes");
      if (updateIntervalMinutes_ < 0) { updateIntervalMinutes_ = 0; }
    }

    // wipe
    wipeOnProtocolChange_ = (
      cfg.exists("rustLaunchSite.wipe.onProtocolChange")
      && cfg.lookup("rustLaunchSite.wipe.onProtocolChange")
    );
    wipeBlueprints_ = (
      cfg.exists("rustLaunchSite.wipe.blueprints")
      && cfg.lookup("rustLaunchSite.wipe.blueprints")
    );

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
