cmake_minimum_required(VERSION 3.28)

project(rustLaunchSite
  LANGUAGES CXX
  VERSION 0.0.1
)

# Sonar is too stupid to ignore missing .modmap files, and we don't need them
#  in this project anyway, so tell CMake not to generate them
set(CMAKE_CXX_SCAN_FOR_MODULES 0)

set(RLS_STATIC ON CACHE BOOL "Use static linking")
if(RLS_STATIC)
  message("RLS: Configuring for static linking")
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    set(RLS_LINK_OPTS "${RLS_LINK_OPTS}" "-static") # "-static-libgcc" "-static-libstdc++" "-static")
  endif()
  set(Boost_USE_STATIC_LIBS ON)
  set(Boost_USE_STATIC_RUNTIME ON)
  set(RLS_COPY_DEPS OFF)
else()
  message("RLS: Configuring for dynamic linking")
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  set(Boost_USE_STATIC_LIBS OFF)
  set(Boost_USE_STATIC_RUNTIME OFF)
  set(RLS_COPY_DEPS ON)
endif()

# additional Boost config
# set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_WARN_NEW_VERSIONS ON)
set(BOOST_USE_DEBUG_LIBS $<CONFIG:Debug>)
set(BOOST_USE_RELEASE_LIBS $<NOT:$<CONFIG:Debug>>)

# use config mode to prefer vcpkg versions
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem process)
find_package(CURL CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(LibArchive REQUIRED) # no config-mode file provided for this one
find_package(nlohmann_json CONFIG REQUIRED)

# fetch dependencies that we're forced to build from source
include(FetchContent)
FetchContent_Declare(ctrl-c
  GIT_REPOSITORY git@github.com:HunterZ/ctrl-c.git
  GIT_TAG        master
)
FetchContent_Declare(SrvLib
  GIT_REPOSITORY git@github.com:HunterZ/SrvLib.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(ctrl-c SrvLib)

set(RLS_COMPILE_FEATURES cxx_std_20)
if(MSVC)
  set(RLS_COMPILE_OPTIONS /W4 /WX)
else()
  set(RLS_COMPILE_OPTIONS -Wall -Wextra -Wpedantic -Werror)
endif()

# build common stuff into an object library
set(RLS_OBJECT_TARGET "${PROJECT_NAME}Obj")
add_library(${RLS_OBJECT_TARGET} OBJECT
  Config.cpp
  Config.h
  Downloader.cpp
  Downloader.h
  Logger.cpp
  Logger.h
  MainCommon.cpp
  MainCommon.h
  Rcon.cpp
  Rcon.h
  Server.cpp
  Server.h
  Updater.cpp
  Updater.h
)
target_compile_features(${RLS_OBJECT_TARGET} PUBLIC ${RLS_COMPILE_FEATURES})
target_compile_options(${RLS_OBJECT_TARGET} PRIVATE ${RLS_COMPILE_OPTIONS})
target_link_libraries(${RLS_OBJECT_TARGET} PRIVATE
  Boost::boost
  Boost::filesystem
  Boost::process
  CURL::libcurl
  ixwebsocket::ixwebsocket
  LibArchive::LibArchive
  nlohmann_json::nlohmann_json
)
target_link_options(${RLS_OBJECT_TARGET} PRIVATE ${RLS_LINK_OPTS})


# build a console binary
add_executable(${PROJECT_NAME} MainConsole.cpp)
target_compile_features(${PROJECT_NAME} PUBLIC ${RLS_COMPILE_FEATURES})
target_compile_options(${PROJECT_NAME} PRIVATE ${RLS_COMPILE_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE
  ${RLS_OBJECT_TARGET}
  ctrl-c
)
target_link_options(${PROJECT_NAME} PRIVATE ${RLS_LINK_OPTS})
add_custom_command(TARGET ${PROJECT_NAME}
  POST_BUILD
  COMMAND "$<$<NOT:$<CONFIG:Debug>>:${CMAKE_STRIP}>"
    ARGS "--strip-all" "$<TARGET_FILE:${PROJECT_NAME}>"
)

# build a service binary
if(WIN32)
  set(RLS_SERVICE_TARGET "${PROJECT_NAME}Svc")
else()
  set(RLS_SERVICE_TARGET "${PROJECT_NAME}d")
endif()
add_executable(${RLS_SERVICE_TARGET} MainService.cpp)
target_compile_features(${RLS_SERVICE_TARGET} PUBLIC ${RLS_COMPILE_FEATURES})
target_compile_options(${RLS_SERVICE_TARGET} PRIVATE ${RLS_COMPILE_OPTIONS})
target_link_libraries(${RLS_SERVICE_TARGET} PRIVATE
  ${RLS_OBJECT_TARGET}
  Boost::boost
  Boost::filesystem
  Boost::process
  srvlib
)
target_link_options(${RLS_SERVICE_TARGET} PRIVATE ${RLS_LINK_OPTS})
add_custom_command(TARGET ${RLS_SERVICE_TARGET}
  POST_BUILD
  COMMAND "$<$<NOT:$<CONFIG:Debug>>:${CMAKE_STRIP}>"
    ARGS "--strip-all" "$<TARGET_FILE:${RLS_SERVICE_TARGET}>"
)

include(GNUInstallDirs)
# include(InstallRequiredSystemLibraries)

# install(CODE [[
#   file(GET_RUNTIME_DEPENDENCIES
#     EXECUTABLES $<TARGET_FILE:${PROJECT_NAME}>
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
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND_EXPAND_LISTS
  )
  add_custom_command(TARGET ${RLS_SERVICE_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:${RLS_SERVICE_TARGET}> $<TARGET_FILE_DIR:${RLS_SERVICE_TARGET}>
    COMMAND_EXPAND_LISTS
  )
endif()

install(TARGETS
  ${PROJECT_NAME}
  ${RLS_SERVICE_TARGET}
)
install(
  FILES exampleConfig.jsonc
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
    set (BUILD_ID "${BUILD_ID}-${VCPKG_TARGET_TRIPLET}-$<CONFIG>")
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
