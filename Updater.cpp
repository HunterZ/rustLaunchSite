#include "Updater.h"

#include "Config.h"
#include "Downloader.h"

#include <boost/process.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <fstream>
#include <iostream>
#include <kubazip/zip/zip.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace
{
inline bool IsDirectory(const std::filesystem::path& path)
{
  const auto& targetPath(
    std::filesystem::is_symlink(path) ?
      std::filesystem::read_symlink(path) : path
  );
  return std::filesystem::is_directory(targetPath);
}

inline bool IsWritable(const std::filesystem::path& path)
{
  // this sucks and may not work with MSVC, but std::filesystem is garbage in
  //  this area
  // (for MSVC, may need to include io.h and use _access() instead?)
  return (0 == access(path.string().c_str(), W_OK));
}
/*
using ZipStatus = std::pair<ssize_t, ssize_t>;
int ZipExtractCallback(const char* filenamePtr, void* argPtr)
{
  ZipStatus dummyStatus;
  auto* statusPtr(static_cast<ZipStatus*>(argPtr));
  if (!statusPtr) { statusPtr = &dummyStatus; }
  auto& [count, total] = *statusPtr;
  ++count;
  std::cout << "Extracted file " << count << "/" << total << ": " << filenamePtr << "\n";
  return 0;
}
*/

// kubazip zip_entry_extract() callback handler
std::size_t ZipExtractToFile
(
  void* contextPtr, [[maybe_unused]] std::uint64_t offset,
  const void* dataPtr, const std::size_t dataSize
)
{
  auto outFilePtr{static_cast<std::fstream*>(contextPtr)};
  auto inDataPtr{static_cast<const char*>(dataPtr)};
  if (!outFilePtr || !inDataPtr)
  {
    std::cout << "ERROR: Null data passed to zip extraction callback handler\n";
    return 0;
  }
  // std::cout << "Extracting " << dataSize << " bytes at offset " << offset << "\n";
  outFilePtr->write(inDataPtr, dataSize);
  return dataSize;
}

enum class SteamCmdReadState
{
  FIND_INFO_START,
  FIND_INFO_END,
  COMPLETE
};

enum class ToStringCase
{
  LOWER, TITLE, UPPER
};

const std::vector<std::string_view> FRAMEWORK_STRING_LOWER
  { "none", "carbon", "oxide" };
const std::vector<std::string_view> FRAMEWORK_STRING_TITLE
  { "None", "Carbon", "Oxide" };
const std::vector<std::string_view> FRAMEWORK_STRING_UPPER
  { "NONE", "CARBON", "OXIDE" };

std::string_view ToString(
  const rustLaunchSite::Updater::PluginFramework framework,
  const ToStringCase stringCase
)
{
  std::size_t index{0};
  switch (framework)
  {
    case rustLaunchSite::Updater::PluginFramework::NONE:   index = 0; break;
    case rustLaunchSite::Updater::PluginFramework::CARBON: index = 1; break;
    case rustLaunchSite::Updater::PluginFramework::OXIDE:  index = 2; break;
  }
  switch (stringCase)
  {
    case ToStringCase::LOWER: return FRAMEWORK_STRING_LOWER.at(index);
    case ToStringCase::TITLE: return FRAMEWORK_STRING_TITLE.at(index);
    case ToStringCase::UPPER: return FRAMEWORK_STRING_UPPER.at(index);
  }
  return {};
}

rustLaunchSite::Updater::PluginFramework ToFramework(
  const rustLaunchSite::Config::ModFramework framework
)
{
  // yes, this is a 1:1 mapping, but we don't want a header / public API level
  //  dependency on Config::ModFramework
  switch (framework)
  {
    case rustLaunchSite::Config::ModFramework::NONE:
      break;
    case rustLaunchSite::Config::ModFramework::CARBON:
      return rustLaunchSite::Updater::PluginFramework::CARBON;
    case rustLaunchSite::Config::ModFramework::OXIDE:
      return rustLaunchSite::Updater::PluginFramework::OXIDE;
  }
  return rustLaunchSite::Updater::PluginFramework::NONE;
}

std::filesystem::path GetFrameworkDllPath(
  const std::filesystem::path& serverInstallPath,
  const rustLaunchSite::Updater::PluginFramework framework
)
{
  switch (framework)
  {
    case rustLaunchSite::Updater::PluginFramework::NONE:
      break;
    case rustLaunchSite::Updater::PluginFramework::CARBON:
      return serverInstallPath / "carbon/managed/Carbon.dll";
    case rustLaunchSite::Updater::PluginFramework::OXIDE:
      return serverInstallPath / "RustDedicated_Data/Managed/Oxide.Rust.dll";
  }
  return {};
}

const std::vector<std::string_view> FRAMEWORK_URL
{
  // NONE
  "",
  // CARBON
  "https://api.github.com/repos/CarbonCommunity/Carbon/releases/tags/production_build",
  // OXIDE
  "https://api.github.com/repos/OxideMod/Oxide.Rust/releases/latest"
};

std::string_view GetFrameworkURL(
  const rustLaunchSite::Updater::PluginFramework framework
)
{
  std::size_t index{0};
  switch (framework)
  {
    case rustLaunchSite::Updater::PluginFramework::NONE:   index = 0; break;
    case rustLaunchSite::Updater::PluginFramework::CARBON: index = 1; break;
    case rustLaunchSite::Updater::PluginFramework::OXIDE:  index = 2; break;
  }
  return FRAMEWORK_URL.at(index);
}


const std::vector<std::string_view> FRAMEWORK_ASSET
{
  // NONE
  "",
  // CARBON
  "Carbon.Windows.Release.zip",
  // OXIDE
  "Oxide.Rust.zip"
};

std::string_view GetFrameworkAsset(
  const rustLaunchSite::Updater::PluginFramework framework
)
{
  std::size_t index{0};
  switch (framework)
  {
    case rustLaunchSite::Updater::PluginFramework::NONE:   index = 0; break;
    case rustLaunchSite::Updater::PluginFramework::CARBON: index = 1; break;
    case rustLaunchSite::Updater::PluginFramework::OXIDE:  index = 2; break;
  }
  return FRAMEWORK_ASSET.at(index);
}
}

namespace rustLaunchSite
{
Updater::Updater(
  std::shared_ptr<const Config> cfgSptr,
  std::shared_ptr<Downloader> downloaderSptr
)
  : downloaderSptr_(downloaderSptr)
  , serverInstallPath_(cfgSptr->GetInstallPath())
  , downloadPath_(cfgSptr->GetPathsDownload())
  , serverUpdateCheck_(cfgSptr->GetUpdateServer())
  , frameworkUpdateCheck_(ToFramework(cfgSptr->GetUpdateModFramework()))
  , frameworkDllPath_(GetFrameworkDllPath(serverInstallPath_, frameworkUpdateCheck_))
{
  // install path must be either a directory, or a symbolic link to one
  if (!IsDirectory(serverInstallPath_))
  {
    throw std::invalid_argument(std::string("ERROR: Server install path does not exist: ") + serverInstallPath_.string());
  }
  if (!std::filesystem::exists(serverInstallPath_ / "RustDedicated.exe"))
  {
    throw std::invalid_argument(std::string("ERROR: Rust dedicated server not found in configured install path: ") + serverInstallPath_.string());
  }

  if (serverUpdateCheck_)
  {
    // derive the Steam app manifest path from the configured install location
    appManifestPath_ = serverInstallPath_ / "steamapps/appmanifest_258550.acf";
    if (std::filesystem::exists(appManifestPath_))
    {
      // extract SteamCMD utility path from manifest
      steamCmdPath_ = GetAppManifestValue(appManifestPath_, "AppState.LauncherPath");
      if (steamCmdPath_.empty())
      {
        std::cout << "WARNING: Failed to locate SteamCMD path from manifest file " << appManifestPath_ << "; automatic Steam updates disabled\n";
        serverUpdateCheck_ = false;
      }
      else if (!std::filesystem::exists(steamCmdPath_))
      {
        std::cout << "WARNING: Failed to locate SteamCMD at manifest file specified path " << steamCmdPath_ << "; automatic Steam updates disabled\n";
        steamCmdPath_.clear();
        serverUpdateCheck_ = false;
      }
    }
    else
    {
      std::cout << "WARNING: Steam app manifest file " << appManifestPath_ << " does not exist; automatic Steam updates disabled\n";
      appManifestPath_.clear();
      serverUpdateCheck_ = false;
    }
  }

  if (
    frameworkUpdateCheck_ != PluginFramework::NONE
    && !std::filesystem::exists(frameworkDllPath_)
  )
  {
    std::cout << "WARNING: " << frameworkDllPath_ << " not found; automatic " << ToString(frameworkUpdateCheck_, ToStringCase::TITLE) << " updates disabled\n";
    frameworkDllPath_.clear();
    frameworkUpdateCheck_ = PluginFramework::NONE;
  }

  if (frameworkUpdateCheck_ != PluginFramework::NONE)
  {
    if (!IsDirectory(downloadPath_))
    {
      std::cout << "WARNING: Configured download path " << downloadPath_ << " is not a directory; automatic " << ToString(frameworkUpdateCheck_, ToStringCase::TITLE) << " updates disabled\n";
      downloadPath_.clear();
      frameworkDllPath_.clear();
      frameworkUpdateCheck_ = PluginFramework::NONE;
    }
    else if (!IsWritable(downloadPath_))
    {
      std::cout << "WARNING: Configured download path " << downloadPath_ << " is not writable; automatic " << ToString(frameworkUpdateCheck_, ToStringCase::TITLE) << " updates disabled\n";
      downloadPath_.clear();
      frameworkDllPath_.clear();
      frameworkUpdateCheck_ = PluginFramework::NONE;
    }
  }

  // std::cout << "Updater initialized. Server updates " << (serverUpdateCheck_ ? "enabled" : "disabled") << ". Oxide updates " << (oxideUpdateCheck_ ? "enabled" : "disabled")<< "\n";
}

Updater::~Updater() = default;

bool Updater::CheckFramework() const
{
  const auto& frameworkTitle{ToString(frameworkUpdateCheck_, ToStringCase::TITLE)};
  if (frameworkUpdateCheck_ == PluginFramework::NONE) { return false; }
  const auto& currentVersion(GetInstalledFrameworkVersion());
  std::cout << "CheckFramework(): Installed " << frameworkTitle << " version: '" << currentVersion << "'\n";
  const auto& latestVersion(GetLatestFrameworkVersion());
  std::cout << "CheckFramework(): Latest " << frameworkTitle << " version: '" << latestVersion << "'\n";
  return (
    !currentVersion.empty() && !latestVersion.empty() &&
    currentVersion != latestVersion
  );
}

bool Updater::CheckServer() const
{
  if (!serverUpdateCheck_) { return false; }
  const std::string& currentServerVersion(GetInstalledServerBuild());
  std::cout << "CheckServer(): Installed Server version: '" << currentServerVersion << "'\n";
  const std::string& latestServerVersion(
    GetLatestServerBuild(GetInstalledServerBranch())
  );
  std::cout << "CheckServer(): Latest Server version: '" << latestServerVersion << "'\n";
  return (
    !currentServerVersion.empty() && !latestServerVersion.empty() &&
    currentServerVersion != latestServerVersion
  );
}

void Updater::UpdateFramework(const bool suppressWarning) const
{
  const std::string_view frameworkTitle{ToString(frameworkUpdateCheck_, ToStringCase::TITLE)};
  // abort if Carbon/Oxide was not already installed
  // this should also catch the case that this is called when update checking is
  //  disabled, but we don't explicity support that
  if (frameworkDllPath_.empty())
  {
    if (!suppressWarning)
    {
      std::cout << "WARNING: Cannot update " << frameworkTitle << " because a previous installation was not detected\n";
    }
    return;
  }

  // abort if any required path is empty, meaning it failed validation
  // this is pathological, but this is also meant to be production software
  if (downloadPath_.empty() || serverInstallPath_.empty())
  {
    std::cout << "ERROR: Cannot update " << frameworkTitle << " because download and/or server install path is invalid\n";
    return;
  }

  // download latest Carbon/Oxide release
  const auto& url{GetLatestFrameworkURL()};
  if (url.empty())
  {
    std::cout << "WARNING: Cannot update " << frameworkTitle << " because download URL was not found\n";
    return;
  }
  // this is now downloaded into RAM because kubazip seems to interact weirdly
  //  with std::filesystem on Windows + MSYS MinGW
  const std::vector<char>& zipData{downloaderSptr_->GetUrlToVector(url)};
  if (zipData.empty())
  {
    std::cout << "ERROR: Cannot update " << frameworkTitle << " because data was not downloaded from URL " << url << "\n";
    return;
  }

  // unzip Carbon/Oxide release into server installation directory
  // NOTE: both plugin frameworks currently release .zip files that are intended
  //  to be extracted directly into the server installation root
  //
  // first, get the number of files in the zip
  // TODO: use zip_stream_openwitherror() if vcpkg updates to newer kubazip
  zip_t* zipPtr(zip_stream_open(zipData.data(), zipData.size(), 0, 'r'));
  if (!zipPtr)
  {
    std::cout << "ERROR: Failed to open zip data with length=" << zipData.size() << " downloaded from URL " << url << "\n";
    return;
  }
  const ssize_t zipEntries(zip_entries_total(zipPtr));
  if (zipEntries <= 0)
  {
    std::cout << "ERROR: Failed to get valid file count from downloaded zip data with length=" << zipData.size() << ": " << zip_strerror(static_cast<int>(zipEntries)) << "\n";
    zip_close(zipPtr);
    return;
  }
  // loop over all zip entries
  for (ssize_t i{0}; i < zipEntries; ++i)
  {
    if
    (
      const auto openResult{zip_entry_openbyindex(zipPtr, i)};
      openResult
    )
    {
      std::cout << "ERROR: Failed to open zip entry - " << frameworkTitle << " installation may now be corrupt! Error: " << zip_strerror(openResult) << "\n";
      zip_entry_close(zipPtr);
      zip_close(zipPtr);
      return;
    }
    std::string_view entryName{zip_entry_name(zipPtr)};
    if (entryName.empty())
    {
      std::cout << "ERROR: Failed to determine zip entry name - " << frameworkTitle << " installation may now be corrupt!\n";
      zip_entry_close(zipPtr);
      zip_close(zipPtr);
      return;
    }
    const int isDirStatus{zip_entry_isdir(zipPtr)};
    if (isDirStatus < 0)
    {
      std::cout << "ERROR: Failed to determine zip entry '" << entryName << "' directory status - " << frameworkTitle << " installation may now be corrupt! Error: " << zip_strerror(isDirStatus) << "\n";
      zip_entry_close(zipPtr);
      zip_close(zipPtr);
      return;
    }
    // kubazip seems to always return isDir=0, even for obvious directory
    //  entries whose names end in a slash, so add that to the heuristic
    const bool isDir{isDirStatus != 0 || entryName.back() == '/' || entryName.back() == '\\'};
    // calculate the full path relative to server installation
    std::filesystem::path entryPath{(serverInstallPath_ / entryName).make_preferred()};
    std::cout << "Extracting zip entry #" << i+1 << "/" << zipEntries << ": " << (isDir ? "Directory" : "File") << " '" << entryName << "' to '" << entryPath << "'\n";
    // get the parent path if a file, or the full path if a dir
    std::filesystem::path containingDir{isDir ? entryPath : entryPath.parent_path()};
    // std::cout << "Creating path (unless it already exists): '" << containingDir << "'\n";
    // create the directory tree
    if
    (
      std::error_code ec{};
      // false is returned if dir already exists, so need to check ec also
      !std::filesystem::create_directories(containingDir, ec) && ec
    )
    {
      std::cout << "ERROR: Failed to replicate path '" << containingDir << "' - " << frameworkTitle << " installation may now be corrupt! Error: " << ec.message() << "\n";
      zip_entry_close(zipPtr);
      zip_close(zipPtr);
      return;
    }
    if (isDir)
    {
      // nothing else to do for a directory
      zip_entry_close(zipPtr);
      continue;
    }
    // this is a file, so extract it
    // start by opening the destination, truncating if it already exists
    std::fstream outFile
    {
      entryPath, std::ios::binary | std::ios_base::out | std::ios_base::trunc
    };
    if (!outFile.is_open() || outFile.fail())
    {
      std::cout << "ERROR: Failure opening file '" << entryPath << "' for write - " << frameworkTitle << " installation may now be corrupt!\n";
      outFile.close();
      zip_entry_close(zipPtr);
      zip_close(zipPtr);
      return;
    }
    // now pass this to kubazip with pointer to a callback that will chunk the
    //  data out to the file
    if
    (
      const int extractResult
      {
        zip_entry_extract(zipPtr, ZipExtractToFile, &outFile)
      };
      extractResult
    )
    {
      std::cout << "ERROR: Failure extracting file '" << entryPath << "' from zip - " << frameworkTitle << " installation may now be corrupt! Error: " << zip_strerror(extractResult) << "\n";
    }
    outFile.close();
    zip_entry_close(zipPtr);
  }
  zip_close(zipPtr);
}

void Updater::UpdateServer() const
{
  // abort if any required path is empty, meaning it failed validation
  if (serverInstallPath_.empty() || steamCmdPath_.empty())
  {
    std::cout << "ERROR: Cannot update server because install and/or steamcmd path is invalid\n";
    return;
  }

  std::vector<std::string> args
  {
    "+force_install_dir", serverInstallPath_.string(),
    "+login", "anonymous",
    "+app_update", "258550"
  };
  if
  (
    std::string betaKey(GetInstalledServerBranch());
    !betaKey.empty()
  )
  {
    args.emplace_back("-beta");
    args.emplace_back(std::move(betaKey));
  }
  args.emplace_back("validate");
  args.emplace_back("+quit");
  // std::cout << "Invoking SteamCMD with args:";
  // for (const auto& a : args)
  // {
  //   std::cout << " " << a;
  // }
  // std::cout<< "\n";
  std::error_code errorCode;
  const int exitCode(boost::process::system(
    boost::process::exe(steamCmdPath_.string()),
    boost::process::args(args),
    boost::process::error(errorCode)
  ));
  if (errorCode)
  {
    std::cout << "WARNING: Error running server update command: " << errorCode.message()<< "\n";
    return;
  }
  if (exitCode)
  {
    std::cout << "WARNING: SteamCMD returned nonzero exit code: " << exitCode<< "\n";
    return;
  }
  // std::cout << "Server update successful\n";
}

std::string Updater::GetAppManifestValue(
  const std::filesystem::path& appManifestPath,
  const std::string_view keyPath,
  const bool warn)
{
  std::string retVal{};

  try
  {
    boost::property_tree::ptree tree;
    boost::property_tree::read_info(appManifestPath.string(), tree);
    retVal = tree.get<std::string>(keyPath.data());
  }
  catch (const boost::property_tree::ptree_bad_path& ex)
  {
    if (warn)
    {
      std::cout << "WARNING: Exception parsing server app manifest: " << ex.what()<< "\n";
    }
    retVal.clear();
  }
  catch (const std::exception& ex)
  {
    std::cout << "WARNING: Exception parsing server app manifest: " << ex.what()<< "\n";
    retVal.clear();
  }
  catch (...)
  {
    std::cout << "WARNING: Unknown exception parsing server app manifest\n";
    retVal.clear();
  }

  // std::cout << "*** " << appManifestPath << " @ " << keyPath << " = " << retVal<< "\n";

  return retVal;
}

std::string Updater::GetInstalledFrameworkVersion() const
{
  std::string retVal;
  if (frameworkDllPath_.empty()) { return retVal; }
  // run powershell and grab all output into inStream
  boost::process::ipstream inStream;
  // for some reason boost requires explicitly requesting a PATH search unless
  //  we want to pass the entire command as a single string
  const auto psPath(boost::process::search_path("powershell.exe"));
  if (psPath.empty())
  {
    std::cout << "ERROR: Failed to find powershell\n";
    return retVal;
  }
  std::error_code errorCode;
  const int exitCode(boost::process::system(
    boost::process::exe(psPath),
    boost::process::args({
      "-Command",
      std::string("(Get-Item '") + frameworkDllPath_.string() + "').VersionInfo.ProductVersion"
    }),
    boost::process::std_out > inStream,
    boost::process::error(errorCode)
  ));
  if (errorCode)
  {
    std::cout << "ERROR: Error running " << ToString(frameworkUpdateCheck_, ToStringCase::TITLE) << " version check command: " << errorCode.message() << "\n";
    return retVal;
  }
  if (exitCode)
  {
    std::cout << "ERROR: Powershell returned nonzero exit code: " << exitCode << "\n";
    return retVal;
  }
  // grab first line of output stream into retVal string
  std::getline(inStream, retVal);
  // for some reason this has a newline at the end, so strip that off
  while (retVal.back() == '\r' || retVal.back() == '\n') { retVal.pop_back(); }
  // strip off anything starting with `+` or `-` if present
  return retVal.substr(0, retVal.find_first_of("+-"));
}

std::string Updater::GetInstalledServerBranch() const
{
  return GetAppManifestValue(appManifestPath_, "AppState.UserConfig.BetaKey", false);
}

std::string Updater::GetInstalledServerBuild() const
{
  return GetAppManifestValue(appManifestPath_, "AppState.buildid");
}

std::string Updater::GetLatestServerBuild(const std::string_view branch) const
{
  std::string retVal;
  // abort if any required path is empty, meaning it failed validation
  if (serverInstallPath_.empty() || steamCmdPath_.empty())
  {
    std::cout << "ERROR: Cannot check for server updates because install and/or steamcmd path is invalid\n";
    return retVal;
  }
  // write a script for steamcmd to run
  // this is needed because steamcmd acts very buggy when I try to use other
  //  methods
  // TODO: this should go in RLS' data directory, not the server's
  const std::filesystem::path scriptFilePath
  {
    serverInstallPath_ / "steamcmd.scr"
  };
  std::ofstream scriptFile(scriptFilePath, std::ios::trunc);
  if (!scriptFile.is_open())
  {
    std::cout << "ERROR: Failed to open steamcmd script file `" << scriptFilePath << "`\n";
    return retVal;
  }
  scriptFile
    << "force_install_dir " << serverInstallPath_ << "\n"
    << "login anonymous\n"
    << "app_info_update 1\n"
    << "app_info_print 258550\n"
    << "quit\n"
  ;
  scriptFile.close();
  // launch steamcmd and extract desired info
  boost::process::ipstream fromChild; // from child to RLS
  std::error_code errorCode;
  boost::process::child sc(
    boost::process::exe(steamCmdPath_.string()),
    boost::process::args({"+runscript", scriptFilePath.string()}),
    boost::process::std_out > fromChild,
    boost::process::error(errorCode)
  );
  // this will hold the extracted info blob as a string
  std::string steamInfo;
  // this will hold the most recently read line of output from steamcmd
  std::string line;
  // track info blob extraction status
  SteamCmdReadState readState(SteamCmdReadState::FIND_INFO_START);
  // now process output from steamcmd one line at a time
  // NOTE: Boost.Process docs say not to read from steam unless app is
  //  running, but this truncates the output so F that - we do what works!
  while (/*sc.running() &&*/ std::getline(fromChild, line))
  {
    bool appendLine(false);
    if (line.empty()) continue;
    switch (readState)
    {
      case SteamCmdReadState::FIND_INFO_START:
      {
        // looking for "258550" (in double quotes, at start of line)
        if (!line.empty() && line[0] == '\"' && line.find("\"258550\"") == 0)
        {
          appendLine = true;
          readState = SteamCmdReadState::FIND_INFO_END;
        }
        break;
      }
      case SteamCmdReadState::FIND_INFO_END:
      {
        // append all lines in this mode
        appendLine = true;
        // looking for "}" as the first character to signal info blob end
        if (!line.empty() && line[0] == '}')
        {
          readState = SteamCmdReadState::COMPLETE;
        }
        break;
      }
      case SteamCmdReadState::COMPLETE:
      {
        appendLine = false;
        break;
      }
    }
    if (appendLine)
    {
      steamInfo.append(line).append("\n");
    }
  }
  sc.wait(errorCode);
  // report any process errors
  if (errorCode)
  {
    std::cout << "WARNING: Error running server update command: " << errorCode.message()<< "\n";
  }
  if (const int exitCode(sc.exit_code()); exitCode)
  {
    std::cout << "WARNING: SteamCMD returned nonzero exit code: " << exitCode << "\n";
  }
  // audit output
  if (readState != SteamCmdReadState::COMPLETE)
  {
    std::cout << "ERROR: SteamCMD output did not include a valid app info tree\n";
    return retVal;
  }
  // process output
  std::stringstream ss(steamInfo);
  try
  {
    boost::property_tree::ptree tree;
    boost::property_tree::read_info(ss, tree);
    retVal = tree.get<std::string>(
      std::string{"258550.depots.branches."} +
      (branch.empty() ? "public" : branch.data()) +
      ".buildid"
  );
  }
  catch (const std::exception& ex)
  {
    std::cout << "ERROR: Exception parsing SteamCMD output: " << ex.what()<< "\n";
    retVal.clear();
  }
  catch (...)
  {
    std::cout << "ERROR: Unknown exception parsing SteamCMD output\n";
    retVal.clear();
  }
  return retVal;
}

std::string Updater::GetLatestFrameworkURL() const
{
  if (!downloaderSptr_)
  {
    std::cout << "ERROR: Downloader handle is null\n";
    return {};
  }
  const std::string_view frameworkURL{GetFrameworkURL(frameworkUpdateCheck_)};
  const std::string& frameworkInfo{downloaderSptr_->GetUrlToString(frameworkURL)};
  const std::string_view frameworkAsset{GetFrameworkAsset(frameworkUpdateCheck_)};
  const std::string_view frameworkTitle{ToString(frameworkUpdateCheck_, ToStringCase::TITLE)};

  try
  {
    const auto& j(nlohmann::json::parse(frameworkInfo));
    // "assets" node is a list of release files
    // there's usually a Linux release that we want to ignore for now
    // TODO: choose the linux version when appropriate if RLS ever gets
    //  ported to that OS
    for (const auto& asset : j["assets"])
    {
      if (asset["name"] == frameworkAsset)
      {
        return asset["browser_download_url"];
      }
    }
  }
  catch (const nlohmann::json::exception& e)
  {
    std::cout << "ERROR: Exception extracting download URL from " << frameworkTitle << " releases JSON: " << e.what() << "\n";
    std::cout << "Input string: '" << frameworkInfo << "'\n";
    return {};
  }

  std::cout << "ERROR: Failed to extract download URL from " << frameworkTitle << " releases JSON\n";
  return {};
}

std::string Updater::GetLatestFrameworkVersion() const
{
  if (!downloaderSptr_)
  {
    std::cout << "ERROR: Downloader handle is null\n";
    return {};
  }
  const std::string_view frameworkURL{GetFrameworkURL(frameworkUpdateCheck_)};
  const std::string& frameworkInfo{downloaderSptr_->GetUrlToString(frameworkURL)};
  const std::string_view frameworkTitle{ToString(frameworkUpdateCheck_, ToStringCase::TITLE)};

  try
  {
    const auto& j(nlohmann::json::parse(frameworkInfo));
    switch (frameworkUpdateCheck_)
    {
      case PluginFramework::NONE: break;
      // need to process the Carbon release name
      case PluginFramework::CARBON:
      {
        // Carbon release versions look like "Production Build — v1.2024.1033.4309"
        // ...but we only want the part after the 'v', so strip the rest off
        static const std::string_view CARBON_PREFIX{"Production Build — v"};
        const std::string& carbonVersion{j["name"]};
        // bail if the version string doesn't start with the prefix string, as it
        //  means release naming has changed
        if (carbonVersion.find(CARBON_PREFIX) != 0)
        {
          std::cout << "ERROR: Carbon release prefix not found in version string: " << carbonVersion << "\n";
          return {};
        }
        // return everything *after* the prefix string
        return carbonVersion.substr(CARBON_PREFIX.length());
      }
      // just return the raw release name for Oxide
      case PluginFramework::OXIDE: return j["name"];
    }
    std::cout << "ERROR: Unsupported plugin framework\n";
    return {};
  }
  catch(const nlohmann::json::exception& e)
  {
    std::cout << "ERROR: JSON exception while extracting version name from " << frameworkTitle << " release data: " << e.what() << "\n";
    std::cout << "Input data:\n" << frameworkInfo << "\n";
    return {};
  }
  catch (const std::exception& e)
  {
    std::cout << "ERROR: General exception while extracting version name from " << frameworkTitle << " release data: " << e.what() << "\n";
    std::cout << "Input data:\n" << frameworkInfo << "\n";
    return {};
  }
  catch (...)
  {
    std::cout << "ERROR: Unknown exception when attempting to get latest plugin framework release version\n";
    std::cout << "Input data:\n" << frameworkInfo << "\n";
    return {};
  }

  std::cout << "ERROR: Unknown failure when attempting to get latest plugin framework release version\n";
  std::cout << "Input data:\n" << frameworkInfo << "\n";
  return {};
}
}
