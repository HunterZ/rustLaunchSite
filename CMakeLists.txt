cmake_minimum_required(VERSION 3.26.3)

# vcpkg goop
# hard dependency on vcpkg is a concession to wanting to support static linking
#  on windows without hating myself
if(DEFINED ENV{VCPKG_DEFAULT_HOST_TRIPLET})
  set(VCPKG_HOST_TRIPLET $ENV{VCPKG_DEFAULT_HOST_TRIPLET})
endif()
if(DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
  set(VCPKG_TARGET_TRIPLET $ENV{VCPKG_DEFAULT_TRIPLET})
endif()
# set(VCPKG_TRACE_FIND_PACKAGE ON)

project(rustLaunchSite LANGUAGES CXX)

# vcpkg currently offers no working CMake goop for libconfig, so we have to do
#  it the hard way (so much for not hating myself)
# the good news is that it seems to set everything up for find_library() to work
#  properly in all configurations without extra help
find_library(LIBCONFIG_LIB libconfig REQUIRED)
find_library(LIBCONFIG_IMPLIB "liblibconfig.dll.a")
find_library(LIBCONFIGPP_LIB libconfig++ REQUIRED)
find_library(LIBCONFIGPP_IMPLIB "liblibconfig++.dll.a")

# follow vcpkg's lead on dynamic versus static linkage
set(RLS_LIBS
  Boost::boost
  Boost::filesystem
  ctrl-c
  CURL::libcurl
  ixwebsocket::ixwebsocket
  kubazip::kubazip
  libconfig++
  libconfig
  nlohmann_json::nlohmann_json
)

# strip release build binaries
# yes, this is GCC-specific; use MinGW on Windows!
set(RLS_LINK_OPTS
  "$<$<CONFIG:MinSizeRel>:-s>"
  "$<$<CONFIG:Release>:-s>"
  # -Wl,-subsystem,windows
)
set(BOOST_USE_DEBUG_LIBS
  $<$<CONFIG:Debug>:ON>
  $<$<CONFIG:MinSizeRel>:OFF>
  $<$<CONFIG:Release>:OFF>
)
set(BOOST_USE_RELEASE_LIBS
  $<$<CONFIG:Debug>:OFF>
  $<$<CONFIG:MinSizeRel>:ON>
  $<$<CONFIG:Release>:ON>
)
if(VCPKG_TARGET_TRIPLET MATCHES ".*static.*")
  message("RLS: Configuring for static linking")
  set(RLS_COPY_DEPS OFF)
  set(RLS_LINK_OPTS "${RLS_LINK_OPTS}" "-static") # "-static-libgcc" "-static-libstdc++" "-static")
  add_library(libconfig STATIC IMPORTED)
  add_library(libconfig++ STATIC IMPORTED)
  set(RLS_LIBS ${RLS_LIBS} pugixml::static)
  set(Boost_USB_STATIC_LIBS ON)
  set(Boost_USB_STATIC_RUNTIME ON)
else()
  message("RLS: Configuring for dynamic linking")
  set(RLS_COPY_DEPS ON)
  add_library(libconfig SHARED IMPORTED)
  if(DEFINED LIBCONFIG_IMPLIB)
    set_property(TARGET libconfig PROPERTY IMPORTED_IMPLIB "${LIBCONFIG_IMPLIB}")
  endif()
  add_library(libconfig++ SHARED IMPORTED)
  if(DEFINED LIBCONFIGPP_IMPLIB)
    set_property(TARGET libconfig++ PROPERTY IMPORTED_IMPLIB "${LIBCONFIGPP_IMPLIB}")
  endif()
  set(RLS_LIBS ${RLS_LIBS} pugixml::pugixml)
  set(Boost_USB_STATIC_LIBS OFF)
  set(Boost_USB_STATIC_RUNTIME OFF)
endif()
set_property(TARGET libconfig PROPERTY IMPORTED_LOCATION "${LIBCONFIG_LIB}")
set_property(TARGET libconfig++ PROPERTY IMPORTED_LOCATION "${LIBCONFIGPP_LIB}")

# use config mode to prefer vcpkg versions
#  ...except for Boost, because vcpkg uses special goop to work with FindBoost
set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(CURL CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(kubazip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
# find_package(tiny-process-library CONFIG REQUIRED)

# build ctrl-c library
# just use the static version no matter what, since it's a clone-and-own
add_library(ctrl-c STATIC
  ctrl-c/ctrl-c.cpp
  ctrl-c/ctrl-c.h
)

# target for building the game binary
add_executable(${PROJECT_NAME}
  Config.cpp
  Config.h
  Downloader.cpp
  Downloader.h
  main.cpp
  Rcon.cpp
  Rcon.h
  Server.cpp
  Server.h
  Updater.cpp
  Updater.h
)
target_compile_options(${PROJECT_NAME} PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)
target_link_libraries(${PROJECT_NAME} PRIVATE ${RLS_LIBS})
target_link_options(${PROJECT_NAME} PRIVATE "${RLS_LINK_OPTS}")

add_executable(downloaderTest
  Downloader.cpp
  Downloader.h
  downloaderTest.cpp
)
target_link_libraries(downloaderTest PRIVATE ${RLS_LIBS})
target_link_options(downloaderTest PRIVATE "${RLS_LINK_OPTS}")

add_executable(rconTest
  Rcon.cpp
  Rcon.h
  rconTest.cpp
)
target_link_libraries(rconTest PRIVATE ${RLS_LIBS})
target_link_options(rconTest PRIVATE "${RLS_LINK_OPTS}")

add_executable(updaterTest
  Config.cpp
  Config.h
  Downloader.cpp
  Downloader.h
  Updater.cpp
  Updater.h
  updaterTest.cpp
)
target_link_libraries(updaterTest PRIVATE ${RLS_LIBS})
target_link_options(updaterTest PRIVATE "${RLS_LINK_OPTS}")

# install(CODE [[
#   file(GET_RUNTIME_DEPENDENCIES
#     EXECUTABLES $<TARGET_FILE:rustLaunchSite>
#     RESOLVED_DEPENDENCIES_VAR _r_deps
#     UNRESOLVED_DEPENDENCIES_VAR _u_deps
#     DIRECTORIES /mingw64/bin /opt/vcpkg/installed/x64-mingw-dynamic/bin
#     POST_EXCLUDE_REGEXES ${PLATFORM_POST_EXCLUDE_REGEXES}
#   )
#   foreach(_file ${_r_deps})
#     file(INSTALL
#       DESTINATION "${CMAKE_INSTALL_PREFIX}/lib"
#       TYPE SHARED_LIBRARY
#       FOLLOW_SYMLINK_CHAIN
#       FILES "${_file}"
#     )
#   endforeach()
#   list(LENGTH _u_deps _u_length)
#   if("${_u_length}" GREATER 0)
#     message(WARNING "Unresolved dependencies detected: ${_u_deps}")
#   endif()
# ]])

if (RLS_COPY_DEPS)
  add_custom_command(TARGET rustLaunchSite
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:rustLaunchSite> $<TARGET_FILE_DIR:rustLaunchSite>
    COMMAND_EXPAND_LISTS
  )
endif()
