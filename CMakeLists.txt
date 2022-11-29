cmake_minimum_required (VERSION 3.11)
project (xbridge_witnessd)

set(CONAN_PATHS_1 ${CMAKE_BINARY_DIR}/conan_paths.cmake)
set(CONAN_PATHS_2 ${CMAKE_BINARY_DIR}/conan-dependencies/conan_paths.cmake)

if(EXISTS "${CONAN_PATHS_1}")
    include(${CONAN_PATHS_1})
elseif(EXISTS "${CONAN_PATHS_2}")
    include(${CONAN_PATHS_2})
else()
    message(FATAL_ERROR  " conan_paths.cmake not found")
endif()

set(CMAKE_CXX_STANDARD 20)
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

add_compile_definitions(
  BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT
  BOOST_CONTAINER_FWD_BAD_DEQUE
  HAS_UNCAUGHT_EXCEPTIONS=1
)

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
      set (rippled_tag "xbridge" CACHE STRING
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

# this one is a string and therefore can't be an option
set (san "" CACHE STRING "On gcc & clang, add sanitizer instrumentation")
set_property (CACHE san PROPERTY STRINGS ";undefined;memory;address;thread")
if (san)
  string (TOLOWER ${san} san)
  set (SAN_FLAG "-fsanitize=${san}")
  set (SAN_LIB "")
  if (is_gcc)
    if (san STREQUAL "address")
      set (SAN_LIB "asan")
    elseif (san STREQUAL "thread")
      set (SAN_LIB "tsan")
    elseif (san STREQUAL "memory")
      set (SAN_LIB "msan")
    elseif (san STREQUAL "undefined")
      set (SAN_LIB "ubsan")
    endif ()
  endif ()
  set (_saved_CRL ${CMAKE_REQUIRED_LIBRARIES})
  set (CMAKE_REQUIRED_LIBRARIES "${SAN_FLAG};${SAN_LIB}")
  check_cxx_compiler_flag (${SAN_FLAG} COMPILER_SUPPORTS_SAN)
  set (CMAKE_REQUIRED_LIBRARIES ${_saved_CRL})
  if (NOT COMPILER_SUPPORTS_SAN)
    message (FATAL_ERROR "${san} sanitizer does not seem to be supported by your compiler")
  endif ()
endif ()

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
  src/xbwd/client/RpcResultParse.cpp
  )
target_include_directories (xbridge_witnessd PRIVATE src)
target_link_libraries (xbridge_witnessd PUBLIC Ripple::xrpl_core XBridgeWitness::opts
  SOCI::soci_core_static SOCI::soci_sqlite3_static fmt::fmt)

if (san)
  target_compile_options (xbridge_witnessd
    INTERFACE
      # sanitizers recommend minimum of -O1 for reasonable performance
      $<$<CONFIG:Debug>:-O1>
      ${SAN_FLAG}
      -fno-omit-frame-pointer)
  target_compile_definitions (xbridge_witnessd
    INTERFACE
      $<$<STREQUAL:${san},address>:SANITIZER=ASAN>
      $<$<STREQUAL:${san},thread>:SANITIZER=TSAN>
      $<$<STREQUAL:${san},memory>:SANITIZER=MSAN>
      $<$<STREQUAL:${san},undefined>:SANITIZER=UBSAN>)
  target_link_libraries (xbridge_witnessd INTERFACE ${SAN_FLAG} ${SAN_LIB})
endif ()

if (has_parent)
  set_target_properties (xbridge_witnessd PROPERTIES EXCLUDE_FROM_ALL ON)
  set_target_properties (xbridge_witnessd PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD ON)
endif ()

#fix for MAC
get_target_property(FMT_INC_DIRS fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
list (GET FMT_INC_DIRS 0 FMT_INC_DIR)
target_include_directories(xbridge_witnessd BEFORE PRIVATE ${FMT_INC_DIR})
