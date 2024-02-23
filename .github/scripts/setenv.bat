@echo off
echo Setup Chocolatey PATH
set PATH=%ChocolateyInstall%\bin;%PATH%
echo Updated PATH: %PATH%

echo Setup vcpkg root
set VCPKG_ROOT=%GITHUB_WORKSPACE%\vcpkg
