#ifndef UPDATER_H
#define UPDATER_H

#include <memory>
#include <string>
#include <utility>

namespace rustLaunchSite
{
  class Config;
  class Downloader;

  /// @brief Rust server / Oxide plugin framework update management facility
  /// @details Implements all facilities and logic relating to checking for,
  ///  downloading, and installing updates for the Rust dedicated server and
  ///  Oxide plugin framework software, based on rustLaunchSite's application
  ///  configuration. Does *not* implement periodic update checking, server
  ///  (re)starts, etc. Only the constructor should throw exceptions.
  class Updater
  {
    public:

      /// @brief Primary constructor
      /// @details Reads relevant config settings into member variables.
      ///  Validates configuration, adjusting operating modes as necessary to
      ///  account for recoverable discrepencies between confgiuration and
      ///  detected installation/system states - or aborting in the case of
      ///  unrecoverable issues.
      /// @param cfg Reference to application configuration instance
      /// @throw @c std::invalid_argument or @c std::runtime_error if
      ///  unrecoverable conditions are detected (e.g. unable to allocate
      ///  dependencies, basic application configuration appears invalid, etc.)
      explicit Updater(const Config& cfg);

      /// @brief Destructor
      /// @details Stops periodic update check thread if it's running.
      virtual ~Updater();

      /// @brief Check whether Oxide update is available
      /// @details This can be called regardless of server state, except maybe
      ///  when an update is being installed.
      /// @return @c true if Oxide update available, @c false if no update
      ///  available or Oxide update checking disabled
      bool CheckOxide();

      /// @brief Check whether Rust dedicated server update is available
      /// @details This can be called regardless of server state, except maybe
      ///  when an update is being installed.
      /// @return @c true if server update available, @c false if no update
      ///  available or server update checking disabled
      bool CheckServer();

      /// @brief Download and install latest Oxide release
      /// @details Verifies download and then overwrites current install. Caller
      ///  is responsible for determining whether this is actually warranted, as
      ///  well as for ensuring the server is not running. Logs a warning if an
      ///  Oxide install was not detected at startup, unless @c suppressWarning
      ///  is set to @c true
      /// @param suppressWarning @c false (default) if a warning should be
      ///  logged when called and no preexisting Oxide installation was
      ///  detected, or @c true to suppress the warning (NOTE: Oxide still won't
      ///  be updated in this case)
      void UpdateOxide(const bool suppressWarning = false);

      /// @brief Check for, install, and validate latest RustDedicated release
      /// @details Runs SteamCMD to do all the work. Caller is responsible for
      ///  determining whether this is actually warranted, as well as for
      ///  ensuring the server is not running.
      void UpdateServer();

    protected:

    private:

      // Open Steam app manifest file, find first instance of specified key, and
      //  return corresponding value with double quotes stripped off. Returns
      //  empty string if key not found.
      std::string GetAppManifestValue(const std::string& key);

      // Get version number of the current Oxide installation, or empty if not
      //  found
      std::string GetInstalledOxideVersion();

      // Get beta/branch of the current rust dedicated server installation, or
      //  empty if not found (which indicates the default "public" branch)
      std::string GetInstalledServerBranch();

      // Get build number of the current rust dedicated server installation, or
      //  empty if not found
      std::string GetInstalledServerBuild();

      // Get build number of the latest server release available on steam for
      //  the given branch/beta name, or empty if not found. If branch name is
      //  empty, "public" will be assumed
      std::string GetLatestServerBuild(const std::string& branch = "");

      // Get version number of the latest Oxide release available on umod.org,
      //  or empty if not found
      std::string GetLatestOxideVersion();

      // Get client-server protocol version currently running on the server
      // this only needs to be checked on startup; if it changes, the server
      //  manager should be triggered to peform wipe processing
      // TODO: move this to Server class, or to a wipe manager class?
      // std::string GetRunningServerProtocol();

      // disabled constructors/operators

      Updater() = delete;
      Updater(const Updater&) = delete;
      Updater& operator= (const Updater&) = delete;

      // unique pointer to Downloader facility
      // this is a pointer to avoid leaking the Downloader header via this one
      std::unique_ptr<Downloader> downloaderUptr_;
      // server installation base path from rustLaunchSite configuration
      std::string serverInstallPath_;
      // path to Steam manifest file for server installation
      std::string appManifestPath_;
      // path to SteamCMD binary as reported by manifest file
      std::string steamCmdPath_;
      // path to Oxide DLL derived from server install path
      // may be empty if Oxide not installed, and/or Oxide updating disabled
      std::string oxideDllPath_;
      // directory in which Oxide downloads should be temporarily stored
      std::string downloadPath_;
      // whether server update checks should be performed
      // defaults to configuration file value, but disabled if necessary state
      //  does not exist to support functionality
      bool serverUpdateCheck_;
      // whether Oxide update checks should be performed
      // defaults to configuration file value, but disabled if necessary state
      //  does not exist to support functionality
      bool oxideUpdateCheck_;
  };
}

#endif // UPDATER_H
