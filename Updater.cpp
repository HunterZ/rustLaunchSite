#include "Updater.h"

#include "Config.h"
#include "Downloader.h"

#include <boost/process.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kubazip/zip/zip.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
  inline bool IsDirectory(const std::string& path)
  {
    const auto& targetPath(
      std::filesystem::is_symlink(path) ?
        std::filesystem::read_symlink(path) : path
    );
    return std::filesystem::is_directory(targetPath);
  }

  inline bool IsWritable(const std::string& path)
  {
    // this sucks and may not work with MSVC, but std::filesystem is garbage in
    //  this area
    // (for MSVC, may need to include io.h and use _access() instead?)
    return (0 == access(path.c_str(), W_OK));
  }

  typedef std::pair<ssize_t, ssize_t> ZipStatus;
  int ZipExtractCallback(const char* filenamePtr, void* argPtr)
  {
    ZipStatus dummyStatus;
    ZipStatus* statusPtr(reinterpret_cast<ZipStatus*>(argPtr));
    if (!statusPtr) { statusPtr = &dummyStatus; }
    ZipStatus& status(*statusPtr);
    std::cout << "Extracted file " << ++status.first << "/" << status.second << ": " << filenamePtr << std::endl;
    return 0;
  }

  enum class SteamCmdReadState
  {
    FIND_INFO_START,
    FIND_INFO_END,
    COMPLETE
  };
}

namespace rustLaunchSite
{
  Updater::Updater(const Config& cfg)
    : downloaderUptr_(std::make_unique<Downloader>())
    , serverInstallPath_(cfg.GetInstallPath())
    , downloadPath_(cfg.GetPathsDownload())
    , serverUpdateCheck_(cfg.GetUpdateServer())
    , oxideUpdateCheck_(cfg.GetUpdateOxide())
  {
    if (!downloaderUptr_)
    {
      throw std::runtime_error("ERROR: Failed to allocate Downloader facility");
    }
    // install path must be either a directory, or a symbolic link to one
    if (!IsDirectory(serverInstallPath_))
    {
      throw std::invalid_argument(std::string("ERROR: Server install path does not exist: ") + serverInstallPath_);
    }
    if (!std::filesystem::exists(serverInstallPath_ + "RustDedicated.exe"))
    {
      throw std::invalid_argument(std::string("ERROR: Rust dedicated server not found in configured install path: ") + serverInstallPath_);
    }

    if (serverUpdateCheck_)
    {
      // derive the Steam app manifest path from the configured install location
      appManifestPath_ = serverInstallPath_ + "\\steamapps\\appmanifest_258550.acf";
      if (std::filesystem::exists(appManifestPath_))
      {
        // extract SteamCMD utility path from manifest
        steamCmdPath_ = GetAppManifestValue("LauncherPath");
        if (steamCmdPath_.empty())
        {
          std::cout << "WARNING: Failed to locate SteamCMD path from manifest file " << appManifestPath_ << "; automatic Steam updates disabled" << std::endl;
          serverUpdateCheck_ = false;
        }
        else if (!std::filesystem::exists(steamCmdPath_))
        {
          std::cout << "WARNING: Failed to locate SteamCMD at manifest file specified path " << steamCmdPath_ << "; automatic Steam updates disabled" << std::endl;
          steamCmdPath_.clear();
          serverUpdateCheck_ = false;
        }
      }
      else
      {
        std::cout << "WARNING: Steam app manifest file " << appManifestPath_ << " does not exist; automatic Steam updates disabled" << std::endl;
        appManifestPath_.clear();
        serverUpdateCheck_ = false;
      }
    }

    if (oxideUpdateCheck_)
    {
      oxideDllPath_ = serverInstallPath_ + "RustDedicated_Data\\Managed\\Oxide.Rust.dll";
      if (!std::filesystem::exists(oxideDllPath_))
      {
        std::cout << "WARNING: Main Oxide DLL not found at " << oxideDllPath_ << "; automatic Oxide updates disabled" << std::endl;
        oxideDllPath_.clear();
        oxideUpdateCheck_ = false;
      }
      else if (!IsDirectory(downloadPath_))
      {
        std::cout << "WARNING: Configured download path " << downloadPath_ << " is not a directory; automatic Oxide updates disabled" << std::endl;
        oxideDllPath_.clear();
        downloadPath_.clear();
        oxideUpdateCheck_ = false;
      }
      else if (!IsWritable(downloadPath_))
      {
        std::cout << "WARNING: Configured download path " << downloadPath_ << " is not writable; automatic Oxide updates disabled" << std::endl;
        oxideDllPath_.clear();
        downloadPath_.clear();
        oxideUpdateCheck_ = false;
      }
    }

    // std::cout << "Updater initialized. Server updates " << (serverUpdateCheck_ ? "enabled" : "disabled") << ". Oxide updates " << (oxideUpdateCheck_ ? "enabled" : "disabled") << std::endl;
  }

  Updater::~Updater()
  {
  }

  bool Updater::CheckOxide()
  {
    if (!oxideUpdateCheck_) { return false; }
    const std::string& currentOxideVersion(GetInstalledOxideVersion());
    // std::cout << "Installed Oxide version: '" << currentOxideVersion << "'" << std::endl;
    const std::string& latestOxideVersion(GetLatestOxideVersion());
    // std::cout << "Latest Oxide version: '" << latestOxideVersion << "'" << std::endl;
    return (
      !currentOxideVersion.empty() && !latestOxideVersion.empty() &&
      currentOxideVersion != latestOxideVersion
    );
  }

  bool Updater::CheckServer()
  {
    if (!serverUpdateCheck_) { return false; }
    const std::string& currentServerVersion(GetInstalledServerBuild());
    // std::cout << "Installed Server version: '" << currentServerVersion << "'" << std::endl;
    const std::string& latestServerVersion(
      GetLatestServerBuild(GetInstalledServerBranch())
    );
    // std::cout << "Latest Server version: '" << latestServerVersion << "'" << std::endl;
    return (
      !currentServerVersion.empty() && !latestServerVersion.empty() &&
      currentServerVersion != latestServerVersion
    );
  }

  void Updater::UpdateOxide(const bool suppressWarning)
  {
    // abort if any required path is empty, meaning it failed validation
    if (downloadPath_.empty() || serverInstallPath_.empty())
    {
      std::cout << "ERROR: Cannot update Oxide because download and/or server install path is invalid" << std::endl;
      return;
    }
    // abort if Oxide was not already installed
    if (oxideDllPath_.empty())
    {
      if (!suppressWarning)
      {
        std::cout << "WARNING: Cannot update Oxide because a previous installation was not detected" << std::endl;
      }
      return;
    }

    const std::string zipFile(downloadPath_ + "oxide.zip");

    // download latest Oxide release
    // TODO: URL is hardcoded for now, but maybe we should discover it via RSS?
    if (!downloaderUptr_->GetUrlToFile(
      zipFile, "https://umod.org/games/rust/download?tag=public"
    ))
    {
      std::cout << "ERROR: Failed to download Oxide" << std::endl;
      std::filesystem::remove(zipFile);
      return;
    }

    // unzip Oxide release into server installation directory
    zip_t* zipPtr(zip_open(zipFile.c_str(), 0, 'r'));
    if (!zipPtr)
    {
      std::cout << "ERROR: Failed to open Oxide zip" << std::endl;
      std::filesystem::remove(zipFile);
      return;
    }
    const ssize_t zipEntries(zip_entries_total(zipPtr));
    zip_close(zipPtr);
    if (zipEntries <= 0)
    {
      std::cout << "ERROR: Failed to get valid file count from Oxide zip: " << zip_strerror(zipEntries) << std::endl;
      std::filesystem::remove(zipFile);
      return;
    }

    ZipStatus zipStatus{0, zipEntries};
    const int extractResult(zip_extract(
      zipFile.c_str(), serverInstallPath_.c_str(), ZipExtractCallback, &zipStatus
    ));
    if (extractResult)
    {
      std::cout << "ERROR: Failed to extract Oxide zip: " << zip_strerror(extractResult) << std::endl;
    }
    else
    {
      // std::cout << "Oxide update successful" << std::endl;
    }

    // remove the zip either way
    std::filesystem::remove(zipFile);
  }

  void Updater::UpdateServer()
  {
    // abort if any required path is empty, meaning it failed validation
    if (serverInstallPath_.empty() || steamCmdPath_.empty())
    {
      std::cout << "ERROR: Cannot update server because install and/or steamcmd path is invalid" << std::endl;
      return;
    }

    std::vector<std::string> args {
      "+force_install_dir", serverInstallPath_,
      "+login", "anonymous",
      "+app_update", "258550"
    };
    const std::string& betaKey(GetInstalledServerBranch());
    if (!betaKey.empty())
    {
      args.push_back("-beta");
      args.push_back(betaKey);
    }
    args.push_back("validate");
    args.push_back("+quit");
    // std::cout << "Invoking SteamCMD with args:";
    // for (const auto& a : args)
    // {
    //   std::cout << " " << a;
    // }
    // std::cout << std::endl;
    std::error_code errorCode;
    const int exitCode(boost::process::system(
      boost::process::exe(steamCmdPath_),
      boost::process::args(args),
      boost::process::error(errorCode)
    ));
    if (errorCode)
    {
      std::cout << "WARNING: Error running server update command: " << errorCode.message() << std::endl;
      return;
    }
    if (exitCode)
    {
      std::cout << "WARNING: SteamCMD returned nonzero exit code: " << exitCode << std::endl;
      return;
    }
    // std::cout << "Server update successful" << std::endl;
  }

  std::string Updater::GetAppManifestValue(const std::string& key)
  {
    std::string retVal;
    // now derive steamcmd's path from the app manifest
    std::ifstream appManifestFile(appManifestPath_);
    if (!appManifestFile.is_open() || !appManifestFile)
    {
      std::cout << "ERROR: Cannot open/read app manifest file: " << appManifestPath_ << std::endl;
      return retVal;
    }

    // search the file for the steamcmd info
    // should be a tab, key in double quotes, 2 tabs, and then the target value
    //  in double quotes
    std::string line;
    const std::string quotedKey(std::string("\"") + key + "\"");
    while (std::getline(appManifestFile, line))
    {
      std::size_t index(line.find(quotedKey));
      if (index == std::string::npos) { continue; }
      // found the line of interest - now find the value
      index = line.find('\"', index + quotedKey.length());
      if (index == std::string::npos) { continue; }
      ++index;
      std::size_t endIndex(line.find('\"', index));
      if (endIndex == std::string::npos) { continue; }
      retVal = line.substr(index, endIndex - index);
      break;
    }
    appManifestFile.close();
    return retVal;
  }

  std::string Updater::GetInstalledOxideVersion()
  {
    std::string retVal;
    if (oxideDllPath_.empty()) { return retVal; }
    // run powershell and grab all output into inStream
    boost::process::ipstream inStream;
    // for some reason boost requires explicitly requesting a PATH search unless
    //  we want to pass the entire command as a single string
    const auto psPath(boost::process::search_path("powershell.exe"));
    if (psPath.empty())
    {
      std::cout << "ERROR: Failed to find powershell" << std::endl;
      return retVal;
    }
    std::error_code errorCode;
    const int exitCode(boost::process::system(
      boost::process::exe(psPath),
      boost::process::args({
        "-Command",
        std::string("(Get-Item '") + oxideDllPath_ + "').VersionInfo.ProductVersion"
      }),
      boost::process::std_out > inStream,
      boost::process::error(errorCode)
    ));
    if (errorCode)
    {
      std::cout << "ERROR: Error running Oxide version check command: " << errorCode.message() << std::endl;
      return retVal;
    }
    if (exitCode)
    {
      std::cout << "ERROR: Powershell returned nonzero exit code: " << exitCode << std::endl;
      return retVal;
    }
    // grab first line of output stream into retVal string
    std::getline(inStream, retVal);
    // for some reason this has a newline at the end, so strip that off
    while (retVal.back() == '\r' || retVal.back() == '\n') { retVal.pop_back(); }
    return retVal;
  }

  std::string Updater::GetInstalledServerBranch()
  {
    return GetAppManifestValue("BetaKey");
  }

  std::string Updater::GetInstalledServerBuild()
  {
    return GetAppManifestValue("buildid");
  }

  std::string Updater::GetLatestServerBuild(const std::string& branch)
  {
    std::string retVal;
    // abort if any required path is empty, meaning it failed validation
    if (serverInstallPath_.empty() || steamCmdPath_.empty())
    {
      std::cout << "ERROR: Cannot check for server updates because install and/or steamcmd path is invalid" << std::endl;
      return retVal;
    }
    // write a script for steamcmd to run
    // this is needed because steamcmd acts very buggy when I try to use other
    //  methods
    // TODO: this should go in RLS' data directory, not the server's
    const std::string scriptFilePath(serverInstallPath_ + "\\steamcmd.scr");
    std::ofstream scriptFile(scriptFilePath, std::ios::trunc);
    if (!scriptFile.is_open())
    {
      std::cout << "ERROR: Failed to open steamcmd script file `" << scriptFilePath << "`" << std::endl;
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
      boost::process::exe(steamCmdPath_),
      boost::process::args({"+runscript", scriptFilePath}),
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
      std::cout << "WARNING: Error running server update command: " << errorCode.message() << std::endl;
    }
    const int exitCode(sc.exit_code());
    if (exitCode)
    {
      std::cout << "WARNING: SteamCMD returned nonzero exit code: " << exitCode << std::endl;
    }
    // audit output
    if (readState != SteamCmdReadState::COMPLETE)
    {
      std::cout << "ERROR: SteamCMD output did not include a valid app info tree" << std::endl;
      return retVal;
    }
    // process output
    std::stringstream ss(steamInfo);
    try
    {
      boost::property_tree::ptree tree;
      boost::property_tree::read_info(ss, tree);
      retVal = tree.get<std::string>(
        std::string("258550.depots.branches.") +
        (branch.empty() ? "public" : branch) +
        ".buildid"
      );
    }
    catch (const std::exception& ex)
    {
      std::cout << "ERROR: Exception parsing SteamCMD output: " << ex.what() << std::endl;
      retVal.clear();
    }
    return retVal;
  }

  std::string Updater::GetLatestOxideVersion()
  {
    std::string retVal;
    if (!downloaderUptr_)
    {
      std::cout << "ERROR: Downloader handle is null" << std::endl;
      return retVal;
    }
    const std::string& oxideInfo(
      downloaderUptr_->GetUrlToString("https://umod.org/games/rust.rss")
    );
    pugi::xml_document xmlDoc;
    const auto& xmlParseResult(xmlDoc.load_string(oxideInfo.c_str()));
    if (!xmlParseResult)
    {
      std::cout << "ERROR: XML parse error: " << xmlParseResult.description() << std::endl;
      return retVal;
    }
    // there should be a "feed" node with a list of "entry" (plus other) nodes
    //  under it. Under each "entry" node (among other things) is something
    //  like:
    //
    //   <title type="text"><![CDATA[2.0.5800]]></title>
    //
    //  ...and:
    //
    //   <id>https://umod.org/games/rust/download?tag=public</id>
    //
    // for now, it should be sufficient to extract the `CDATA` values as
    //  lexicographically-comparable version strings, then find the largest one
    //  and optionally grab its siblilng `id` value for a download URL
    auto xmlFeedNode(xmlDoc.child("feed"));
    if (!xmlFeedNode)
    {
      std::cout << "ERROR: No `feed` node under Oxide RSS XML root" << std::endl;
      return retVal;
    }
    for (auto xmlEntryNode : xmlFeedNode.children("entry"))
    {
      const std::string value(xmlEntryNode.child_value("title"));
      if (retVal.empty() || value > retVal) { retVal = std::move(value); }
    }

    return retVal;
  }
}
