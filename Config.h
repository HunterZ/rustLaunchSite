#ifndef CONFIG_H
#define CONFIG_H

#include <filesystem>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rustLaunchSite
{
class Logger;

/// @brief rustLaunchSite application configuration facility
/// @details Abstracts use of config file parser to load config file data into
///  an instance of this class, which can then be queried for settings. Only
///  the constructor should throw exceptions.
class Config
{
public:

  enum class ModFrameworkType { NONE, CARBON, OXIDE };
  static std::string ToString(const ModFrameworkType type)
  {
    using enum ModFrameworkType;
    switch (type)
    {
      case NONE:   return "None";
      case CARBON: return "Carbon";
      case OXIDE:  return "Oxide";
    }
    return "Unknown";
  }

  enum class SeedStrategy { FIXED, LIST, RANDOM };

  struct Parameter
  {
    const std::variant<bool, double, int, std::string> data_ = {};

    Parameter() = delete;
    explicit Parameter(const bool         b) : data_(b) {}
    explicit Parameter(const double       d) : data_(d) {}
    explicit Parameter(const int          i) : data_(i) {}
    explicit Parameter(const std::string& s) : data_(s) {}
    explicit Parameter(std::string&&      s) : data_(std::move(s)) {}

    Parameter(const Parameter&) = default;
    Parameter(Parameter&&) noexcept = default;

    ~Parameter() = default;

    Parameter& operator= (const Parameter&) = delete;
    Parameter& operator= (Parameter&&) = delete;

    constexpr bool IsBool() const
    {
      return std::holds_alternative<bool>(data_);
    }

    constexpr bool IsDouble() const
    {
      return std::holds_alternative<double>(data_);
    }

    constexpr bool IsInt() const
    {
      return std::holds_alternative<int>(data_);
    }

    constexpr bool IsString() const
    {
      return std::holds_alternative<std::string>(data_);
    }

    constexpr bool GetBool() const
    {
      if (!IsBool())
      {
        throw std::runtime_error("Called Parameter::GetBool() on a non-bool");
      }
      return std::get<bool>(data_);
    }

    constexpr double GetDouble() const
    {
      if (!IsDouble())
      {
        throw std::runtime_error("Called Parameter::GetDouble() on a non-double");
      }
      return std::get<double>(data_);
    }

    constexpr int GetInt() const
    {
      if (!IsInt())
      {
        throw std::runtime_error("Called Parameter::GetInt() on a non-int");
      }
      return std::get<int>(data_);
    }

    constexpr std::string GetString() const
    {
      if (!IsString())
      {
        throw std::runtime_error("Called Parameter::GetString() on a non-string");
      }
      return std::get<std::string>(data_);
    }

    constexpr std::string ToString() const
    {
      return std::visit(
        [](const auto& arg) { return std::format("{}", arg); }, data_);
    }
  };

  using ParameterMapType = std::map<std::string, Parameter, std::less<>>;

  /// @brief Primary constructor
  /// @details Creates an instance that is populated with data from the
  ///  specified config file.
  /// @param configFile Configuration file to load
  /// @throw @c std::invalid_argument on parse or validation failure
  explicit Config(Logger& logger, std::filesystem::path configFile);

  // accessor methods for loaded settings

  std::filesystem::path GetInstallPath()                         const
    { return installPath_; }
  std::string           GetInstallIdentity()                     const
    { return installIdentity_; }
  bool                  GetProcessAutoRestart()                  const
    { return processAutoRestart_; }
  std::filesystem::path GetProcessReasonPath()                   const
    { return processReasonPath_; }
  int                   GetProcessShutdownDelaySeconds()         const
    { return processShutdownDelaySeconds_; }
  std::string           GetRconPassword()                        const
    { return rconPassword_; }
  std::string           GetRconIP()                              const
    { return rconIP_; }
  int                   GetRconPort()                            const
    { return rconPort_; }
  bool                  GetRconPassthroughIP()                   const
    { return rconPassthroughIP_; }
  bool                  GetRconPassthroughPort()                 const
    { return rconPassthroughPort_; }
  bool                  GetRconLog()                             const
    { return rconLog_; }
  SeedStrategy          GetSeedStrategy()                        const
    { return seedStrategy_; }
  int                   GetSeedFixed()                           const
    { return seedFixed_; }
  std::vector<int>      GetSeedList()                            const
    { return seedList_; }
  std::filesystem::path GetSteamcmdPath()                        const
    { return steamcmdPath_; }
  bool                  GetUpdateServerOnInterval()              const
    { return updateServerOnInterval_; }
  bool                  GetUpdateServerOnRelaunch()              const
    { return updateServerOnRelaunch_; }
  bool                  GetUpdateServerOnStartup()               const
    { return updateServerOnStartup_; }
  int                   GetUpdateServerRetryDelaySeconds()       const
    { return updateServerRetryDelaySeconds_; }
  bool                  GetUpdateModFrameworkOnInterval()        const
    { return updateModFrameworkOnInterval_; }
  bool                  GetUpdateModFrameworkOnRelaunch()        const
    { return updateModFrameworkOnRelaunch_; }
  bool                  GetUpdateModFrameworkOnServerUpdate()    const
    { return updateModFrameworkOnServerUpdate_; }
  bool                  GetUpdateModFrameworkOnStartup()         const
    { return updateModFrameworkOnStartup_; }
  int                   GetUpdateModFrameworkRetryDelaySeconds() const
    { return updateModFrameworkRetryDelaySeconds_; }
  ModFrameworkType      GetUpdateModFrameworkType()              const
    { return updateModFrameworkType_; }
  int                   GetUpdateIntervalMinutes()               const
    { return updateIntervalMinutes_; }
  bool                  GetWipeOnProtocolChange()                const
    { return wipeOnProtocolChange_; }
  bool                  GetWipeBlueprints()                      const
    { return wipeBlueprints_; }
  ParameterMapType      GetMinusParams()                         const
    { return minusParams_; }
  ParameterMapType      GetPlusParams()                          const
    { return plusParams_; }

private:

  // rustLaunchSite settings

  std::filesystem::path installPath_ = {};
  std::string           installIdentity_ = {};
  bool                  processAutoRestart_ = {};
  std::filesystem::path processReasonPath_ = {};
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
  std::filesystem::path steamcmdPath_ = {};
  bool                  updateServerOnInterval_ = {};
  bool                  updateServerOnRelaunch_ = {};
  bool                  updateServerOnStartup_ = {};
  int                   updateServerRetryDelaySeconds_ = {};
  bool                  updateModFrameworkOnInterval_ = {};
  bool                  updateModFrameworkOnRelaunch_ = {};
  bool                  updateModFrameworkOnServerUpdate_ = {};
  bool                  updateModFrameworkOnStartup_ = {};
  int                   updateModFrameworkRetryDelaySeconds_ = {};
  ModFrameworkType      updateModFrameworkType_ = ModFrameworkType::NONE;
  int                   updateIntervalMinutes_ = {};
  bool                  wipeOnProtocolChange_ = {};
  bool                  wipeBlueprints_ = {};

  // dedicatedServer settings

  ParameterMapType minusParams_ = {};
  ParameterMapType plusParams_ = {};

  // logger

  Logger& logger_;

  // disabled constructors/operators

  Config() = delete;
  Config(const Config&) = delete;
  Config& operator= (const Config&) = delete;
};
}

#endif // CONFIG_H
