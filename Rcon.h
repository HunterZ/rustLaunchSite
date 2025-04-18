#ifndef RCON_H
#define RCON_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

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
  /// @details Attempts to establish connection immediately.
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
    const bool logMessages
  );

  /// @brief Destructor
  /// @details Does nothing, but compiler errors out when using pimpl idiom
  ///  with @c std::unique_ptr if it's not defined
  /// @todo Class should probably support inheritance for unit test mocking
  ///  purposes, which among other things would mean marking this @c virtual
  ~Rcon();

  /// @brief Query whether RCON connection is established
  /// @return @c true if RCON is connected, or @c false if attempting to
  ///  (re)connect
  bool IsConnected() const;

  using MessageHandler = std::function<void(const std::string&)>;

  /// @brief Register a function to handle broadcast messages
  /// @param handler Function that should be invoked to process messages
  /// @details Does not check for duplicate registrations, so this should be
  ///  avoided without good reason. Handlers will only be invoked for
  ///  non-response messages. Caller is responsible for ensuring that
  ///  @c handler is always safe to invoke.
  void Register(MessageHandler handler);

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

private:

  // get shared pointer to current init handle, or new one if none exists yet
  //
  // If no handle exists, init occurs and a new handle is returned. Else
  //  nothing happens, and the already-existing handle is returned. When all
  //  pointers to the handle have destructed, deinit occurs and the handle is
  //  destroyed.
  //
  // The point of this is to ensure that the underlying API gets initialized on
  //  first need and deinitialized when no longer needed, while also preventing
  //  attempts to reinitialize while already in use. Consequently, each @c Rcon
  //  instance calls this on construction and holds onto the @c shared_ptr
  //  during its lifetime.
  static std::shared_ptr<RconInitHandle> GetInitHandle();

  // websocket callback handler
  void WebsocketMessageHandler(const WebSocketMessage& message);

  // disabled constructors/operators

  Rcon() = delete;
  Rcon(const Rcon&) = delete;
  Rcon& operator= (const Rcon&) = delete;

  // static weak pointer to current init handle (if one exists)
  // this is used to ensure that the current handle gets reused across
  //  instances, but also that deinitialization occurs when the last
  //  instance drops its co-ownership of the handle
  static std::weak_ptr<RconInitHandle> rconInitHandleWptr_;
  // mutex to enforce thread-safe creation of init handles
  static std::mutex rconInitHandleMutex_;

  // condition variable for waiting for / signaling the receipt of responses
  //  to RCON commands, as receipt processing happens in a background thread
  // condition_variable_any is used for the same reason as recursive_mutex
  std::condition_variable_any cv_;
  // whether RCON messages should be logged for debugging purposes
  bool logMessages_;
  // message callback registrants
  // this is a vector because functions don't support being sorted
  std::vector<MessageHandler> messageHandlers_;
  // mutex for thread safety between user calls and WebSocket library
  // a recursive mutex is used for now because of a nasty issue in which IX
  //  sometimes invokes our callback handler from our own thread:
  //  https://github.com/machinezone/IXWebSocket/issues/457
  mutable std::recursive_mutex mutex_;
  // instance-specific shared pointer to init handle
  std::shared_ptr<RconInitHandle> rconInitHandleSptr_ = GetInitHandle();
  // maintainability alias for request ID type
  using REQUEST_ID_TYPE = int32_t;
  // outstanding RCON request identifiers
  std::unordered_set<REQUEST_ID_TYPE> requests_;
  // received RCON response messages by identifier
  std::map<REQUEST_ID_TYPE, std::string> responses_;
  // websocket connection handle
  std::unique_ptr<WebSocket> webSocketUptr_;
};
}

#endif // RCON_H
