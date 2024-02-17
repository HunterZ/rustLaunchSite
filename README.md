# rustLaunchSite
Rust dedicated server manager

## Introduction & Features
RLS is a C++ application whose aim is to act as a step-up from simple shell scripts that are commonly used to keep a self-hosted Rust dedicated server running. To this end, it currently provides the following major features:
- Automatic restart of server application
- Delayed shutdown with user notices when players are online
- Automatic monitoring for and installation of server software and/or Carbon/Oxide plugin framework updates, including server shutdown and restart
- Server configuration derived from RLS configuration to avoid redundancy

Basically I want RLS to automate a lot of the backend maintenance drudgery of running a self-hosted server, so that I have more time to engage in community and possibly even play myself.

RLS is currently only supported on Windows, but I've tried to use cross-platform tools as much as possible in hopes of minimizing porting friction.

## Usage & Runtime Dependencies
Rust dedicated server (optionally with Carbon or Oxide plugin framework) must be installed via SteamCMD prior to using RLS, and SteamCMD must still be available via the same path from which it was run to perform the install.

RLS must be run with elevated permissions ("Run As Administrator" on Windows) because SteamCMD seems to silently fail without it.

RLS should support being run as a service (e.g. via NSSM), but this has not yet been tested. If it receives a Ctrl+C, it will attempt an orderly server and application shutdown.

RLS requires a single command line parameter: A path to an RLS configuration file. An example file (`example.cfg`) is included, which is heavily commented to help you figure things out.

RLS currently only logs to the standard console output. This can be redirected to a file.

## Roadmap
I have a lot of ideas for improving RLS. See the Issues section of the project, as I plan to capture my thoughts there.

I should mention that some features mentioned in `example.cfg` have not been implemented yet - most notably wipe automation.

## Building & Library Dependencies
RLS is written in C++ and was developed in VS Code using CMake, MSYS2 MinGW x64, and vcpkg. I use static linking to minimize deployment size and complexity, but dynamic linking _should_ be possible.

RLS currently has the following FOSS library dependencies without modifications, all of which are available via vcpkg or MSYS2 except for Ctrl+C which was sourced from GitHub: https://github.com/evgenykislov/ctrl-c
- Boost (Filesystem, Process)
- Ctrl+C
- ixwebsocket
- kubazip
- libcurl
- libconfig
- nlohmann_json

## Contributing
Contributions are welcome. Feel free to open issues and/or pull requests. I cannot guarantee that I will act on these, however, so you also have my blessing to maintain your own fork (although I'd certainly appreciate credit for my contributions) or possibly become a co-owner.

## License
RLS is a hobby project, I chose the permissive MIT license to encourage usage and contributions. See the `LICENSE` file for details, including links to FOSS dependency licenses.
