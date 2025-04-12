# rustLaunchSite
Rust dedicated server manager

## Introduction & Features
RLS is a C++ application whose aim is to act as a step-up from simple shell scripts that are commonly used to keep a self-hosted Rust dedicated server running. To this end, it currently provides the following major features:
- Automatic relaunch of server application
- Monitoring for and automatica installation of RustDedicated server application and/or Carbon/Oxide plugin framework updates, including clean server shutdown and relaunch
- Delayed shutdown with user notices when players are online
- Some aspects of server configuration automatically derived from RLS configuration

Basically I want RLS to automate a lot of the backend maintenance drudgery of running a self-hosted server, so that I have more time to engage in community and possibly even play myself.

RLS is currently only supported on Windows, but I've tried to use cross-platform tools as much as possible in hopes of minimizing porting friction.

## Usage & Runtime Dependencies
Rust dedicated server (optionally with Carbon or Oxide plugin framework) must be installed via SteamCMD prior to using RLS, and SteamCMD must still be available via the same path from which it was run to perform the install.

RLS must be run with elevated permissions ("Run As Administrator" on Windows) because SteamCMD seems to silently fail without it.

RLS supports being run as a service (e.g. via NSSM/WinSW on Windows), as it attemps an orderly server and application shutdown on receipt of Ctrl+C. This also means clean nightly restarts can be triggered via an OS task scheduler job that restarts the service.

RLS requires a single command line parameter: A path to an RLS configuration file. An example file (`exampleConfig.jsonc`) is included, which is heavily commented to help you figure things out.

RLS currently only logs to the standard console output. This can be redirected to a file.

To check Carbon/Oxide DLL versions, RLS invokes Powershell on Windows, or monodis on Linux. The latter can be found in a mono-utils or similar package in your distro's package manager. The appropriate utility must be resolvable from the PATH environment variable of the account under which RLS is executed.

## Roadmap
I have a lot of ideas for improving RLS. See the Issues section of the project, as I plan to capture my thoughts there.

I should also mention that some features mentioned in `example.cfg` have not been implemented yet - most notably wipe automation.

## Building & Library Dependencies
RLS is written in C++ and was developed in Visual Studio Code using CMake, MSYS2 MinGW x64, and vcpkg. I use static linking to minimize deployment size and complexity, but dynamic linking _should_ be possible. A vcpkg manifest and CMake preset are provided, which assume that the environment variable VCPKG_ROOT is defined.

RLS currently has the following FOSS library dependencies without modifications, all of which are available via vcpkg or MSYS2 except for Ctrl+C which was sourced from GitHub: https://github.com/evgenykislov/ctrl-c
- Boost (filesystem, process, property-tree)
- Ctrl+C
- ixwebsocket
- kubazip
- libcurl
- nlohmann_json

RLS has also been made to work with MSVC via vscode. This requires installing Visual Studio BuildTools and then launching vscode from an x64 Native Tools Command Prompt to setup the build environment properly.

Preliminary GitHub Actions support has also been implemented to provide automated server-side MinGW and MSVC builds.

## Contributing
Contributions are welcome. Feel free to open issues and/or pull requests. I cannot guarantee that I will act on these, however, so you also have my blessing to maintain your own fork (although I'd certainly appreciate credit for my contributions) or possibly become a co-owner.

## License
RLS is a hobby project, I chose the permissive MIT license to encourage usage and contributions. See the `LICENSE` file for details, including links to FOSS dependency licenses.
