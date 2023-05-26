#include "Rcon.h"

#include <chrono>
#include <iostream>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

// NOTE: The RCON initialization handle stuff in this file is an implementation
//  detail needed because some of the underlying libraries being used require
//  global init and deinit.
// The first Rcon class instance created will trigger init, and deinit will
//  occur when the last Rcon instance is destroyed.
// This is a little janky in that init+deinit could occur multiple times if an
//  Rcon instance is created after all previous instances have been destroyed,
//  but it seems like a better starting point than requiring that main() call
//  static functions.

namespace
{
  // maintain weak pointer to init handle
  // only Rcon instances will hold shared pointers to it, but this is needed to
  //  ensure they all get a shared pointer to the same handle instance
  std::weak_ptr<rustLaunchSite::RconInitHandle> RCON_INIT_HANDLE_WPTR;
  // use a mutex in case two Rcon instances get created at the same time in
  //  different threads
  std::mutex RCON_INIT_HANDLE_MUTEX;

  // get init handle, creating one if necessary (which in turn performs init)
  // if weak pointer is valid, then there is an Rcon instance holding a shared
  //  pointer to an active init handle and we can just return a copy of that;
  //  otherwise create a new one, capture it in the weak pointer, and return it
  std::shared_ptr<rustLaunchSite::RconInitHandle> GetRconInitHandle()
  {
    // lock mutex to prevent concurrent creation of multiple handles
    std::scoped_lock lock(RCON_INIT_HANDLE_MUTEX);
    // attempt to upgrade weak pointer to shared pointer
    auto handleSptr(RCON_INIT_HANDLE_WPTR.lock());
    if (!handleSptr)
    {
      // no handle exists, so create and cache one
      RCON_INIT_HANDLE_WPTR = handleSptr =
        std::make_shared<rustLaunchSite::RconInitHandle>();
    }
    // return new or existing handle
    return handleSptr;
  }
}

namespace rustLaunchSite
{
  // RAII wrapper for underlying init API
  struct RconInitHandle
  {
    RconInitHandle()
    {
      ix::initNetSystem();
      std::cout << "Initialized WebSocket library" << std::endl;
    }
    ~RconInitHandle()
    {
      ix::uninitNetSystem();
      std::cout << "Uninitialized WebSocket library" << std::endl;
    }
  };
  // end of init stuff

  // these are simple wrappers around the underlying websocket library's types
  //  in order to avoid leaking header-level dependencies
  struct WebSocket
  {
    ix::WebSocket webSocket_;

    ix::WebSocket& operator()() { return webSocket_; }
  };

  struct WebSocketMessage
  {
    const ix::WebSocketMessagePtr& webSocketMessagePtr_;

    explicit WebSocketMessage(const ix::WebSocketMessagePtr& msgPtr)
      : webSocketMessagePtr_(msgPtr) {}

    const ix::WebSocketMessagePtr& operator()() const
    {
      return webSocketMessagePtr_;
    }
  };

  Rcon::Rcon(
    const std::string& hostOrIp, const int port, const std::string& password,
    const long long startIdentifier
  )
    : rconInitHandleSptr_(GetRconInitHandle())
    , webSocketUptr_(std::make_unique<WebSocket>())
    , identifier_(startIdentifier)
  {
    if (!webSocketUptr_)
    {
      throw std::runtime_error("Failed to create underlying WebSocket handle");
    }
    auto& webSocket((*webSocketUptr_)());
    // Rust doesn't support secure WebSocket connections, so form insecure URL
    std::stringstream s;
    s << port;
    const std::string url(std::string("ws://") + hostOrIp + ":" + s.str() + "/" + password);
    webSocket.setUrl(url);
    // Don't enable a heartbeat: Rust seems to have a mickey-mouse WebSocket
    //  implementation that doesn't support the required ping/pong protocol,
    //  which causes the WebSocket library to freak out and reset the connection
    //  at every heartbeat ping.
    // webSocket.setPingInterval(45);
    webSocket.enableAutomaticReconnection();
    webSocket.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr& msg)
      {
        this->WebsocketMessageHandler(WebSocketMessage(msg));
      }
    );

    webSocket.start();
    std::cout << "Started WebSocket RCON connection to server: " << hostOrIp << std::endl;
  }

  Rcon::~Rcon()
  {
    auto& webSocket((*webSocketUptr_)());
    webSocket.stop();
  }

  bool Rcon::IsConnected() const
  {
    auto& webSocket((*webSocketUptr_)());
    return (webSocket.getReadyState() == ix::ReadyState::Open);
  }

  std::string Rcon::SendCommand(
    const std::string& command,
    const std::size_t timeoutMilliseconds
  )
  {
    if (!IsConnected())
    {
      std::cout << "Ignoring RCON command due to no connection" << std::endl;
      return std::string();
    }

    // unique_lock is used here instead of scoped_lock so that condition
    //  variable wait can unlock it
    std::unique_lock lock(mutex_);

    // cache current identifier, then increment
    const long long identifier(identifier_++);

    // Rust RCON message format is JSON with a simple list of 3 parameters:
    // - 'Identifier' set to a number, which will be used in the response
    // - 'Message' set to the command to be executed
    // - 'Name' set to WebRcon
    nlohmann::json j;
    std::stringstream s;
    j["Identifier"] = identifier;
    j["Message"] = command;
    j["Name"] = "WebRcon";

    // record pending request ID if timeout specified
    if (timeoutMilliseconds)
    {
      // std::cout << "Queued request for RCON response to command ID=" << identifier << std::endl;
      requests_.insert(identifier);
    }
    // else
    // {
    //   std::cout << "Not requesting response for RCON comamnd ID=" << identifier << std::endl;
    // }

    auto& webSocket((*webSocketUptr_)());
    const auto& result(webSocket.send(j.dump()));
    // std::cout
    //   << "Sent RCON command via WebSocket"
    //   << ": ID=" << identifier
    //   << ", message=" << j.dump()
    //   << ", compressionError=" << result.compressionError
    //   << ", payloadSize=" << result.payloadSize
    //   << ", success=" << result.success
    //   << ", wireSize=" << result.wireSize
    //   << std::endl;

    // TODO: any special failure handling needed?

    // the rest of this method is concerned with waiting for a response, so bail
    //  out here if no timeout specified
    if (!timeoutMilliseconds)
    {
      // std::cout << "*** skipping RCON response wait" << std::endl;
      return std::string();
    }

    if (result.success)
    {
      // calculate timeout time in case we don't get a response
      const auto timeout(
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMilliseconds)
      );
      // std::cout << "*** waiting on RCON response for " << timeoutMilliseconds << " ms, until time: " << timeout.time_since_epoch().count() << std::endl;
      // wait until condition variable notified (potential response received
      //  while waiting), timeout elapsed (no response), or stop condition
      //  encountered (response receipt confirmed)
      // this will release the mutex until the wait completes, at which point it
      //  will be reacquired
      // `success` contains the final stop condition evaluation, which will be
      //  true if a response was received, or false if a timeout occurred
      // const bool success(
      cv_.wait_until(
        lock, timeout, [this, identifier]()
        {
          return (responses_.find(identifier) != responses_.end());
        }
      );
      // std::cout << "*** success=" << success << std::endl;
    }

    // remove pending request ID, as the transaction is considered complete at
    //  this point, regardless of outcome
    requests_.erase(identifier);
    // std::cout << "SendCommand(): Removed pending request ID=" << identifier << std::endl;

    const auto responseIter(responses_.find(identifier));
    if (responseIter == responses_.end())
    {
      std::cout << "WARNING: Response wait timed out for RCON command: " << command << std::endl;
      return std::string();
    }

    // move the response string into a local variable so that it doesn't get
    //  invalidated when we erase it from the map
    const std::string response(std::move(responseIter->second));
    responses_.erase(responseIter);
    return response;
  }

  void Rcon::WebsocketMessageHandler(const WebSocketMessage& message)
  {
    std::scoped_lock lock(mutex_);

    // std::cout << "Processing WebSocket event; pending request IDs: {";
    // for (const auto i : requests_)
    // {
    //   std::cout << " " << i;
    // }
    // std::cout << " }" << std::endl;

    const auto& msg(*(message()));
    switch (msg.type)
    {
      case ix::WebSocketMessageType::Message:
      {
        // std::cout << "WebsocketMessageHandler(): Processing received message: " << msg.str << std::endl;
        const nlohmann::json j(nlohmann::json::parse(msg.str));
        if (j.contains("Identifier"))
        {
          const long long id(j["Identifier"].get<long long>());
          if (id > 0) // id > 0 <=> response
          {
            auto reqIter(requests_.find(id));
            if (reqIter == requests_.end())
            {
              // std::cout << "WARNING: Ignoring RCON response with unknown ID=" << id << ": " << msg.str << std::endl;
            }
            else
            {
              // start with an empty string in case  message string is missing
              std::string response;
              if (j.contains("Message"))
              {
                response = j["Message"].get<std::string>();
              }
              responses_[id] = std::move(response);
              // clear pending request in case a second response comes in with
              //  the same ID (*cough cough Oxide*)
              requests_.erase(id);
              // std::cout << "WebsocketMessageHandler(): Removed pending request ID=" << id << std::endl;
              // wake up all waiting senders; they will each check the response
              //  map for their respective ID to see if the response is theirs
              cv_.notify_all();
            }
          }
          // else
          // {
          //   std::cout << "Ignoring non-response RCON message: " << msg.str << std::endl;
          // }
        }
        else
        {
          std::cout << "WARNING: Received WebSocket message with no RCON ID" << std::endl;
        }
      }
      break;
      case ix::WebSocketMessageType::Open:
      {
        std::cout << "WebSocket connection established" << std::endl; //: " << msg.openInfo.uri << std::endl;
      }
      break;
      case ix::WebSocketMessageType::Close:
      {
        std::cout << "WebSocket connection terminated: " << msg.closeInfo.reason << std::endl;
      }
      break;
      case ix::WebSocketMessageType::Error:
      {
        std::cout << "WebSocket error: " << msg.errorInfo.reason << std::endl;
        // TODO: does connected_ need to be set to false, or is this only fired
        //  when trying to (re)connect?
      }
      break;
      case ix::WebSocketMessageType::Ping:
      {
        std::cout << "WebSocket PING" << std::endl;
      }
      break;
      case ix::WebSocketMessageType::Pong:
      {
        std::cout << "WebSocket PONG" << std::endl;
      }
      break;
      case ix::WebSocketMessageType::Fragment:
      {
        std::cout << "Received WebSocket fragment" << std::endl;
      }
      break;
    }
  }
}
