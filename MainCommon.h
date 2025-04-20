#ifndef MAINCOMMON_H
#define MAINCOMMON_H

namespace rustLaunchSite
{
// exit codes
// TODO: use standard codes instead?
//  see https://en.cppreference.com/w/cpp/error/errc
enum RLS_EXIT
{
  SUCCESS = 0,
  ARG,      // invalid_argument
  HANDLER,  // io_error / state_not_recoverable
  START,    // no_child_process
  UPDATE,   // no_child_process
  RESTART,  // no_child_process
  EXCEPTION // interrupted
};

int Start(int argc, char* argv[]);
void Stop();
}

#endif
