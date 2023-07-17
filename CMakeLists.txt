cmake_minimum_required (VERSION 3.20)
project (xbridge_witnessd)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11))
  message(FATAL_ERROR "Use gcc >= 11.0")
elseif((CMAKE_CXX_COMPILER_ID STREQUAL "Clang") AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13))
  message(FATAL_ERROR "Use clang >= 13.0")
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR "${CMAKE_TOOLCHAIN_FILE} " STREQUAL " ")
  set(conan_toolchain ${CMAKE_CURRENT_BINARY_DIR}/build/generators/conan_toolchain.cmake)
  if(NOT EXISTS "${conan_toolchain}")
    message(FATAL_ERROR  "${conan_toolchain} not found")
  endif()
  include(${conan_toolchain})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_VERBOSE_MAKEFILE 1)



#[===========================================[
  The tool depends on the xrpl_core
  library which is defined by the rippled
  project. This looks for installed rippled
  libs and, if not found, pulls them in with
  FetchContent.
#]===========================================]
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Builds/CMake")

#include(deps/Boost)
find_package(SOCI REQUIRED)
find_package(fmt REQUIRED)
find_package(date REQUIRED)
find_package(OpenSSL REQUIRED)

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
    include(deps/FindRipple)
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

        set (conan_build_type "Release")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
          set (conan_build_type "Debug")
        endif()

        set (conan_profile "default")
        if(NOT "${CONAN_PROFILE} " STREQUAL " ")
          set (conan_profile ${CONAN_PROFILE})
        endif()

        execute_process (COMMAND conan install ${rippled_src_SOURCE_DIR} --build missing --output-folder . -s build_type=${conan_build_type} -pr=${conan_profile}
                       WORKING_DIRECTORY ${rippled_src_BINARY_DIR}
                       RESULT_VARIABLE _git_exit_code
                       OUTPUT_STRIP_TRAILING_WHITESPACE
                       ECHO_OUTPUT_VARIABLE
                       ECHO_ERROR_VARIABLE
                       )
        if (NOT _git_exit_code EQUAL 0)
          message (WARNING "conan exit_code: ${_git_exit_code}\n conan command failed - build might be incorrect")
        endif ()

        option (tests "Build tests" OFF)
        add_subdirectory(${rippled_src_SOURCE_DIR} ${rippled_src_BINARY_DIR})
      endif()
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

set(HEADERS
  src/xbwd/app/App.h
  src/xbwd/app/BuildInfo.h
  src/xbwd/app/Config.h
  src/xbwd/app/DBInit.h
  src/xbwd/basics/ChainTypes.h
  src/xbwd/basics/StructuredLog.h
  src/xbwd/basics/ThreadSaftyAnalysis.h
  src/xbwd/client/WebsocketClient.h
  src/xbwd/client/ChainListener.h
  src/xbwd/client/RpcResultParse.h
  src/xbwd/core/DatabaseCon.h
  src/xbwd/core/SociDB.h
  src/xbwd/federator/Federator.h
  src/xbwd/federator/FederatorEvents.cpp
  src/xbwd/federator/TxnSupport.h
  src/xbwd/rpc/fromJSON.h
  src/xbwd/rpc/RPCCall.h
  src/xbwd/rpc/RPCClient.h
  src/xbwd/rpc/RPCHandler.h
  src/xbwd/rpc/ServerHandler.h
)

set(SOURCES
  src/xbwd/app/App.cpp
  src/xbwd/app/BuildInfo.cpp
  src/xbwd/app/Config.cpp
  src/xbwd/app/DBInit.cpp
  src/xbwd/app/main.cpp
  src/xbwd/client/WebsocketClient.cpp
  src/xbwd/client/ChainListener.cpp
  src/xbwd/client/RpcResultParse.cpp
  src/xbwd/core/DatabaseCon.cpp
  src/xbwd/core/SociDB.cpp
  src/xbwd/federator/Federator.cpp
  src/xbwd/federator/FederatorEvents.cpp
  src/xbwd/rpc/RPCClient.cpp
  src/xbwd/rpc/RPCHandler.cpp
  src/xbwd/rpc/ServerHandler.cpp
)

add_executable (${PROJECT_NAME}
  ${SOURCES}
  ${HEADERS}
)

target_compile_options(${PROJECT_NAME} PUBLIC "-fstack-protector-strong")
if (is_linux)
  target_link_options(${PROJECT_NAME} PUBLIC "LINKER:-z,relro,-z,now")
endif()
target_include_directories (${PROJECT_NAME} PRIVATE src ${date_INCLUDE_DIR})
target_link_libraries (${PROJECT_NAME} PUBLIC Ripple::xrpl_core XBridgeWitness::opts
  SOCI::soci_core_static SOCI::soci_sqlite3_static fmt::fmt OpenSSL::Crypto OpenSSL::SSL)

if (san)
  target_compile_options (${PROJECT_NAME}
    INTERFACE
      # sanitizers recommend minimum of -O1 for reasonable performance
      $<$<CONFIG:Debug>:-O1>
      ${SAN_FLAG}
      -fno-omit-frame-pointer)
  target_compile_definitions (${PROJECT_NAME}
    INTERFACE
      $<$<STREQUAL:${san},address>:SANITIZER=ASAN>
      $<$<STREQUAL:${san},thread>:SANITIZER=TSAN>
      $<$<STREQUAL:${san},memory>:SANITIZER=MSAN>
      $<$<STREQUAL:${san},undefined>:SANITIZER=UBSAN>)
  target_link_libraries (${PROJECT_NAME} INTERFACE ${SAN_FLAG} ${SAN_LIB})
endif ()

if (has_parent)
  set_target_properties (${PROJECT_NAME} PROPERTIES EXCLUDE_FROM_ALL ON)
  set_target_properties (${PROJECT_NAME} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD ON)
endif ()

#fix for MAC
get_target_property(FMT_INC_DIRS fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
list (GET FMT_INC_DIRS 0 FMT_INC_DIR)
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE ${FMT_INC_DIR})
