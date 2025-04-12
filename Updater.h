#ifndef UPDATER_H
#define UPDATER_H

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace rustLaunchSite
{
class Config;
class Downloader;

/// @brief Rust server and Carbon/Oxide modding framework updater facility
/// @details Implements all facilities and logic relating to checking for,
///  downloading, and installing updates for the Rust dedicated server and
///  Carbon/Oxide modding framework software, based on rustLaunchSite's
///  application configuration. Does *not* implement periodic update checking,
///  server (re)starts, etc. Only the constructor should throw exceptions.
/// @todo Support non-release builds of Carbon/Oxide?
class Updater
{
public:

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
  virtual ~Updater();

  /// @brief Check whether an update is available for the configured modding
  ///  framework (i.e. Carbon/Oxide)
  /// @details This can be called regardless of server state, except maybe when
  ///  an update is being installed. Does nothing if no modding framework is
  ///  configured, or configuration was deemed unusable.
  /// @return Boolean indication of whether a modding framework update is
  ///  available, or @c false if check skipped due to configuration
  bool CheckFramework() const;

  /// @brief Check whether Rust dedicated server update is available
  /// @details This can be called regardless of server state, except maybe when
  ///  an update is being installed. Will check regardless of configuration
  ///  options, so it is up to the caller to enforce these.
  /// @return Boolean indication of whether a server update is available
  bool CheckServer() const;

  /// @brief Download and install latest configured modding framework release
  /// @details Verifies download and then overwrites current install. Caller is
  ///  responsible for determining whether this is actually warranted, as well
  ///  as for ensuring the server is not running. Logs a warning if an
  ///  installation of the configured modding framework was not detected at
  ///  startup, unless @c suppressWarning is set to @c true.
  /// @param suppressWarning @c false (default) if a warning should be logged
  ///  when called and no preexisting modding framework installation was
  ///  detected, or @c true to suppress the warning (NOTE: Carbon/Oxide still
  ///  won't be updated in this case)
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

  // shared pointer to Config facility
  std::shared_ptr<const Config> cfgSptr_;
  // shared pointer to Downloader facility
  std::shared_ptr<Downloader> downloaderSptr_;
  // server installation base path from rustLaunchSite configuration
  std::filesystem::path serverInstallPath_;
  // path to Steam manifest file for server installation
  std::filesystem::path appManifestPath_;
  // path to SteamCMD binary as reported by manifest file
  std::filesystem::path steamCmdPath_;
  // directory in which modding framework downloads should be temporarily stored
  std::filesystem::path downloadPath_;
  // path to Carbon/Oxide modding framework DLL derived from server install path
  // may be empty if not installed, and/or modding framework updating disabled
  std::filesystem::path frameworkDllPath_;
};
}

#endif // UPDATER_H
