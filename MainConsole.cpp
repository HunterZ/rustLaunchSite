#include "MainCommon.h"
#include "ctrl-c.h"
#include <iostream>

namespace
{
bool HandleCtrlC(CtrlCLibrary::CtrlSignal s)
{
  if (s != CtrlCLibrary::kCtrlCSignal)
  {
    std::cout << "rustLaunchSite: WARNING: Ignoring unknown signal" << std::endl;
    return false;
  }
  rustLaunchSite::Stop();
  return true;
}
}

int main(int argc, char *argv[])
{
  // install Ctrl+C handler
  // TODO: change this to an RAII wrapper so that it cleans itself up at the end
  const auto handlerId{CtrlCLibrary::SetCtrlCHandler(HandleCtrlC)};
  if (handlerId == CtrlCLibrary::kErrorID)
  {
    std::cout << "rustLaunchSite: ERROR: Failed to install Ctrl+C handler" << std::endl;
    return rustLaunchSite::RLS_EXIT::HANDLER;
  }

  const auto retVal{rustLaunchSite::Start(argc, argv)};

  CtrlCLibrary::ResetCtrlCHandler(handlerId);

  return retVal;
}
