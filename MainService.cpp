#include "MainCommon.h"

#include "Service.h"

int main(int argc, char* argv[])
{
  SrvParam svParam;
#if defined(_WIN32) || defined(_WIN64)
  svParam.szDspName = L"Rust Launch Site";               // Servicename in Service control manager of windows
  svParam.szDescribe = L"Rust dedicated server manager"; // Description in Service control manager of windows
#endif
  svParam.szSrvName = L"rustLaunchSite";                 // Service name (service id)
  svParam.fnStartCallBack = [&]()
  {   // Start you server here
    rustLaunchSite::Start(argc, argv);
  };
  svParam.fnStopCallBack = []()
  {   // Stop you server here
    rustLaunchSite::Stop();
  };
  svParam.fnSignalCallBack = []()
  {   // what ever you do with this callback, maybe reload the configuration
  };

  return ServiceMain(argc, argv, svParam);
}
