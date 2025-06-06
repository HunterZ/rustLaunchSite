#include "Updater.h"

#include "Config.h"
#include "Downloader.h"

#if _MSC_VER
  // make Boost happy when building with MSVC
  #include <SDKDDKVer.h>
#endif

#include <boost/process/v1/args.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/error.hpp>
#include <boost/process/v1/exe.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/pipe.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/process/v1/system.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>
// these must be included after boost, because it #include's Windows.h
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#if _MSC_VER
  #include <io.h> // _access_s()
#else
  #include <unistd.h> // access()
#endif

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
  // std::filesystem is garbage here, so we need to use access() or similar
#if _MSC_VER
  return (0 == _access_s(path.string().c_str(), 2));
#else
  return (0 == access(path.string().c_str(), W_OK));
#endif
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
  const rustLaunchSite::Config::ModFrameworkType framework,
  const ToStringCase stringCase
)
{
  std::size_t index{0};
  switch (framework)
  {
    case rustLaunchSite::Config::ModFrameworkType::NONE:   index = 0; break;
    case rustLaunchSite::Config::ModFrameworkType::CARBON: index = 1; break;
    case rustLaunchSite::Config::ModFrameworkType::OXIDE:  index = 2; break;
  }
  switch (stringCase)
  {
    case ToStringCase::LOWER: return FRAMEWORK_STRING_LOWER.at(index);
    case ToStringCase::TITLE: return FRAMEWORK_STRING_TITLE.at(index);
    case ToStringCase::UPPER: return FRAMEWORK_STRING_UPPER.at(index);
  }
  return {};
}

std::filesystem::path GetFrameworkDllPath(
  const std::filesystem::path& serverInstallPath,
  const rustLaunchSite::Config::ModFrameworkType framework
)
{
  switch (framework)
  {
    case rustLaunchSite::Config::ModFrameworkType::NONE:
      break;
    case rustLaunchSite::Config::ModFrameworkType::CARBON:
      return serverInstallPath / "carbon/managed/Carbon.dll";
    case rustLaunchSite::Config::ModFrameworkType::OXIDE:
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

std::shared_ptr<struct archive> GetReadArchive()
{
  return {archive_read_new(), [](struct archive* h){archive_read_free(h);}};
}

std::shared_ptr<struct archive> GetWriteDiskArchive()
{
  return {
    archive_write_disk_new(), [](struct archive* h){archive_write_free(h);}};
}

int CopyArchiveData(
  std::shared_ptr<struct archive> ar, std::shared_ptr<struct archive> aw)
{
  while (true)
  {
    const void* buff{};
    std::size_t size{};
    std::int64_t offset{};
    const auto readResult{
      archive_read_data_block(ar.get(), &buff, &size, &offset)};
    if (ARCHIVE_EOF == readResult) return ARCHIVE_OK;
    if (readResult < ARCHIVE_OK) return readResult;
    const auto writeResult{
      archive_write_data_block(aw.get(), buff, size, offset)};
    if (writeResult < ARCHIVE_OK)
    {
      std::cout << "Failed to write extracted file data: " << archive_error_string(aw.get()) << "\n";
      return static_cast<int>(writeResult);
    }
  }
}

bool CheckArchiveResult(
  const int result,
  std::string_view prefix,
  std::shared_ptr<struct archive> arch)
{
  if (result == ARCHIVE_EOF) return false;
  if (result >= ARCHIVE_OK) return true;
  const bool isError{result < ARCHIVE_WARN};
  std::cout << (isError ? "ERROR" : "WARNING");
  if (!prefix.empty()) std::cout << ": " << prefix;
  if (arch) std::cout << ": " << archive_error_string(arch.get());
  std::cout << "\n";
  return !isError;
}

bool IsExecutableFile(const std::filesystem::path& filePath)
{
  const auto& ext{filePath.extension()};
  return
       ".a"   == ext
    || ".dll" == ext
    || ".DLL" == ext
    || ".sh"  == ext
    || ".so"  == ext
  ;
}

void FixPermissions(const std::filesystem::path& filePath)
{
  if (!IsExecutableFile(filePath)) return;
  std::error_code ec{};
  std::filesystem::permissions(filePath,
    std::filesystem::perms::owner_exec
    | std::filesystem::perms::group_exec
    | std::filesystem::perms::others_exec,
    std::filesystem::perm_options::add,
    ec
  );
  if (ec)
  {
    std::cout << "ERROR: Issue while setting execute permissions on file " << filePath << ": " << ec.message() << "\n";
    return;
  }
  std::cout << "Set execute permissions on file " << filePath << "\n";
}

// extract Carbon/Oxide release archive into server installation directory
void ExtractArchiveData(
  const std::vector<char>& archData,
  std::string_view url,
  std::string_view frameworkTitle,
  [[maybe_unused]] const std::filesystem::path& serverInstallPath)
{
  if (archData.size() < 2)
  {
    std::cout << "ERROR: Cannot update " << frameworkTitle << " because valid data was not downloaded from URL " << url << "\n";
    return;
  }
  auto arch{GetReadArchive()};
  // determine file type
  if (0x1F == archData.at(0) &&
      0x8B == static_cast<unsigned char>(archData.at(1)))
  {
    // .tar.gz
    archive_read_support_filter_gzip(arch.get());
    archive_read_support_format_tar(arch.get());
  }
  else if ('P' == archData.at(0) && 'K' == archData.at(1))
  {
    // .zip
    archive_read_support_format_zip(arch.get());
  }
  else
  {
    // unknown
    std::cout << "ERROR: Failed to determine " << frameworkTitle << " archive format for data of length=" << archData.size() << " downloaded from URL " << url << "\n";
    return;
  }
  // bind arch to data blob
  auto result{
    archive_read_open_memory(arch.get(), archData.data(), archData.size())};
  if (ARCHIVE_OK != result)
  {
    std::cout << "ERROR: Failed to open " << frameworkTitle << " archive data of length=" << archData.size() << " downloaded from URL " << url << "\n";
    return;
  }
  // allocate a handle for writing extracted data to disk
  auto outFile{GetWriteDiskArchive()};
  archive_write_disk_set_options(
    outFile.get(),
    ARCHIVE_EXTRACT_ACL    |
    ARCHIVE_EXTRACT_FFLAGS |
    ARCHIVE_EXTRACT_PERM   |
    ARCHIVE_EXTRACT_TIME
  );
  archive_write_disk_set_standard_lookup(outFile.get());
  // loop through archive contents and extract to disk
  while (true)
  {
    // extract entry metadata
    struct archive_entry* entry{};
    if (!CheckArchiveResult(archive_read_next_header(arch.get(), &entry),
        "Issue while reading archive entry", arch))
    {
      break;
    }
    // make the output file relative to server install path
    const auto outFilePath{serverInstallPath / archive_entry_pathname_w(entry)};
    archive_entry_copy_pathname_w(entry, outFilePath.generic_wstring().data());
    // create output file for entry
    result = archive_write_header(outFile.get(), entry);
    if (result < ARCHIVE_OK)
    {
      std::cout << "WARNING: Issue while creating output file " << outFilePath << ": " << archive_error_string(outFile.get()) << "\n";
    }
    else if
    (
      archive_entry_size(entry) > 0 && !CheckArchiveResult(
        // write output file data
        CopyArchiveData(arch, outFile),
        std::string{"Issue while writing output file "} +
          outFilePath.generic_string(), outFile)
    )
    {
      break;
    }
    // finalize output file
    if (!CheckArchiveResult(archive_write_finish_entry(outFile.get()),
        std::string{"Issue while finalizing output file "} +
          outFilePath.generic_string(), outFile))
    {
      break;
    }
    std::cout << "Extracted file " << outFilePath << "\n";
    FixPermissions(outFilePath);
  }
  archive_read_close(arch.get());
  archive_write_close(outFile.get());
}

std::string_view GetFrameworkURL(
  const rustLaunchSite::Config::ModFrameworkType framework
)
{
  std::size_t index{0};
  switch (framework)
  {
    case rustLaunchSite::Config::ModFrameworkType::NONE:   index = 0; break;
    case rustLaunchSite::Config::ModFrameworkType::CARBON: index = 1; break;
    case rustLaunchSite::Config::ModFrameworkType::OXIDE:  index = 2; break;
  }
  return FRAMEWORK_URL.at(index);
}

const std::vector<std::string_view> FRAMEWORK_ASSET
{
  // NONE
  "",
  // CARBON
#if _MSC_VER || defined(__MINGW32__)
  "Carbon.Windows.Release.zip",
#else
  "Carbon.Linux.Release.tar.gz",
#endif
  // OXIDE
#if _MSC_VER || defined(__MINGW32__)
  "Oxide.Rust.zip"
#else
  "Oxide.Rust-linux.zip"
#endif
};

std::string_view GetFrameworkAsset(
  const rustLaunchSite::Config::ModFrameworkType framework
)
{
  std::size_t index{0};
  switch (framework)
  {
    case rustLaunchSite::Config::ModFrameworkType::NONE:   index = 0; break;
    case rustLaunchSite::Config::ModFrameworkType::CARBON: index = 1; break;
    case rustLaunchSite::Config::ModFrameworkType::OXIDE:  index = 2; break;
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
  : cfgSptr_(cfgSptr)
  , downloaderSptr_(downloaderSptr)
  , serverInstallPath_(cfgSptr->GetInstallPath())
  , appManifestPath_(cfgSptr->GetInstallPath() / "steamapps/appmanifest_258550.acf")
  , steamCmdPath_(cfgSptr->GetSteamcmdPath())
  , frameworkDllPath_(GetFrameworkDllPath(
      serverInstallPath_, cfgSptr->GetUpdateModFrameworkType()))
{
  // install path must be either a directory, or a symbolic link to one
  if (!IsDirectory(serverInstallPath_))
  {
    throw std::invalid_argument(std::string("ERROR: Server install path does not exist: ") + serverInstallPath_.string());
  }
#if _MSC_VER || defined(__MINGW32__)
  if (!std::filesystem::exists(serverInstallPath_ / "RustDedicated.exe"))
#else
  if (!std::filesystem::exists(serverInstallPath_ / "RustDedicated"))
#endif
  {
    throw std::invalid_argument(std::string("ERROR: Rust dedicated server not found in configured install path: ") + serverInstallPath_.string());
  }

/*
  if (std::filesystem::exists(appManifestPath_))
  {
    // extract SteamCMD utility path from manifest
    steamCmdPath_ = GetAppManifestValue(appManifestPath_, "AppState.LauncherPath");
    if (steamCmdPath_.empty())
    {
      std::cout << "WARNING: Failed to locate SteamCMD path from manifest file " << appManifestPath_ << "; automatic Steam updates disabled\n";
    }
    else if (!std::filesystem::exists(steamCmdPath_))
    {
      std::cout << "WARNING: Failed to locate SteamCMD at manifest file specified path " << steamCmdPath_ << "; automatic Steam updates disabled\n";
      steamCmdPath_.clear();
    }
  }
  else
*/
  if (!std::filesystem::exists(appManifestPath_))
  {
    std::cout << "WARNING: Steam app manifest file " << appManifestPath_ << " does not exist; automatic Steam updates disabled\n";
    appManifestPath_.clear();
  }

  if (!std::filesystem::exists(steamCmdPath_))
  {
    std::cout << "WARNING: Failed to locate SteamCMD at config file specified path " << steamCmdPath_ << "; automatic Steam updates disabled\n";
    steamCmdPath_.clear();
  }

  if (
    !frameworkDllPath_.empty() && !std::filesystem::exists(frameworkDllPath_))
  {
    std::cout << "WARNING: Modding framework DLL '" << frameworkDllPath_ << "' not found; automatic " << ToString(cfgSptr->GetUpdateModFrameworkType(), ToStringCase::TITLE) << " updates disabled\n";
    frameworkDllPath_.clear();
  }

  // std::cout << "Updater initialized. Server updates " << (serverUpdateCheck_ ? "enabled" : "disabled") << ". Oxide updates " << (oxideUpdateCheck_ ? "enabled" : "disabled")<< "\n";
}

Updater::~Updater() = default;

bool Updater::CheckFramework() const
{
  if (cfgSptr_->GetUpdateModFrameworkType() == Config::ModFrameworkType::NONE)
  {
    return false;
  }
  const auto& frameworkTitle{
    ToString(cfgSptr_->GetUpdateModFrameworkType(), ToStringCase::TITLE)};
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
  if (cfgSptr_->GetUpdateModFrameworkType() == Config::ModFrameworkType::NONE)
  {
    return;
  }
  const auto& frameworkTitle{
    ToString(cfgSptr_->GetUpdateModFrameworkType(), ToStringCase::TITLE)};
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
  if (serverInstallPath_.empty())
  {
    std::cout << "ERROR: Cannot update " << frameworkTitle << " because server install path is invalid\n";
    return;
  }

  // get URL of latest Carbon/Oxide release archive
  const auto& url{GetLatestFrameworkURL()};
  if (url.empty())
  {
    std::cout << "WARNING: Cannot update " << frameworkTitle << " because download URL was not found\n";
    return;
  }
  // download archive to RAM
  const std::vector<char>& archData{downloaderSptr_->GetUrlToVector(url)};
  // extract archive
  ExtractArchiveData(archData, url, frameworkTitle, serverInstallPath_);
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
  const int exitCode(boost::process::v1::system(
    boost::process::v1::exe(steamCmdPath_.string()),
    boost::process::v1::args(args),
    boost::process::v1::error(errorCode)
  ));
  if (errorCode)
  {
    std::cout << "WARNING: Error running server update command: " << errorCode.message() << "\n";
    return;
  }
  if (exitCode)
  {
    std::cout << "WARNING: SteamCMD returned nonzero exit code: " << exitCode << "\n";
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
#if _MSC_VER || defined(__MINGW32__)
  // run powershell and grab all output into inStream
  boost::process::v1::ipstream inStream;
  // for some reason boost requires explicitly requesting a PATH search unless
  //  we want to pass the entire command as a single string
  const auto& psPath{boost::process::v1::search_path("powershell.exe")};
  if (psPath.empty())
  {
    std::cout << "ERROR: Failed to find powershell\n";
    return retVal;
  }
  std::error_code errorCode;
  const int exitCode(boost::process::v1::system(
    boost::process::v1::exe(psPath),
    boost::process::v1::args({
      "-Command",
      std::string("(Get-Item '") + frameworkDllPath_.string() +
        "').VersionInfo.ProductVersion"
    }),
    boost::process::v1::std_out > inStream,
    boost::process::v1::error(errorCode)
  ));
  if (errorCode)
  {
    std::cout << "ERROR: Error running " << ToString(cfgSptr_->GetUpdateModFrameworkType(), ToStringCase::TITLE) << " version check command: " << errorCode.message() << "\n";
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
#else
  // run monodis and grab all output into inStream
  boost::process::v1::ipstream inStream;
  // for some reason boost requires explicitly requesting a PATH search unless
  //  we want to pass the entire command as a single string
  const auto& psPath{boost::process::v1::search_path("monodis")};
  if (psPath.empty())
  {
    std::cout << "ERROR: Failed to find monodis; you may need to install mono-utils or similar\n";
    return retVal;
  }
  std::error_code errorCode;
  const int exitCode(boost::process::v1::system(
    boost::process::v1::exe(psPath),
    boost::process::v1::args({
      "--assembly",
      frameworkDllPath_.string()
    }),
    boost::process::v1::std_out > inStream,
    boost::process::v1::error(errorCode)
  ));
  if (errorCode)
  {
    std::cout << "ERROR: Error running " << ToString(cfgSptr_->GetUpdateModFrameworkType(), ToStringCase::TITLE) << " version check command: " << errorCode.message() << "\n";
    return retVal;
  }
  if (exitCode)
  {
    std::cout << "ERROR: " << psPath << " returned nonzero exit code: " << exitCode << "\n";
    return retVal;
  }
  // find the line that begins with "Version:"
  std::string line;
  while (std::getline(inStream, line))
  {
    if (0 == line.find("Version:"))
    {
      // grab everything after "Version:" and after any spaces
      retVal = line.substr(line.find_first_not_of(' ', 8));
      break;
    }
  }
  // chop off anything from the third version separator onwards (if present)
  std::size_t sepCount{0};
  std::size_t sepPos{};
  for (std::size_t i{0}; i < retVal.length(); ++i)
  {
    if ('.' == retVal.at(i))
    {
      ++sepCount;
      if (3 == sepCount)
      {
        sepPos = i;
        break;
      }
    }
  }
  if (sepCount > 2 && sepPos > 0)
  {
    retVal.resize(sepPos);
  }
  // return version number or empty string
  return retVal;
#endif
}

std::string Updater::GetInstalledServerBranch() const
{
  return GetAppManifestValue(
    appManifestPath_, "AppState.UserConfig.BetaKey", false);
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
  boost::process::v1::ipstream fromChild; // from child to RLS
  std::error_code errorCode;
  boost::process::v1::child sc(
    boost::process::v1::exe(steamCmdPath_.string()),
    boost::process::v1::args({"+runscript", scriptFilePath.string()}),
    boost::process::v1::std_out > fromChild,
    boost::process::v1::error(errorCode)
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
  if (const auto exitCode{sc.exit_code()}; exitCode)
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
  const auto modFrameworkType{cfgSptr_->GetUpdateModFrameworkType()};
  const auto frameworkURL{GetFrameworkURL(modFrameworkType)};
  const auto& frameworkInfo{downloaderSptr_->GetUrlToString(frameworkURL)};
  const auto frameworkAsset{GetFrameworkAsset(modFrameworkType)};
  const auto frameworkTitle{ToString(modFrameworkType, ToStringCase::TITLE)};

  try
  {
    const auto& j(nlohmann::json::parse(frameworkInfo));
    // "assets" node is a list of release files, so find the one of interest
    for (const auto& asset : j["assets"])
    {
      if (asset["name"].get<std::string>() == frameworkAsset)
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
  const auto modFrameworkType{cfgSptr_->GetUpdateModFrameworkType()};
  const auto frameworkURL{GetFrameworkURL(modFrameworkType)};
  const auto& frameworkInfo{downloaderSptr_->GetUrlToString(frameworkURL)};
  const auto frameworkTitle{ToString(modFrameworkType, ToStringCase::TITLE)};

  try
  {
    const auto& j(nlohmann::json::parse(frameworkInfo));
    switch (modFrameworkType)
    {
      case Config::ModFrameworkType::NONE: break;
      // need to process the Carbon release name
      case Config::ModFrameworkType::CARBON:
      {
        // Carbon release versions look like "Production Build — v1.2024.1033.4309"
        // ...but we only want the part after the 'v', so strip the rest off
        static const std::string_view CARBON_PREFIX{"Production Build — v"};
        const std::string& carbonVersion(j["name"]);
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
      case Config::ModFrameworkType::OXIDE: return j["name"];
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
}
}
