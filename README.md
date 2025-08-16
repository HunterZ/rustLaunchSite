# rustLaunchSite
Rust dedicated server manager

## Introduction & Features
RLS is a C++ application whose aim is to act as a step-up from simple shell scripts that are commonly used to keep a self-hosted Rust dedicated server running. To this end, it currently provides the following major features:
- Automatic relaunch of RustDedicated server process on unexpected exit
- Clean shutdown of RustDedicated server process via RCON `quit` command on commanded stop
- Monitoring for and automatic installation of RustDedicated server application and/or Carbon/Oxide plugin framework updates, including clean server shutdown and relaunch
- Delayed shutdown with user notices when players are online
- Some aspects of server configuration automatically derived from RLS configuration

Basically I want RLS to automate a lot of the backend maintenance drudgery of running a self-hosted server, so that I have more time to engage in community and possibly even play myself.

RLS supports both console and system service flavors on both Windows and Linux.

## Usage & Runtime Dependencies
RLS currently comes in two flavors: A console binary and a service binary.

### Common Dependencies
A RustDedicated server instlalation (optionally with Carbon or Oxide plugin framework) must exist prior to using RLS.

SteamCMD install location must be discoverable via PATH or your RLS config file.

RLS must be run with elevated permissions ("Run As Administrator" on Windows) because SteamCMD seems to silently fail without it. On Linux, it is recommended that RLS be run via a user account that owns both RustDedicated and rustLaunchSite installs.

### Console RLS Usage
The console flavor of RLS currently logs to the console and is non-interactive. Output can be redirected to a file via the OS.

This requires a single command line parameter: A path to an RLS configuration file. An example file (`examples/exampleConfig.jsonc`) is included, which is heavily commented to help you figure things out.

A clean shutdown of both the service and RLS can be triggered by issuing a Ctrl+C keypress combo.

### Windows Dependencies
Powershell is invoked to inspect Carbon/Oxide DLL versions. This must be discoverable via the `PATH` environment variable of the account under which RLS is run.

### Windows RLS Service Usage
The Windows service flavor of RLS is able to self-install. Run `rustLaunchSiteSvc -h` for info.

Nightly restarts can be triggered via a Task Scheduler job that restarts the service.

### Linux RLS Dependencies
`monodis` is invoked to inspect Carbon/Oxide DLL versions. This must be discoverable via the `PATH` environment variable of the account under which RLS is run. Your OS package manager can generally install this via a `mono-utils` or similar package.

### Linux RLS Service Usage
The Linux flavor of RLS includes a `rustLaunchSited` binary that can be configured to run as a systemd service. See `examples/systemd` or `README.systemd.md` for more information.

## Roadmap
I have a lot of ideas for improving RLS. See the Issues section of the project, as I plan to capture my thoughts there.

I should also mention that some features mentioned in `examples/exampleConfig.json` have not been implemented yet - most notably wipe automation.

## Building & Library Dependencies
RLS is written in C++ and was developed in Visual Studio Code using CMake, ninja-build, MSYS2 MinGW x64 / MSVC / Linux GCC, and vcpkg. I use static linking by default; dynamic linking _should_ be possible, but has not been tested/maintained. A vcpkg manifest and CMake preset are provided, which assume that the environment variable `VCPKG_ROOT` is defined.

RLS currently has the following FOSS library dependencies via vcpkg:
- Boost (dll, filesystem, process, property-tree)
- ixwebsocket
- libarchive
- libcurl
- nlohmann_json

...And the following FOSS library dependencies via GitHub:
- https://github.com/evgenykislov/ctrl-c
- https://github.com/Tomenz/SrvLib

MSVC is recommended for bulding on Windows, although MSYS2 MinGW GCC is also supported. Building with MSVC requires installing Visual Studio BuildTools and then launching vscode from an x64 Native Tools Command Prompt to setup the build environment properly.

GitHub Actions support has also been implemented to provide automated server-side Linux, MinGW, and MSVC builds, mainly as a sanity check.

## Contributing
Contributions are welcome. Feel free to open issues and/or pull requests. I cannot guarantee that I will act on these, however, so you also have my blessing to maintain your own fork (although I'd certainly appreciate credit for my contributions) or possibly become a co-owner.

## License
RLS is a hobby project, I chose the permissive MIT license to encourage usage and contributions. See the `LICENSE` file for details, including links to FOSS dependency licenses.
