cmake_minimum_required(VERSION 3.26.3)

# vcpkg goop
# hard dependency on vcpkg is a concession to wanting to support static linking
#  on windows without hating myself
if(NOT DEFINED VCPKG_HOST_TRIPLET AND DEFINED ENV{VCPKG_DEFAULT_HOST_TRIPLET})
  set(VCPKG_HOST_TRIPLET $ENV{VCPKG_DEFAULT_HOST_TRIPLET})
  message("RLS: Set VCPKG_HOST_TRIPLET=${VCPKG_HOST_TRIPLET} from environment")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET AND DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
  set(VCPKG_TARGET_TRIPLET $ENV{VCPKG_DEFAULT_TRIPLET})
  message("RLS: Set VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET} from environment")
endif()
# set(VCPKG_TRACE_FIND_PACKAGE ON)

project(rustLaunchSite
  LANGUAGES CXX
  VERSION 0.0.1
)

# follow vcpkg's lead on dynamic versus static linkage
set(RLS_LIBS
  Boost::boost
  Boost::filesystem
  ctrl-c
  CURL::libcurl
  ixwebsocket::ixwebsocket
  kubazip::kubazip
  libconfig::config++
  libconfig::config
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
  set(Boost_USE_STATIC_LIBS ON)
  set(Boost_USE_STATIC_RUNTIME ON)
else()
  message("RLS: Configuring for dynamic linking")
  set(RLS_COPY_DEPS ON)
  set(Boost_USE_STATIC_LIBS OFF)
  set(Boost_USE_STATIC_RUNTIME OFF)
endif()

# use config mode to prefer vcpkg versions
#  ...except for Boost, because vcpkg uses special goop to work with FindBoost
# set(Boost_NO_BOOST_CMAKE ON)
# set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(CURL CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(kubazip CONFIG REQUIRED)
find_package(libconfig CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# build ctrl-c library (as static for simplicity)
add_library(ctrl-c STATIC
  ctrl-c/src/ctrl-c.cpp
  ctrl-c/src/ctrl-c.h
)
target_include_directories(ctrl-c INTERFACE "${CMAKE_SOURCE_DIR}/ctrl-c/src")

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

# add_executable(downloaderTest
#   Downloader.cpp
#   Downloader.h
#   downloaderTest.cpp
# )
# target_link_libraries(downloaderTest PRIVATE ${RLS_LIBS})
# target_link_options(downloaderTest PRIVATE "${RLS_LINK_OPTS}")

# add_executable(rconTest
#   Rcon.cpp
#   Rcon.h
#   rconTest.cpp
# )
# target_link_libraries(rconTest PRIVATE ${RLS_LIBS})
# target_link_options(rconTest PRIVATE "${RLS_LINK_OPTS}")

# add_executable(updaterTest
#   Config.cpp
#   Config.h
#   Downloader.cpp
#   Downloader.h
#   Updater.cpp
#   Updater.h
#   updaterTest.cpp
# )
# target_link_libraries(updaterTest PRIVATE ${RLS_LIBS})
# target_link_options(updaterTest PRIVATE "${RLS_LINK_OPTS}")

include(GNUInstallDirs)

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

if(RLS_COPY_DEPS)
  add_custom_command(TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:rustLaunchSite> $<TARGET_FILE_DIR:rustLaunchSite>
    COMMAND_EXPAND_LISTS
  )
endif()

install(TARGETS ${PROJECT_NAME})
install(
  FILES example.cfg
  TYPE DATA
)
install(
  FILES LICENSE README.md
  TYPE DOC
)

# generate a fake build ID if we weren't given one by the environment (e.g. from a CI/CD workflow/pipeline)
if(NOT DEFINED BUILD_ID)
  set (BUILD_ID "LOCAL")
  if(DEFINED VCPKG_TARGET_TRIPLET)
    set (BUILD_ID "${BUILD_ID}-${VCPKG_TARGET_TRIPLET}")
  endif()
  message("RLS: Synthesized build ID: ${BUILD_ID}")
else()
  message("RLS: Using external build ID: ${BUILD_ID}")
endif()

# set package variables
# NOTE: CMake uses project name/version for packaging, so we don't need to re-specify those
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Rust dedicated server manager")
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-${BUILD_ID}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "C:\\Games\\${PROJECT_NAME}")
set(CPACK_PACKAGE_RELOCATABLE ON)
set(CPACK_PACKAGE_VENDOR "HunterZ")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_STRIP_FILES ON)
set(CPACK_VERBATIM_VARIABLES ON)
include(CPack)
