#ifndef SERVER_H
#define SERVER_H

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rustLaunchSite
{
class  Config;
class  Rcon;
struct ProcessImpl;

/// @brief rustLaunchSite server management facility
/// @details Implements use cases and state relating to the management of a
///  potentially-running dedicated server application instance. Only the
///  constructor should throw exceptions.
class Server
{
public:

  /// @brief Primary constructor
  /// @param cfgSptr Shared pointer to application configuration
  /// @details Starts RCON service immediately.
  /// @throw @c std::invalid_argument if dedicated server binary or install
  ///  path are not found, or @c std::runtime_error if RCON facility
  ///  creation failed
  explicit Server(std::shared_ptr<const Config> cfgSptr);

  /// @brief Destructor
  /// @details For some reason this needs to be explicitly declared in order
  ///  to support having a member @c unique_ptr to a forward-declared type.
  ~Server();

  /// @brief Server info of interest that can be retrieved via RCON queries
  struct Info
  {
    /// @brief @c true if data valid, @c false if query failed
    bool valid_{false};
    /// @brief Number of players currently connected
    std::size_t players_{0};
    /// @brief Client-server protocol version
    std::string protocol_{};
  };

  /// @brief Query server for various info via RCON
  /// @details All fields should be populated if validity indicator set.
  ///  Will immediately fail if server is not running. May block the caller
  ///  for a period of time to give the server a chance to respond.
  /// @return Struct containing results
  Info GetInfo();

  /// @brief Query whether the server is running
  /// @details This may be based on a cached value. Does not imply that the
  ///  server is fully started, or that RCON is available. Does not imply
  ///  that a @c Start() call is needed to (re)start the server.
  /// @return @c true if the server process is currently running, @c false
  ///  if it is in a stopped/restart state
  bool IsRunning() const;

  /// @brief Send RCON command to server, optionally waiting for a response
  /// @param command RCON console command to send
  /// @param waitForResponse @c true to block for a limited amount of time
  ///  while waiting for a response, or @c false to return immediately after
  ///  sending command
  /// @return Response message from server, or empty if none due to RCON not
  ///  available, timeout, error, @c waitForResponse=false etc.
  std::string SendRconCommand(
    const std::string& command,
    const bool waitForResponse
  );

  /// @brief Start the server
  /// @details This is a non-blocking call. A successful return value does
  ///  not guarantee that the server will manage to come all the way up.
  /// @return @c true if server appeared to launch or is already running,
  ///  or @c false if an error was detected
  bool Start();

  /// @brief Stop the server
  /// @details Blocks until the server shuts down. Attempts a graceful
  ///  shutdown, but will graduate to more forceful methods if/as needed.
  ///  The server will not be automatically restarted when stopped via this
  ///  method, so bringing it back up will require calling @c Start() again.
  ///  Does nothing if the server is already stopped.
  /// @param reason Optional shutdown reason, provided to anyone monitoring
  ///  the server (online players, Discord integrations, etc.)
  void Stop(const std::string& reason = {});

private:

  // disabled constructors/operators

  Server() = delete;
  Server(const Server&) = delete;
  Server& operator= (const Server&) = delete;

  // perform stop delay processing
  // if a stop delay is configured, loop until it has elapsed, or until all
  //  players have disconnected (whichever occurs first)
  void StopDelay(std::string_view reason = {});

  // unique pointer to low-level server process management interface
  // this is a pointer to an opaque type to avoid leaking a dependency on
  //  underlying process management API headers
  std::unique_ptr<ProcessImpl> processImplUptr_;
  // unique pointer to RCON interface
  // this is a pointer because it gets allocated and destroyed as the server
  //  process is started and stopped
  std::unique_ptr<Rcon> rconUptr_;
  // ordered list of command-line arugments to be passed to Rust dedicated
  //  server binary on launch
  std::vector<std::string> rustDedicatedArguments_;
  // path to Rust dedicated server binary
  std::filesystem::path rustDedicatedPath_;
  // number of seconds to delay server shutdown when users logged on
  // zero means don't wait even if users are logged on
  std::size_t stopDelaySeconds_;
  // path that should be used as working directory when launching server
  std::filesystem::path workingDirectory_;
};
}

#endif // SERVER_H
