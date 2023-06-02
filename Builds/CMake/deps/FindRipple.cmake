find_path(Ripple_SRC
  NAMES src/ripple/protocol/STXChainBridge.h
  PATHS ${RIPPLE_SRC_DIR}
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)
if(Ripple_SRC)
  set(Ripple_INCLUDE_DIR ${Ripple_SRC}/src)
endif()

find_library(Ripple_LIBRARY
  NAMES xrpl_core
  PATHS ${RIPPLE_BIN_DIR}
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

if(Ripple_SRC AND Ripple_LIBRARY)
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Ripple DEFAULT_MSG
    Ripple_INCLUDE_DIR Ripple_LIBRARY)
  if(Ripple_FOUND)
    add_library(Ripple::xrpl_core STATIC IMPORTED)
    set_target_properties(Ripple::xrpl_core PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${Ripple_INCLUDE_DIR}"
      IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
      IMPORTED_LOCATION "${Ripple_LIBRARY}")
    target_link_libraries (Ripple::xrpl_core INTERFACE
      ${RIPPLE_BIN_DIR}/src/ed25519-donna/libed25519.a
      ${RIPPLE_BIN_DIR}/src/secp256k1/libsecp256k1.a)
  endif()
endif()

if(Ripple_FOUND)
  message("Looking for RIPPLED ... found")
else()
  message("Looking for RIPPLED ... NOT found")
endif()
