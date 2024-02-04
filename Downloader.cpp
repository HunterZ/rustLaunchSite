#include "Downloader.h"

#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>

// NOTE: The Downloader initialization handle stuff in this file is an
//  implementation detail needed because some of the underlying libraries being
//  used require global init and deinit.
// The first Downloader class instance created will trigger init, and deinit
//  will occur when the last Downloader instance is destroyed.
// This is a little janky in that init+deinit could occur multiple times if a
//  Downloader instance is created after all previous instances have been
//  destroyed, but it seems like a better starting point than requiring that
//  main() call static functions.

namespace
{
  // Curl requires a C-style callback handler, so this function accumulates
  //  downloaded data into a file
  std::size_t WriteToFile(
    char const* dataPtr, std::size_t size, std::size_t nmemb, std::ofstream* filePtr
  )
  {
    if (!dataPtr || !filePtr)
    {
      std::cout << "Null pointer(s) passed to Curl file write handler\n";
      return 0;
    }
    const std::size_t numBytes(size * nmemb);
    filePtr->write(dataPtr, numBytes);
    // std::cout << "Wrote " << numBytes << " bytes to download file\n";
    return numBytes;
  }

  // Curl requires a C-style callback handler, so this function accumulates
  //  downloaded data into a string
  std::size_t WriteToString(
    char const* dataPtr, std::size_t size, std::size_t nmemb, std::string* stringPtr
  )
  {
    if (!dataPtr || !stringPtr)
    {
      std::cout << "Null pointer(s) passed to Curl string write handler\n";
      return 0;
    }
    const std::size_t numBytes(size * nmemb);
    stringPtr->append(dataPtr, numBytes);
    return numBytes;
  }
}

namespace rustLaunchSite
{
  Downloader::Downloader() = default;

  std::string Downloader::GetUrlToString(const std::string_view url) const
  {
    std::string retVal;
    auto* curlPtr(curl_easy_init());
    if (!curlPtr)
    {
      std::cout << "WARNING: curl_easy_init() returned nullptr\n";
      return retVal;
    }
    CURLcode curlCode(CURLE_OK);
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_URL, url.data()) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_FOLLOWLOCATION, 1L) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_USERAGENT, "rustLaunchSite") : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_WRITEFUNCTION, WriteToString) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_WRITEDATA, &retVal) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_perform(curlPtr) : curlCode;
    curl_easy_cleanup(curlPtr);
    if (curlCode != CURLE_OK)
    {
      std::cout << "WARNING: Curl failure for URL `" << url << "`: " << curl_easy_strerror(curlCode) << "\n";
      retVal.clear();
    }
    return retVal;
  }

  bool Downloader::GetUrlToFile(
    const std::filesystem::path& file,
    const std::string_view url
  ) const
  {
    std::ofstream outFile(
      file, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc
    );
    if (!outFile)
    {
      std::cout << "WARNING: Failed to open output file for write: " << file;
      return false;
    }
    auto* curlPtr(curl_easy_init());
    if (!curlPtr)
    {
      std::cout << "WARNING: curl_easy_init() returned nullptr\n";
      outFile.close();
      std::filesystem::remove(file);
      return false;
    }
    CURLcode curlCode(CURLE_OK);
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_URL, url.data()) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_FOLLOWLOCATION, 1L) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_WRITEFUNCTION, WriteToFile) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_WRITEDATA, &outFile) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_perform(curlPtr) : curlCode;
    curl_easy_cleanup(curlPtr);
    outFile.close();
    if (curlCode != CURLE_OK)
    {
      std::cout << "WARNING: Curl failure for URL `" << url << "`: " << curl_easy_strerror(curlCode) << "\n";
      std::filesystem::remove(file);
      return false;
    }
    return true;
  }

  Downloader::InitHandle Downloader::GetInitHandle()
  {
    static std::mutex mutex;
    static std::size_t handleNumber{0};
    static std::weak_ptr<std::size_t> handleWptr{};

    std::scoped_lock lock{mutex};
    if (handleWptr.expired())
    {
      ++handleNumber;
      std::cout << "Initializing downloader handle #" << handleNumber << std::endl;
      curl_global_init(CURL_GLOBAL_ALL);
      InitHandle handleSptr
      {
        &handleNumber, [](std::size_t const* hn)
        {
          std::cout << "Deinitializing downloader handle #" << *hn << std::endl;
          curl_global_cleanup();
        }
      };
      handleWptr = handleSptr;
    }
    return handleWptr.lock();
  }
}
