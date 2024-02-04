#ifndef UPDATER_H
#define UPDATER_H

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rustLaunchSite
{
class Config;
class Downloader;

/// @brief Rust server and Carbon/Oxide plugin framework updater facility
/// @details Implements all facilities and logic relating to checking for,
///  downloading, and installing updates for the Rust dedicated server and
///  Carbon/Oxide plugin framework software, based on rustLaunchSite's
///  application configuration. Does *not* implement periodic update checking,
///  server (re)starts, etc. Only the constructor should throw exceptions.
/// @todo Support non-release builds of Carbon/Oxide?
/// @todo Collapse Carbon/Oxide facilities as much as possible?
class Updater
{
public:

  /// @brief Enum for specifying which plugin framework to manage
  enum class PluginFramework
  {
    NONE, CARBON, OXIDE
  };

  /// @brief Primary constructor
  /// @details Reads relevant config settings into member variables. Validates
  ///  configuration, adjusting operating modes as necessary to account for
  ///  recoverable discrepencies between confgiuration and detected
  ///  installation/system states - or aborting in the case of unrecoverable
  ///  issues.
  /// @param cfgSptr Shared pointer to application configuration instance
  /// @param downloaderUptr Shared pointer to download facility instance
  /// @throw @c std::invalid_argument or @c std::runtime_error if unrecoverable
  ///  conditions are detected (e.g. unable to allocate dependencies, basic
  ///  application configuration appears invalid, etc.)
  explicit Updater(
    std::shared_ptr<const Config> cfgSptr,
    std::shared_ptr<Downloader> downloaderSptr
  );

  /// @brief Destructor
  /// @details Stops periodic update check thread if it's running.
  virtual ~Updater();

  /// @brief Check whether Carbon/Oxide update is available
  /// @details This can be called regardless of server state, except maybe when
  ///  an update is being installed.
  /// @return @c true if Carbon/Oxide update available, @c false if no update
  ///  available or Carbon/Oxide update checking disabled
  bool CheckFramework() const;

  /// @brief Check whether Rust dedicated server update is available
  /// @details This can be called regardless of server state, except maybe when
  ///  an update is being installed.
  /// @return @c true if server update available, @c false if no update
  ///  available or server update checking disabled
  bool CheckServer() const;

  /// @brief Download and install latest Carbon/Oxide release
  /// @details Verifies download and then overwrites current install. Caller is
  ///  responsible for determining whether this is actually warranted, as well
  ///  as for ensuring the server is not running. Logs a warning if a
  ///  Carbon/Oxide install was not detected at startup, unless
  ///  @c suppressWarning is set to @c true
  /// @param suppressWarning @c false (default) if a warning should be logged
  ///  when called and no preexisting Carbon/Oxide installation was detected, or
  ///  @c true to suppress the warning (NOTE: Carbon/Oxide still won't be
  ///  updated in this case)
  void UpdateFramework(const bool suppressWarning = false) const;

  /// @brief Check for, install, and validate latest RustDedicated release
  /// @details Runs SteamCMD to do all the work. Caller is responsible for
  ///  determining whether this is actually warranted, as well as for ensuring
  ///  the server is not running.
  void UpdateServer() const;

private:

  // Open Steam app manifest file at given path, find key with given
  //  period-delimited path, and return corresponding value with double quotes
  //  stripped off.
  // Returns empty string on error or key not found.
  // Logs a warning if warn=true (default) and the specified path could not be
  //  found.
  static std::string GetAppManifestValue(
    const std::filesystem::path& appManifestPath,
    const std::string_view keyPath,
    const bool warn = true
  );

  // Get version number of the current Carbon/Oxide installation, or empty if
  //  not found
  std::string GetInstalledFrameworkVersion() const;

  // Get beta/branch of the current rust dedicated server installation, or empty
  //  if not found (which indicates the default "public" branch)
  std::string GetInstalledServerBranch() const;

  // Get build number of the current rust dedicated server installation, or
  //  empty if not found
  std::string GetInstalledServerBuild() const;

  // Get build number of the latest server release available on steam for the
  //  given branch/beta name, or empty if not found. If branch name is empty,
  //  "public" will be assumed
  std::string GetLatestServerBuild(const std::string_view branch = {}) const;

  // Get download URL for latest Carbon/Oxide release on GitHub, or empty if not
  //  found
  std::string GetLatestFrameworkURL() const;

  // Get version number of the latest Carbon/Oxide release available on GitHub,
  //  or empty if not found
  std::string GetLatestFrameworkVersion() const;

  // disabled constructors/operators

  Updater() = delete;
  Updater(const Updater&) = delete;
  Updater& operator= (const Updater&) = delete;

  // shared pointer to Downloader facility
  std::shared_ptr<Downloader> downloaderSptr_;
  // server installation base path from rustLaunchSite configuration
  std::filesystem::path serverInstallPath_;
  // path to Steam manifest file for server installation
  std::filesystem::path appManifestPath_;
  // path to SteamCMD binary as reported by manifest file
  std::filesystem::path steamCmdPath_;
  // directory in which Oxide downloads should be temporarily stored
  std::filesystem::path downloadPath_;
  // whether server update checks should be performed
  // defaults to configuration file value, but disabled if necessary state does
  //  not exist to support functionality
  bool serverUpdateCheck_;
  // whether/which Carbon/Oxide update checks should be performed
  // defaults to configuration file value, but disabled if necessary state does
  //  not exist to support functionality
  PluginFramework frameworkUpdateCheck_= {PluginFramework::NONE};
  // path to Carbon/Oxide plugin framework DLL derived from server install path
  // may be empty if not installed, and/or plugin framework updating disabled
  std::filesystem::path frameworkDllPath_;
};
}

#endif // UPDATER_H
