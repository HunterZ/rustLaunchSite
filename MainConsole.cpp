#include "Logger.h"
#include "MainCommon.h"
#include "ctrl-c.h"
#include <memory>

namespace
{
/// CtrlCLibrary callback handler
///
/// This is mainly just a shunt to RLS Stop()
bool HandleCtrlC(CtrlCLibrary::CtrlSignal s)
{
  if (s != CtrlCLibrary::kCtrlCSignal)
  {
    return false;
  }
  rustLaunchSite::Stop();
  return true;
}

/// Alias CtrlCLibrary's handle ID type here, to reduce maintenance burden in
///  the case that it ever changes
using CtrlCHandleT = unsigned int;

/// Lambda for use as unique_ptr custom deleter to clean up CtrlCLibrary handler
auto DeleteCtrlCHandle = [](CtrlCHandleT* handlePtr)
{
  if (!handlePtr) return;
  CtrlCLibrary::ResetCtrlCHandler(*handlePtr);
  delete handlePtr; // NOSONAR
};

/// Return a unique_ptr to a CtrlCLibrary handler ID, where the unique_ptr is
///  also an RAII wrapper for the handler (i.e. the handler is automatically
///  cleaned up when the unique_ptr goes out of scope)
std::unique_ptr<CtrlCHandleT, decltype(DeleteCtrlCHandle)> MakeCtrlCHandle()
{
  return
  { // NOSONAR
    new CtrlCHandleT{CtrlCLibrary::SetCtrlCHandler(HandleCtrlC)},
    DeleteCtrlCHandle
  };
}
}

/// Main entry point for console flavor
int main(int argc, char *argv[])
{
  // allocate logger on the stack so that it auto-destructs at end of main()
  // also, this is a stdout / std::cout logger since this is the console flavor
  rustLaunchSite::Logger logger{};

  // attempt to install Ctrl+C handler
  if (
    auto ctrlCHandle{MakeCtrlCHandle()}; CtrlCLibrary::kErrorID == *ctrlCHandle)
  {
    LOG_ERROR(logger, "Failed to install Ctrl+C handler");
    return rustLaunchSite::RLS_EXIT_HANDLER;
  }

  // start RLS common main, and return result on exit
  return rustLaunchSite::Start(logger, argc, argv);
}
