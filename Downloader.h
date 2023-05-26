#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <memory>
#include <string>

namespace rustLaunchSite
{
  // forward declarations
  struct DownloaderInitHandle;

  /// @brief URL download facility
  /// @details Encapsulates use of low-level API for downloading the contents of
  ///  a URL to a file or string. Global initialization of the underlying API
  ///  will occur on first instantiation of this class, and it will be
  ///  deinitialized when the last concurrent instance of this class is
  ///  destroyed; it is recommended that this cycle not be allowed to occur more
  ///  than once during a single run of the application. Should not throw any
  ///  exceptions.
  class Downloader
  {
    public:

      /// @brief Primary constructor
      /// @details Performs global init of underlying API if needed, which is
      ///  the only state managed by this class.
      Downloader();

      /// @brief Download specified URL contents to a string
      /// @param url URL whose contents should be downloaded
      /// @return Contents retrieved from URL, or empty string on failure
      std::string GetUrlToString(const std::string& url);

      /// @brief Download specified URL contents to a file
      /// @details The file will be truncated prior to download attempt.
      /// @param file File to which URL contents should be saved
      /// @param url URL whose contents should be downloaded
      /// @return @c true on success, @c false on failure
      bool GetUrlToFile(const std::string& file, const std::string& url);

    protected:

    private:

      // disabled constructors/operators

      Downloader(const Downloader&) = delete;
      Downloader& operator= (const Downloader&) = delete;

      // handle to manage global (de)init of underlying functionality
      std::shared_ptr<DownloaderInitHandle> downloaderInitHandleSptr_;
  };
}

#endif // DOWNLOADER_H
