#[===================================================================[
   read version from source
#]===================================================================]

file (STRINGS src/xbwd/app/BuildInfo.cpp BUILD_INFO)
foreach (line_ ${BUILD_INFO})
  if (line_ MATCHES "versionString[ ]*=[ ]*\"(.+)\"")
    set (xbwd_version ${CMAKE_MATCH_1})
  endif ()
endforeach ()
if (xbwd_version)
  message (STATUS "XBridgeWitness version: ${xbwd_version}")
else ()
  message (FATAL_ERROR "unable to determine XBridgeWitness version")
endif ()
