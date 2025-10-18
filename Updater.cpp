#include "Updater.h"

#include "Config.h"
#include "Downloader.h"
#include "Logger.h"

#if _MSC_VER
  // make Boost happy when building with MSVC
  #include <sdkddkver.h>
#endif

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/process/environment.hpp>
#include <boost/process/stdio.hpp>
#include <boost/process/process.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>
// these must be included after boost, because it #include's Windows.h
#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace
{
bool IsDirectory(const std::filesystem::path& path)
{
  const auto& targetPath(
    std::filesystem::is_symlink(path) ?
      std::filesystem::read_symlink(path) : path
  );
  return std::filesystem::is_directory(targetPath);
}

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
  using enum rustLaunchSite::Config::ModFrameworkType;
  switch (framework)
  {
    case NONE:   index = 0; break;
    case CARBON: index = 1; break;
    case OXIDE:  index = 2; break;
  }
  using enum ToStringCase;
  switch (stringCase)
  {
    case LOWER: return FRAMEWORK_STRING_LOWER.at(index);
    case TITLE: return FRAMEWORK_STRING_TITLE.at(index);
    case UPPER: return FRAMEWORK_STRING_UPPER.at(index);
  }
  return {};
}

std::filesystem::path GetFrameworkDllPath(
  const std::filesystem::path& serverInstallPath,
  const rustLaunchSite::Config::ModFrameworkType framework
)
{
  using enum rustLaunchSite::Config::ModFrameworkType;
  switch (framework)
  {
    case NONE:
      break;
    case CARBON:
      return serverInstallPath / "carbon/managed/Carbon.dll";
    case OXIDE:
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
  return { // NOSONAR
    archive_read_new(), [](struct archive* h){archive_read_free(h);}};
}

std::shared_ptr<struct archive> GetWriteDiskArchive()
{
  return { // NOSONAR
    archive_write_disk_new(), [](struct archive* h){archive_write_free(h);}};
}

int CopyArchiveData(
  rustLaunchSite::Logger& logger
, std::shared_ptr<struct archive> ar
, std::shared_ptr<struct archive> aw)
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
      LOGINF(logger, "Failed to write extracted file data: " << archive_error_string(aw.get()));
      return static_cast<int>(writeResult);
    }
  }
}

bool CheckArchiveResult(
  rustLaunchSite::Logger& logger
, const int result
, std::string_view prefix
, std::shared_ptr<struct archive> arch)
{
  if (result == ARCHIVE_EOF) return false;
  if (result >= ARCHIVE_OK) return true;
  const bool isError{result < ARCHIVE_WARN};
  LOG(
    logger
    , isError ? rustLaunchSite::LogLevel::ERR : rustLaunchSite::LogLevel::WRN
    , prefix << ": " << arch ? archive_error_string(arch.get()) : "");
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

void FixPermissions(
  rustLaunchSite::Logger& logger, const std::filesystem::path& filePath)
{
  using enum std::filesystem::perms;
  if (!IsExecutableFile(filePath)) return;
  std::error_code ec{};
  std::filesystem::permissions(
    filePath
  , owner_exec | group_exec | others_exec
  , std::filesystem::perm_options::add
  , ec
  );
  if (ec)
  {
    LOGWRN(logger, "Issue while setting execute permissions on file " << filePath << ": " << ec.message());
    return;
  }
  LOGINF(logger, "Set execute permissions on file " << filePath);
}

// extract Carbon/Oxide release archive into server installation directory
void ExtractArchiveData(
  rustLaunchSite::Logger& logger
, const std::vector<char>& archData
, std::string_view url
, std::string_view frameworkTitle
, [[maybe_unused]] const std::filesystem::path& serverInstallPath)
{
  if (archData.size() < 2)
  {
    LOGWRN(logger, "Cannot update " << frameworkTitle << " because valid data was not downloaded from URL " << url);
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
    LOGWRN(logger, "Failed to determine " << frameworkTitle << " archive format for data of length=" << archData.size() << " downloaded from URL " << url);
    return;
  }
  // bind arch to data blob
  auto result{
    archive_read_open_memory(arch.get(), archData.data(), archData.size())};
  if (ARCHIVE_OK != result)
  {
    LOGWRN(logger, "Failed to open " << frameworkTitle << " archive data of length=" << archData.size() << " downloaded from URL " << url);
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
    if (!CheckArchiveResult(
      logger
    , archive_read_next_header(arch.get(), &entry)
    , "Issue while reading archive entry"
    , arch))
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
      LOGWRN(logger, "Issue while creating output file " << outFilePath << ": " << archive_error_string(outFile.get()));
    }
    else if (archive_entry_size(entry) > 0 && !CheckArchiveResult(
      logger
      // write output file data
    , CopyArchiveData(logger, arch, outFile)
    , std::string{"Issue while writing output file "} +
        outFilePath.generic_string()
    , outFile)
    )
    {
      break;
    }
    // finalize output file
    if (!CheckArchiveResult(
      logger
      , archive_write_finish_entry(outFile.get())
      , std::string{"Issue while finalizing output file "} +
          outFilePath.generic_string()
      , outFile))
    {
      break;
    }
    LOGINF(logger, "Extracted file " << outFilePath);
    FixPermissions(logger, outFilePath);
  }
  archive_read_close(arch.get());
  archive_write_close(outFile.get());
}

std::string_view GetFrameworkURL(
  const rustLaunchSite::Config::ModFrameworkType framework
)
{
  std::size_t index{0};
  using enum rustLaunchSite::Config::ModFrameworkType;
  switch (framework)
  {
    case NONE:   index = 0; break;
    case CARBON: index = 1; break;
    case OXIDE:  index = 2; break;
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
  using enum rustLaunchSite::Config::ModFrameworkType;
  switch (framework)
  {
    case NONE:   index = 0; break;
    case CARBON: index = 1; break;
    case OXIDE:  index = 2; break;
  }
  return FRAMEWORK_ASSET.at(index);
}

template<typename P>
std::string RunExecutable(
  rustLaunchSite::Logger& logger,
  const P& exe,
  const std::vector<std::string>& args)
{
  if (exe.empty()) return {};

  boost::asio::io_context ioContext;
  boost::asio::readable_pipe readPipe{ioContext};
  boost::process::process proc(
    ioContext, exe, args, boost::process::process_stdio{{}, readPipe, readPipe}
  );
  std::string output;
  boost::system::error_code errorCode;
  boost::asio::read(readPipe, boost::asio::dynamic_buffer(output), errorCode);
  proc.wait();

  // LOGINF(logger, exe << " output:\n" << output);

  if (errorCode && boost::asio::error::eof != errorCode)
  {
    LOGWRN(logger, "Got error code " << errorCode.value() << " / category " << errorCode.category().name() << " running " << exe << ": " << errorCode.message());
  }
  const auto exitCode(proc.exit_code());
  if (proc.exit_code())
  {
    LOGWRN(logger, "Got nonzero exit code " << exitCode << " running " << exe);
  }

  return output;
}

template<typename TreeReadFunc>
std::string GetAppManifestValueCommon(
  rustLaunchSite::Logger& logger
, TreeReadFunc trf
, std::string_view keyPath
, const bool warn = true)
{
  std::string retVal;

  try
  {
    boost::property_tree::ptree tree;
    trf(tree);
    retVal = tree.get<std::string>(keyPath.data());
  }
  catch (const boost::property_tree::ptree_bad_path& ex)
  {
    if (warn)
    {
      LOGWRN(logger, "Exception parsing server app manifest: " << ex.what());
    }
    retVal.clear();
  }
  catch (const std::exception& ex)
  {
    LOGWRN(logger, "Exception parsing server app manifest: " << ex.what());
    retVal.clear();
  }
  catch (...)
  {
    LOGWRN(logger, "Unknown exception parsing server app manifest");
    retVal.clear();
  }

  // LOGINF(logger_, "*** " << appManifestPath << " @ " << keyPath << " = " << retVal<< "");

  return retVal;
}

std::string GetAppManifestValue(
  rustLaunchSite::Logger& logger
, const std::filesystem::path& appManifestPath
, std::string_view keyPath
, const bool warn = true)
{
  return GetAppManifestValueCommon(
    logger,
    [&appManifestPath](boost::property_tree::ptree& tree)
    {
      boost::property_tree::read_info(appManifestPath.string(), tree);
    },
    keyPath,
    warn
  );
}

std::string GetAppManifestValue(
  rustLaunchSite::Logger& logger
, const std::string& appManifestData
, std::string_view keyPath
, const bool warn = true)
{
  return GetAppManifestValueCommon(
    logger,
    [&appManifestData](boost::property_tree::ptree& tree)
    {
      std::stringstream stream{appManifestData};
      boost::property_tree::read_info(stream, tree);
    },
    keyPath,
    warn
  );
}
}

namespace rustLaunchSite
{
Updater::Updater(
  Logger& logger
, std::shared_ptr<const Config> cfgSptr
, std::shared_ptr<Downloader> downloaderSptr
)
  : cfgSptr_{cfgSptr}
  , downloaderSptr_{downloaderSptr}
  , serverInstallPath_{cfgSptr->GetInstallPath()}
  , appManifestPath_{cfgSptr->GetInstallPath() / "steamapps/appmanifest_258550.acf"}
  , steamCmdPath_{cfgSptr->GetSteamcmdPath()}
  , frameworkDllPath_{GetFrameworkDllPath(
      serverInstallPath_, cfgSptr->GetUpdateModFrameworkType())}
  , logger_{logger}
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

  if (!std::filesystem::exists(appManifestPath_))
  {
    LOGWRN(logger_, "Steam app manifest file " << appManifestPath_ << " does not exist; automatic Steam updates disabled");
    appManifestPath_.clear();
  }

  if (!std::filesystem::exists(steamCmdPath_))
  {
    LOGWRN(logger_, "Failed to locate SteamCMD at config file specified path " << steamCmdPath_ << "; automatic Steam updates disabled");
    steamCmdPath_.clear();
  }

  if (
    !frameworkDllPath_.empty() && !std::filesystem::exists(frameworkDllPath_))
  {
    LOGWRN(logger_, "Modding framework DLL '" << frameworkDllPath_ << "' not found; automatic " << ToString(cfgSptr->GetUpdateModFrameworkType(), ToStringCase::TITLE) << " updates disabled");
    frameworkDllPath_.clear();
  }

  // LOGINF(logger_, "Updater initialized. Server updates " << (serverUpdateCheck_ ? "enabled" : "disabled") << ". Oxide updates " << (oxideUpdateCheck_ ? "enabled" : "disabled")<< "");
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
  LOGINF(logger_, "CheckFramework(): Installed " << frameworkTitle << " version: '" << currentVersion << "'");
  const auto& latestVersion(GetLatestFrameworkVersion());
  LOGINF(logger_, "CheckFramework(): Latest " << frameworkTitle << " version: '" << latestVersion << "'");
  return (
    !currentVersion.empty() && !latestVersion.empty() &&
    currentVersion != latestVersion
  );
}

bool Updater::CheckServer() const
{
  const std::string& currentServerVersion(GetInstalledServerBuild());
  LOGINF(logger_, "CheckServer(): Installed Server version: '" << currentServerVersion << "'");
  std::string latestServerVersion;
  for (std::size_t i{0}; i < 5 && latestServerVersion.empty(); ++i)
  {
    if (i)
    {
      LOGINF(logger_, "CheckServer(): Retrying latest Server version check...");
    }
    latestServerVersion = GetLatestServerBuild(GetInstalledServerBranch());
  }
  if (latestServerVersion.empty())
  {
    LOGWRN(logger_, "CheckServer(): Exhausted latest Server version check attempts");
  }
  else
  {
    LOGINF(logger_, "CheckServer(): Latest Server version: '" << latestServerVersion << "'");
  }
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
      LOGWRN(logger_, "Cannot update " << frameworkTitle << " because a previous installation was not detected");
    }
    return;
  }

  // abort if any required path is empty, meaning it failed validation
  if (serverInstallPath_.empty())
  {
    LOGWRN(logger_, "Cannot update " << frameworkTitle << " because server install path is invalid");
    return;
  }

  // get URL of latest Carbon/Oxide release archive
  const auto& url{GetLatestFrameworkURL()};
  if (url.empty())
  {
    LOGWRN(logger_, "Cannot update " << frameworkTitle << " because download URL was not found");
    return;
  }
  // download archive to RAM
  const std::vector<char>& archData{downloaderSptr_->GetUrlToVector(url)};
  // extract archive
  ExtractArchiveData(
    logger_, archData, url, frameworkTitle, serverInstallPath_);
}

void Updater::UpdateServer() const
{
  // abort if any required path is empty, meaning it failed validation
  if (serverInstallPath_.empty() || steamCmdPath_.empty())
  {
    LOGWRN(logger_, "Cannot update server because install and/or steamcmd path is invalid");
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

  RunExecutable(logger_, steamCmdPath_.string(), args);
}

std::string Updater::GetInstalledFrameworkVersion() const
{
  std::string retVal;
  if (frameworkDllPath_.empty()) { return retVal; }
#if _MSC_VER || defined(__MINGW32__)
  // TODO: make powershell path configurable?
  const auto& psPath(
    boost::process::environment::find_executable("powershell.exe"));
  if (psPath.empty())
  {
    LOGWRN(logger_, "Failed to find powershell; you may need to install mono-utils or similar");
    return retVal;
  }

  retVal = RunExecutable(
    logger_,
    psPath,
    {
      "-Command",
      "(Get-Item '" + frameworkDllPath_.string() +
        "').VersionInfo.ProductVersion"
    }
  );

  // for some reason this has a newline at the end, so strip that off
  while (retVal.back() == '\r' || retVal.back() == '\n') { retVal.pop_back(); }
  // strip off anything starting with `+` or `-` if present
  return retVal.substr(0, retVal.find_first_of("+-"));
#else
  // TODO: make monodis path configurable?
  const auto& monodisPath(
    boost::process::environment::find_executable("monodis"));
  if (monodisPath.empty())
  {
    LOGWRN(logger_, "Failed to find monodis; you may need to install mono-utils or similar");
    return retVal;
  }

  const auto& output(RunExecutable(
    logger_, monodisPath, {"--assembly", frameworkDllPath_.string()}));

  // attempt to extract version from output
  std::smatch match;
  if (std::regex_search(
    output, match, std::regex{R"(Version: *[0-9]+\.[0-9]+\.[0-9]+)"}))
  {
    const auto& matchStr(match.begin()->str());
    std::smatch match2;
    if (std::regex_search(
      matchStr, match2, std::regex{R"([0-9]+\.[0-9]+\.[0-9]+)"}))
    {
      retVal = match2.begin()->str();
    }
  }

  if (retVal.empty())
  {
    LOGWRN(logger_, "Failed to extract Carbon version from monodis output");
  }
  // return version number or empty string
  return retVal;
#endif
}

std::string Updater::GetInstalledServerBranch() const
{
  const auto& branch(GetAppManifestValue(
    logger_
  , appManifestPath_
  , "AppState.UserConfig.BetaKey"
  , false));
  // clean installs may not have a branch listed in appmanifest; assume public
  return branch.empty() ? "public" : branch;
}

std::string Updater::GetInstalledServerBuild() const
{
  return GetAppManifestValue(logger_, appManifestPath_, "AppState.buildid");
}

std::string Updater::GetLatestServerBuild(const std::string_view branch) const
{
  // abort if any required path is empty, meaning it failed validation
  if (serverInstallPath_.empty() || steamCmdPath_.empty())
  {
    LOGWRN(logger_, "Cannot check for server updates because install and/or steamcmd path is invalid");
    return {};
  }

  const auto& output(RunExecutable(
    logger_,
    steamCmdPath_.string(),
    {
      "+force_install_dir", serverInstallPath_.string(),
      "+login", "anonymous",
      "+app_info_update", "1",
      "+app_info_print", "258550",
      "+quit"
    }
  ));

  const auto startPos(output.find("\"258550\""));
  if (std::string::npos != startPos)
  {
    //258550.depots.branches.<branch>.buildid
    static const std::string PATH_PREFIX("258550.depots.branches.");
    constexpr std::string_view DEFAULT_BRANCH("public");
    static const std::string PATH_SUFFIX(".buildid");
    return GetAppManifestValue(logger_, output.substr(startPos),
      PATH_PREFIX +
      std::string(branch.empty() ? DEFAULT_BRANCH : branch) +
      PATH_SUFFIX);
  }

  LOGWRN(logger_, "Failed to extract latest server version from SteamCMD output");
  return {};
}

std::string Updater::GetLatestFrameworkURL() const
{
  if (!downloaderSptr_)
  {
    LOGWRN(logger_, "Downloader handle is null");
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
    LOGWRN(logger_, "Exception extracting download URL from " << frameworkTitle << " releases JSON: " << e.what());
    LOGWRN(logger_, "\t...Input string: '" << frameworkInfo << "'");
    return {};
  }

  LOGWRN(logger_, "Failed to extract download URL from " << frameworkTitle << " releases JSON");
  return {};
}

std::string Updater::GetLatestFrameworkVersion() const
{
  if (!downloaderSptr_)
  {
    LOGWRN(logger_, "Downloader handle is null");
    return {};
  }
  const auto modFrameworkType{cfgSptr_->GetUpdateModFrameworkType()};
  const auto frameworkURL{GetFrameworkURL(modFrameworkType)};
  const auto& frameworkInfo{downloaderSptr_->GetUrlToString(frameworkURL)};
  const auto frameworkTitle{ToString(modFrameworkType, ToStringCase::TITLE)};

  try
  {
    const auto& j(nlohmann::json::parse(frameworkInfo));
    if (!j.contains("name"))
    {
      LOGWRN(logger_, "Data received from frameworkURL=" << frameworkURL << " missing JSON 'name': " << frameworkInfo);
      return {};
    }
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
          LOGWRN(logger_, "Carbon release prefix not found in version string: " << carbonVersion);
          return {};
        }
        // return everything *after* the prefix string
        return carbonVersion.substr(CARBON_PREFIX.length());
      }
      // just return the raw release name for Oxide
      case Config::ModFrameworkType::OXIDE: return j["name"];
    }
    LOGWRN(logger_, "Unsupported plugin framework");
    return {};
  }
  catch(const nlohmann::json::exception& e)
  {
    LOGWRN(logger_, "JSON exception while extracting version name from " << frameworkTitle << " release data: " << e.what() << "\n\t...Input data: " << frameworkInfo);
    return {};
  }
  catch (const std::exception& e)
  {
    LOGWRN(logger_, "General exception while extracting version name from " << frameworkTitle << " release data: " << e.what() << "\n\t...Input data: " << frameworkInfo);
    return {};
  }
  catch (...)
  {
    LOGWRN(logger_, "Unknown exception while extracting version name from " << frameworkTitle << " release data\n\t...Input data: " << frameworkInfo);
    return {};
  }
}
}
