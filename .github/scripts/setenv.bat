@echo off
echo Setup Chocolatey PATH
set PATH=%ChocolateyInstall%\bin;%PATH%
echo Updated PATH: %PATH%

@REM echo Setup vcpkg root
@REM set VCPKG_ROOT=%GITHUB_WORKSPACE%\vcpkg
