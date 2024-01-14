#include "Rcon.h"

#include <chrono>
#include <iostream>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>

// internal type/data definitions
namespace
{
struct IdEngine
{
  std::random_device rd_{};
  std::mt19937 mt_{rd_()};
  std::uniform_int_distribution<int32_t> dist_{INT32_MIN, INT32_MAX};

  int32_t Get()
  {
    int32_t val{dist_(mt_)};
    // if we got zero, roll again until we get something else
    while (!val) { val = dist_(mt_); }
    return val;
  }
};

// SONAR TODO: promote this to a public class, or merge with Rcon?
// fanciest approach would be to define an API, make this an implementation,
//  and have Rcon take a shared_ptr as a constructor parameter, as this would
//  allow mock implementations to be injected for unit test purposes etc.
IdEngine ID_ENGINE;
}

namespace rustLaunchSite
{
// opaque type definitions

// initialization handle
//
// this is an empty type used for managing init/deinit via RAII
struct RconInitHandle
{
  std::size_t count_ = 0;

  virtual ~RconInitHandle() = default;
};

// these are simple wrappers around the underlying websocket library's types
//  in order to avoid leaking header-level dependencies
struct WebSocket
{
  ix::WebSocket webSocket_;

  virtual ~WebSocket() = default;

  ix::WebSocket& operator*() { return webSocket_; }

  ix::WebSocket* operator->() { return &webSocket_; }
};

struct WebSocketMessage
{
  const ix::WebSocketMessagePtr& webSocketMessagePtr_;

  //NOSONAR - this is IXWebSocket API
  explicit WebSocketMessage(const ix::WebSocketMessagePtr& msgPtr)
    : webSocketMessagePtr_(msgPtr) {}

  virtual ~WebSocketMessage() = default;

  const ix::WebSocketMessage& operator*() const
  {
    return *webSocketMessagePtr_;
  }

  ix::WebSocketMessage* operator->() const
  {
    return webSocketMessagePtr_.get();
  }
};

// static member init

std::weak_ptr<RconInitHandle> Rcon::rconInitHandleWptr_ = {};
std::mutex Rcon::rconInitHandleMutex_ = {};

// public member methods

Rcon::Rcon(
  const std::string& hostOrIp, const int port, const std::string& password,
  const bool logMessages
)
  : logMessages_(logMessages)
{
  // do this here, because otherwise Sonar badgers me to use in-class
  //  initializers, which isn't possible with opaque types
  webSocketUptr_ = std::make_unique<WebSocket>();
  if (!webSocketUptr_)
  {
    throw std::runtime_error("Failed to create underlying WebSocket handle");
  }
  auto& webSocket(**webSocketUptr_);
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
    [this](const ix::WebSocketMessagePtr& msgPtr)
    {
      this->WebsocketMessageHandler(WebSocketMessage(msgPtr));
    }
  );

  webSocket.start();
  std::cout << "Started WebSocket RCON connection to server: " << hostOrIp << std::endl;
}

Rcon::~Rcon()
{
  if (webSocketUptr_) { (*webSocketUptr_)->stop(); }
}

bool Rcon::IsConnected() const
{
  return (
    webSocketUptr_ &&
    (*webSocketUptr_)->getReadyState() == ix::ReadyState::Open)
  ;
}

void Rcon::Register(MessageHandler handler)
{
  // handler is passed by value, so move that temporary into the container
  messageHandlers_.push_back(std::move(handler));
}

std::string Rcon::SendCommand(
  const std::string& command,
  const std::size_t timeoutMilliseconds
)
{
  if (!IsConnected())
  {
    std::cout << "Ignoring RCON command due to no connection" << std::endl;
    return {};
  }

  // unique_lock is used here instead of scoped_lock so that condition
  //  variable wait can unlock it
  std::unique_lock lock(mutex_);

  // allocate a request ID if a timeout was specified
  const REQUEST_ID_TYPE identifier{timeoutMilliseconds ? ID_ENGINE.Get() : 0};

  // Rust RCON message format is JSON with a simple list of 3 parameters:
  // - 'Identifier' set to a number, which will be used in the response
  // - 'Message' set to the command to be executed
  // - 'Name' set to WebRcon
  nlohmann::json j;
  std::stringstream s;
  j["Identifier"] = identifier;
  j["Message"] = command;
  j["Name"] = "WebRcon";

  // record pending request if we have an ID
  if (identifier)
  {
    // std::cout << "Queued request for RCON response to command ID=" << identifier << std::endl;
    requests_.insert(identifier);
  }
  // else
  // {
  //   std::cout << "Not requesting response for RCON comamnd ID=" << identifier << std::endl;
  // }

  const auto& result((*webSocketUptr_)->send(j.dump()));
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
    return {};
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
    std::cout << "WARNING: Response wait timed out for RCON command (id=" << identifier << "): " << command << std::endl;
    return {};
  }

  // move the response string into a local variable so that it doesn't get
  //  invalidated when we erase it from the map
  const std::string response(std::move(responseIter->second));
  responses_.erase(responseIter);
  return response;
}

// private methods

// get init handle, creating one if necessary (which in turn performs init)
// if weak pointer is valid, then there is an Rcon instance holding a shared
//  pointer to an active init handle and we can just return a copy of that;
//  otherwise create a new one, capture it in the weak pointer, and return it
std::shared_ptr<RconInitHandle> Rcon::GetInitHandle()
{
  // lock mutex to prevent concurrent creation of multiple handles
  std::scoped_lock lock(rconInitHandleMutex_);
  // attempt to upgrade weak pointer to shared pointer
  auto handleSptr(rconInitHandleWptr_.lock());
  if (!handleSptr)
  {
    // create a static dummy object to associate with the shared pointer
    static RconInitHandle RCON_INIT_HANDLE;
    // count number of inits that have occurred
    ++RCON_INIT_HANDLE.count_;
    // the handle is just a dummy type - what we really care about is attaching
    //  a custom deleter to perform uninit when the last copy of the shared_ptr
    //  goes away
    rconInitHandleWptr_ = handleSptr = std::shared_ptr<RconInitHandle>(
      // dummy object (have to use new instead of make_shared because of custom
      //  deleter)
      &RCON_INIT_HANDLE,
      // custom deleter lambda
      [](RconInitHandle const* handlePtr)
      {
        ix::uninitNetSystem();
        std::cout << "Uninitialized WebSocket library (destroyed handle #" << handlePtr->count_ << ")" << std::endl;
      }
    );

    // perform the actual init
    ix::initNetSystem();
    std::cout << "Initialized WebSocket library (created handle #" << RCON_INIT_HANDLE.count_ << ")" << std::endl;
  }
  // return new or existing handle
  return handleSptr;
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

  const auto& msg(*message);
  switch (msg.type)
  {
    case ix::WebSocketMessageType::Message:
    {
      if (logMessages_)
      {
        std::cout << "Processing received RCON message: " << msg.str << std::endl;
      }
      const nlohmann::json j(nlohmann::json::parse(msg.str));
      // first thing to do is look for an expected response
      // id = 0 is a broadcast, while id != 0 is a response
      // ...except if it's a chat message, as those always have id=-1 for some
      //  reason?
      const bool isChat
      {
        j.contains("Type") && j["Type"].get<std::string>() == "Chat"
      };
      if (!j.contains("Identifier"))
      {
        std::cout << "WARNING: Received WebSocket message with no RCON ID: " << msg.str << std::endl;
        return;
      }
      if (const auto id(j["Identifier"].get<REQUEST_ID_TYPE>()); id && !isChat)
      {
        if (!requests_.count(id))
        {
          // we can now log this, because we use id=0 when not expecting a
          //  response
          std::cout << "WARNING: Ignoring RCON response with unknown ID=" << id << ": " << msg.str << std::endl;
          return;
        }
        // start with an empty string in case message string is missing
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
        return;
      }
      // broadcast message - report to any registered handlers
      for (const auto& handler : messageHandlers_)
      {
        handler(msg.str);
      }
      return;
    }
    break;
    case ix::WebSocketMessageType::Open:
    {
      std::cout << "WebSocket connection established" << std::endl;
      return;
    }
    break;
    case ix::WebSocketMessageType::Close:
    {
      std::cout << "WebSocket connection terminated: " << msg.closeInfo.reason << std::endl;
      return;
    }
    break;
    case ix::WebSocketMessageType::Error:
    {
      std::cout
        << "WebSocket error (ReadyState="
        << (webSocketUptr_ ? static_cast<int>((*webSocketUptr_)->getReadyState()) : -1)
        << "): " << msg.errorInfo.reason << std::endl;
      // TODO: does connected_ need to be set to false, or is this only fired
      //  when trying to (re)connect?
      return;
    }
    break;
    case ix::WebSocketMessageType::Ping:
    {
      std::cout << "WebSocket PING" << std::endl;
      return;
    }
    break;
    case ix::WebSocketMessageType::Pong:
    {
      std::cout << "WebSocket PONG" << std::endl;
      return;
    }
    break;
    case ix::WebSocketMessageType::Fragment:
    {
      std::cout << "Received WebSocket fragment" << std::endl;
      return;
    }
    break;
  }
  std::cout << "WARNING: Unknown WebSocket message type: " << static_cast<int>(msg.type) << std::endl;
}
}
