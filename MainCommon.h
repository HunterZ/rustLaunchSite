#ifndef MAINCOMMON_H
#define MAINCOMMON_H

namespace rustLaunchSite
{
class Logger;

// TODO: use standard codes instead?
//  see https://en.cppreference.com/w/cpp/error/errc
// [[maybe_unused]] is because vscode clangd seems too stupid to detect usage

[[maybe_unused]] constexpr int RLS_EXIT_SUCCESS   = 0; ///< successful exit
[[maybe_unused]] constexpr int RLS_EXIT_ARG       = 1; ///< invalid argument
[[maybe_unused]] constexpr int RLS_EXIT_HANDLER   = 2; ///< signal handler fail
[[maybe_unused]] constexpr int RLS_EXIT_START     = 3; ///< child process error
[[maybe_unused]] constexpr int RLS_EXIT_UPDATE    = 4; ///< child process error
[[maybe_unused]] constexpr int RLS_EXIT_RESTART   = 5; ///< child process error
[[maybe_unused]] constexpr int RLS_EXIT_EXCEPTION = 6; ///< interrupted
[[maybe_unused]] constexpr int RLS_EXIT_THREAD    = 7; ///< thread create failed

/// Common RLS application entrypoint
int Start(Logger& logger, int argc, char* argv[]);

/// Request an orderly shutdown
///
/// This method is thread-safe
void Stop();
}

#endif
