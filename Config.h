#ifndef CONFIG_H
#define CONFIG_H

#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace rustLaunchSite
{
/// @brief rustLaunchSite application configuration facility
/// @details Abstracts use of config file parser to load config file data into
///  an instance of this class, which can then be queried for settings. Only
///  the constructor should throw exceptions.
class Config
{
public:

  enum class ModFrameworkType { NONE, CARBON, OXIDE };

  enum class SeedStrategy { FIXED, LIST, RANDOM };

  struct Parameter
  {
    std::optional<bool>        boolValue_;
    std::optional<double>      doubleValue_;
    std::optional<int>         intValue_;
    std::optional<std::string> stringValue_;

    explicit Parameter()                     : boolValue_(false) {}
    explicit Parameter(const bool b)         : boolValue_(b)     {}
    explicit Parameter(const double d)       : doubleValue_(d)   {}
    explicit Parameter(const int i)          : intValue_(i)      {}
    explicit Parameter(const std::string& s) : stringValue_(s)   {}

    Parameter(const Parameter& rhs) = default;

    Parameter(Parameter&& rhs) noexcept
      : boolValue_  (std::move(rhs.boolValue_))
      , doubleValue_(std::move(rhs.doubleValue_))
      , intValue_   (std::move(rhs.intValue_))
      , stringValue_(std::move(rhs.stringValue_))
    {
    }

    ~Parameter() = default;

    Parameter& operator= (const Parameter& rhs)
    {
      if (&rhs == this) return *this;
      boolValue_   = rhs.boolValue_;
      doubleValue_ = rhs.doubleValue_;
      intValue_    = rhs.intValue_;
      stringValue_ = rhs.stringValue_;
      return *this;
    }

    std::string ToString() const
    {
      std::stringstream s;
      if (boolValue_)   { s << *boolValue_;   return s.str(); }
      if (doubleValue_) { s << *doubleValue_; return s.str(); }
      if (intValue_)    { s << *intValue_;    return s.str(); }
      // if (stringValue_) { return std::string("\"") + *stringValue_ + '\"'; }
      if (stringValue_) { return *stringValue_; }
      return "<UNKNOWN>";
    }
  };

  using ParameterMapType = std::map<std::string, Parameter, std::less<>>;

  /// @brief Primary constructor
  /// @details Creates an instance that is populated with data from the
  ///  specified config file.
  /// @param configFile Configuration file to load
  /// @throw @c std::invalid_argument on parse or validation failure
  explicit Config(std::filesystem::path configFile);

  // accessor methods for loaded settings

  std::filesystem::path GetInstallPath()                      const
    { return installPath_; }
  std::string           GetInstallIdentity()                  const
    { return installIdentity_; }
  std::filesystem::path GetPathsCache()                       const
    { return pathsCache_; }
  std::filesystem::path GetPathsDownload()                    const
    { return pathsDownload_; }
  bool                  GetProcessAutoRestart()               const
    { return processAutoRestart_; }
  int                   GetProcessShutdownDelaySeconds()      const
    { return processShutdownDelaySeconds_; }
  std::string           GetRconPassword()                     const
    { return rconPassword_; }
  std::string           GetRconIP()                           const
    { return rconIP_; }
  int                   GetRconPort()                         const
    { return rconPort_; }
  bool                  GetRconPassthroughIP()                const
    { return rconPassthroughIP_; }
  bool                  GetRconPassthroughPort()              const
    { return rconPassthroughPort_; }
  bool                  GetRconLog()                          const
    { return rconLog_; }
  SeedStrategy          GetSeedStrategy()                     const
    { return seedStrategy_; }
  int                   GetSeedFixed()                        const
    { return seedFixed_; }
  std::vector<int>      GetSeedList()                         const
    { return seedList_; }
  bool                  GetUpdateServerOnInterval()           const
    { return updateServerOnInterval_; }
  bool                  GetUpdateServerOnRelaunch()           const
    { return updateServerOnRelaunch_; }
  bool                  GetUpdateServerOnStartup()            const
    { return updateServerOnStartup_; }
  bool                  GetUpdateModFrameworkOnInterval()     const
    { return updateModFrameworkOnInterval_; }
  bool                  GetUpdateModFrameworkOnRelaunch()     const
    { return updateModFrameworkOnRelaunch_; }
  bool                  GetUpdateModFrameworkOnServerUpdate() const
    { return updateModFrameworkOnServerUpdate_; }
  bool                  GetUpdateModFrameworkOnStartup()      const
    { return updateModFrameworkOnStartup_; }
  ModFrameworkType      GetUpdateModFrameworkType()           const
    { return updateModFrameworkType_; }
  int                   GetUpdateIntervalMinutes()            const
    { return updateIntervalMinutes_; }
  bool                  GetWipeOnProtocolChange()             const
    { return wipeOnProtocolChange_; }
  bool                  GetWipeBlueprints()                   const
    { return wipeBlueprints_; }
  ParameterMapType      GetMinusParams()                      const
    { return minusParams_; }
  ParameterMapType      GetPlusParams()                       const
    { return plusParams_; }

private:

  // rustLaunchSite settings

  std::filesystem::path installPath_ = {};
  std::string           installIdentity_ = {};
  std::filesystem::path pathsCache_ = {};
  std::filesystem::path pathsDownload_ = {};
  bool                  processAutoRestart_ = {};
  int                   processShutdownDelaySeconds_ = {};
  std::string           rconPassword_ = {};
  std::string           rconIP_ = {};
  int                   rconPort_ = {};
  bool                  rconPassthroughIP_ = {};
  bool                  rconPassthroughPort_ = {};
  bool                  rconLog_ = {};
  SeedStrategy          seedStrategy_ = SeedStrategy::RANDOM;
  int                   seedFixed_ = {};
  std::vector<int>      seedList_ = {};
  bool                  updateServerOnInterval_ = {};
  bool                  updateServerOnRelaunch_ = {};
  bool                  updateServerOnStartup_ = {};
  bool                  updateModFrameworkOnInterval_ = {};
  bool                  updateModFrameworkOnRelaunch_ = {};
  bool                  updateModFrameworkOnServerUpdate_ = {};
  bool                  updateModFrameworkOnStartup_ = {};
  ModFrameworkType      updateModFrameworkType_ = ModFrameworkType::NONE;
  int                   updateIntervalMinutes_ = {};
  bool                  wipeOnProtocolChange_ = {};
  bool                  wipeBlueprints_ = {};

  // dedicatedServer settings

  ParameterMapType minusParams_ = {};
  ParameterMapType plusParams_ = {};

  // disabled constructors/operators

  Config() = delete;
  Config(const Config&) = delete;
  Config& operator= (const Config&) = delete;
};
}

#endif // CONFIG_H
