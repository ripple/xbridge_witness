cmake_minimum_required (VERSION 3.11)
project (xbridge_witnessd)

include(${CMAKE_BINARY_DIR}/conan_paths.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_VERBOSE_MAKEFILE 1)

find_package(SOCI REQUIRED)
find_package(fmt REQUIRED)

#[===========================================[
  The tool depends on the xrpl_core
  library which is defined by the rippled
  project. This looks for installed rippled
  libs and, if not found, pulls them in with
  FetchContent.
#]===========================================]
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Builds/CMake")
include(XBridgeWitnessNIH)

get_directory_property(has_parent PARENT_DIRECTORY)

if(coverage)
  # Rippled also responds to the "coverage" flag, so clear it if it's set until
  # the dependency is set up.
  set(xbridge_witness_coverage ${coverage})
  set(coverage OFF CACHE BOOL "gcc/clang only" FORCE)
endif()

if (NOT has_parent)
  find_package(Ripple QUIET)
  if (NOT TARGET Ripple::xrpl_core)
    find_package(Git)

    if (NOT GIT_FOUND)
      message (FATAL_ERROR "git is required to determine branch name")
    endif ()

    execute_process (COMMAND ${GIT_EXECUTABLE} "rev-parse" "--abbrev-ref" "HEAD"
                     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                     RESULT_VARIABLE _git_exit_code
                     OUTPUT_VARIABLE _branch
                     OUTPUT_STRIP_TRAILING_WHITESPACE
                     ERROR_QUIET)
    if (NOT _git_exit_code EQUAL 0)
      message (WARNING "git command failed - deduced branch might be incorrect")
    endif ()

    # rippled_tag is cache string and can be overriden when configuring
    # with -Drippled_tag=commit_or_tag in order to pick a specific
    # rippled version to download. Default tag is develop/master/release as 
    # determined by the branch of this project
    if (NOT (_branch STREQUAL "master" OR _branch STREQUAL "release"))
      set (rippled_tag "attest" CACHE STRING
        "tag/commit of rippled to fetch from if a local install is not found")
    else ()
      set (rippled_tag "${_branch}" CACHE STRING
        "tag/commit of rippled to fetch from if a local install is not found")
    endif ()
    message(STATUS "Installed rippled not found... \
      using local copy from tag/commit [${rippled_tag}]")
    include (FetchContent)
    FetchContent_Declare(
      rippled_src
      GIT_REPOSITORY https://github.com/seelabs/rippled.git
      GIT_TAG ${rippled_tag}
    )
    FetchContent_GetProperties(rippled_src)
    if(NOT rippled_src_POPULATED)
      message (STATUS "Pausing to download rippled source...")
      FetchContent_Populate(rippled_src)
      add_subdirectory(${rippled_src_SOURCE_DIR} ${rippled_src_BINARY_DIR})
    endif()
  endif ()
endif ()

if(xbridge_witness_coverage)
  set(coverage ${xbridge_witness_coverage} CACHE BOOL "gcc/clang only" FORCE)
endif()

include(XBridgeWitnessSanity)
include(XBridgeWitnessCov)
include(XBridgeWitnessInterface)

add_executable (xbridge_witnessd
  src/xbwd/app/App.cpp
  src/xbwd/app/BuildInfo.cpp
  src/xbwd/app/Config.cpp
  src/xbwd/app/DBInit.cpp
  src/xbwd/app/main.cpp
  src/xbwd/core/DatabaseCon.cpp
  src/xbwd/core/SociDB.cpp
  src/xbwd/federator/Federator.cpp
  src/xbwd/federator/FederatorEvents.cpp
  src/xbwd/rpc/RPCHandler.cpp
  src/xbwd/rpc/ServerHandler.cpp
  src/xbwd/client/WebsocketClient.cpp
  src/xbwd/client/ChainListener.cpp
  )
target_include_directories (xbridge_witnessd PRIVATE src)
target_link_libraries (xbridge_witnessd Ripple::xrpl_core XBridgeWitness::opts
  SOCI::soci_core_static SOCI::soci_sqlite3_static fmt::fmt)

if (has_parent)
  set_target_properties (xbridge_witnessd PROPERTIES EXCLUDE_FROM_ALL ON)
  set_target_properties (xbridge_witnessd PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD ON)
endif ()



