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
  // maintain weak pointer to init handle
  // only Downloader instances will hold shared pointers to it, but this is
  //  needed to ensure they all get a shared pointer to the same handle instance
  std::weak_ptr<rustLaunchSite::DownloaderInitHandle>
    DOWNLOADER_INIT_HANDLE_WPTR;
  // use a mutex in case two Downloader instances get created at the same time
  //  in different threads
  std::mutex DOWNLOADER_INIT_HANDLE_MUTEX;

  // get init handle, creating one if necessary (which in turn performs init)
  // if weak pointer is valid, then there is a Downloader instance holding a
  //  shared pointer to an active init handle and we can just return a copy of
  //  that; otherwise create a new one, capture it in the weak pointer, and
  //  return it
  std::shared_ptr<rustLaunchSite::DownloaderInitHandle>
    GetDownloaderInitHandle()
  {
    // lock mutex to prevent concurrent creation of multiple handles
    std::scoped_lock lock(DOWNLOADER_INIT_HANDLE_MUTEX);
    // attempt to upgrade weak pointer to shared pointer
    auto handleSptr(DOWNLOADER_INIT_HANDLE_WPTR.lock());
    if (!handleSptr)
    {
      // no handle exists, so create and cache one
      DOWNLOADER_INIT_HANDLE_WPTR = handleSptr =
        std::make_shared<rustLaunchSite::DownloaderInitHandle>();
    }
    // return new or existing handle
    return handleSptr;
  }

  // Curl requires a C-style callback handler, so this function accumulates
  //  downloaded data into a file
  std::size_t WriteToFile(
    char* dataPtr, std::size_t size, std::size_t nmemb, std::ofstream* filePtr
  )
  {
    if (!dataPtr || !filePtr)
    {
      std::cout << "Null pointer(s) passed to Curl file write handler" << std::endl;
      return 0;
    }
    const std::size_t numBytes(size * nmemb);
    filePtr->write(dataPtr, numBytes);
    // std::cout << "Wrote " << numBytes << " bytes to download file" << std::endl;
    return numBytes;
  }

  // Curl requires a C-style callback handler, so this function accumulates
  //  downloaded data into a string
  std::size_t WriteToString(
    char* dataPtr, std::size_t size, std::size_t nmemb, std::string* stringPtr
  )
  {
    if (!dataPtr || !stringPtr)
    {
      std::cout << "Null pointer(s) passed to Curl string write handler" << std::endl;
      return 0;
    }
    const std::size_t numBytes(size * nmemb);
    stringPtr->append(dataPtr, numBytes);
    return numBytes;
  }
}

namespace rustLaunchSite
{
  // RAII wrapper for underlying init API
  struct DownloaderInitHandle
  {
    DownloaderInitHandle()
    {
      curl_global_init(CURL_GLOBAL_ALL);
      std::cout << "Initialized Downloader library" << std::endl;
    }
    ~DownloaderInitHandle()
    {
      curl_global_cleanup();
      std::cout << "Uninitialized Downloader library" << std::endl;
    }
  };
  // end of init stuff

  Downloader::Downloader()
    : downloaderInitHandleSptr_(GetDownloaderInitHandle())
  {
  }

  std::string Downloader::GetUrlToString(const std::string& url)
  {
    std::string retVal;
    auto* curlPtr(curl_easy_init());
    if (!curlPtr)
    {
      std::cout << "WARNING: curl_easy_init() returned nullptr" << std::endl;
      return retVal;
    }
    CURLcode curlCode(CURLE_OK);
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_URL, url.c_str()) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_FOLLOWLOCATION, 1L) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_WRITEFUNCTION, WriteToString) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_WRITEDATA, &retVal) : curlCode;
    curlCode = curlCode == CURLE_OK ?
      curl_easy_perform(curlPtr) : curlCode;
    curl_easy_cleanup(curlPtr);
    if (curlCode != CURLE_OK)
    {
      std::cout << "WARNING: Curl failure for URL `" << url << "`: " << curl_easy_strerror(curlCode) << std::endl;
      retVal.clear();
    }
    return retVal;
  }

  bool Downloader::GetUrlToFile(const std::string& file, const std::string& url)
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
      std::cout << "WARNING: curl_easy_init() returned nullptr" << std::endl;
      outFile.close();
      std::filesystem::remove(file);
      return false;
    }
    CURLcode curlCode(CURLE_OK);
    curlCode = curlCode == CURLE_OK ?
      curl_easy_setopt(curlPtr, CURLOPT_URL, url.c_str()) : curlCode;
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
      std::cout << "WARNING: Curl failure for URL `" << url << "`: " << curl_easy_strerror(curlCode) << std::endl;
      std::filesystem::remove(file);
      return false;
    }
    return true;
  }
}
