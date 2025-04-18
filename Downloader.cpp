#include "Downloader.h"

#include <algorithm>
#include <cstddef>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <mutex>

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
  char const* dataPtr, std::size_t size, std::size_t nmemb,
  std::ofstream* filePtr
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
  char const* dataPtr, std::size_t size, std::size_t nmemb,
  std::string* stringPtr
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

// Curl requires a C-style callback handler, so this function accumulates
//  downloaded data into a binary data buffer
std::size_t WriteToVector(
  char const* dataPtr, std::size_t size, std::size_t nmemb,
  std::vector<char>* vPtr
)
{
  if (!dataPtr || !vPtr)
  {
    std::cout << "Null pointer(s) passed to Curl vector write handler\n";
    return 0;
  }
  const std::size_t numBytes(size * nmemb);
  std::copy(dataPtr, dataPtr + numBytes, std::back_inserter(*vPtr));
  return numBytes;
}
} // anonymous namespace end

namespace rustLaunchSite
{
Downloader::Downloader() = default;

bool Downloader::GetUrlToFile(
  const std::filesystem::path& file,
  std::string_view url
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

std::string Downloader::GetUrlToString(std::string_view url) const
{
  std::string retVal{};
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

std::vector<char> Downloader::GetUrlToVector(std::string_view url) const
{
  std::vector<char> retVal{};
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
    curl_easy_setopt(curlPtr, CURLOPT_WRITEFUNCTION, WriteToVector) : curlCode;
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

Downloader::InitHandle Downloader::GetInitHandle()
{
  // set up a mutex ASAP to ensure that weak pointer only ever references one
  //  shared pointer at a time
  static std::mutex mutex;
  std::scoped_lock lock{mutex};

  static std::size_t handleNumber{0};
  static std::weak_ptr<std::size_t> handleWptr{};

  // attempt to upgrade weak pointer to a shared pointer ASAP
  auto existingHandleSptr{handleWptr.lock()};
  // if we got a valid shared pointer, return it
  if (existingHandleSptr) { return existingHandleSptr; }

  // shared pointer is invalid, so make a new one
  ++handleNumber;
  std::cout << "Initializing downloader handle #" << handleNumber << std::endl;
  curl_global_init(CURL_GLOBAL_ALL);
  // shared pointer points at our static handle count, but instead of managing
  //  its memory, the deleter performs a global libcurl cleanup action
  InitHandle newHandleSptr
  {
    &handleNumber, [](std::size_t const* hn)
    {
      std::cout << "Deinitializing downloader handle #" << *hn << std::endl;
      curl_global_cleanup();
    }
  };
  handleWptr = newHandleSptr;
  return newHandleSptr;
}
} // rustLaunchSite namespace end
