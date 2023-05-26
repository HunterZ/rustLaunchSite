#ifndef RCON_H
#define RCON_H

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace rustLaunchSite
{
  // forward declarations
  struct RconInitHandle;
  struct WebSocket;
  struct WebSocketMessage;

  /// @brief Rust WebRCON client facility
  /// @details Provides RAII-based connectivity to Rust dedicated server
  ///  websocket RCON endpoint, plus API to execute commands and retrieve
  ///  responses. Only the constructor should throw exceptions.
  class Rcon
  {
    public:

      /// @brief Public constructor
      /// @details Attempts to establish connection immediately. Starting
      ///  identifier can be specified to help avoid collision between multiple
      ///  connections to the same server. @todo maybe use a random identifier
      ///  for each command instead?
      /// @param hostOrIp Hostname or IP address of RCON server
      /// @param port Port of RCON server
      /// @param password Password for RCON authentication
      /// @param startIdentifier First monotonically-increasing identifier used
      ///  to match RCON requests with replies
      /// @throw @c std::runtime_error if underlying websocket handle creation
      ///  fails
      explicit Rcon(
        const std::string& hostOrIp,
        const int port,
        const std::string& password,
        const long long startIdentifier = 1
      );

      /// @brief Destructor
      /// @details Does nothing, but compiler errors out when using pimpl idiom
      ///  with @c std::unique_ptr if it's not defined
      ~Rcon();

      /// @brief Query whether RCON connection is established
      /// @return @c true if RCON is connected, or @c false if attempting to
      ///  (re)connect
      bool IsConnected() const;

      /// @brief Send command to RCON server and optionally return response if
      ///  received before specified timeout has elapsed
      /// @details Usage of JSON etc. is abstracted away, such that callers can
      ///  use simple strings; only the message part of responses is returned,
      ///  and line break sequences etc. are not converted. This call will block
      ///  for up to the specified/default timeout period before returning, but
      ///  will return immediately if not connected.
      /// @param command RCON command to be executed
      /// @param timeoutMilliseconds Maximum number of milliseconds to block
      ///  waiting for a response, or zero to return without waiting
      /// @return Response message, or empty string if not connected, no
      ///  response received, or any other error occurs
      std::string SendCommand(
        const std::string& command, const std::size_t timeoutMilliseconds
      );

      /// @todo add API for retrieving RCON messages that are not the result of
      ///  a command?

    protected:

    private:

      // websocket callback handler
      void WebsocketMessageHandler(const WebSocketMessage& message);

      // disabled constructors/operators

      Rcon() = delete;
      Rcon(const Rcon&) = delete;
      Rcon& operator= (const Rcon&) = delete;

      // handle to manage global (de)init of underlying functionality
      std::shared_ptr<RconInitHandle> rconInitHandleSptr_;
      // websocket connection handle
      std::unique_ptr<WebSocket> webSocketUptr_;
      // monotonically increasing identifier
      long long identifier_;
      // mutex for thread safety between user calls and WebSocket library
      // a recursive mutex is used for now because of a nasty issue in which IX
      //  sometimes invokes our callback handler from our own thread:
      //  https://github.com/machinezone/IXWebSocket/issues/457
      mutable std::recursive_mutex mutex_;
      // condition variable for waiting for / signaling the receipt of responses
      //  to RCON commands, as receipt processing happens in a background thread
      // condition_variable_any is used for the same reason as recursive_mutex
      std::condition_variable_any cv_;
      // received RCON response messages by identifier
      std::map<long long, std::string> responses_;
      // outstanding RCON request identifiers
      std::set<long long> requests_;
  };
}

#endif // RCON_H
