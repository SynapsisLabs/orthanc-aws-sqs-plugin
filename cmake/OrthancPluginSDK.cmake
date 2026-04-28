# Locate (or download) the Orthanc Plugin SDK headers.
#
# Order of preference:
#   1. ORTHANC_PLUGIN_SDK_INCLUDE_DIR provided on the command line
#      (e.g. -DORTHANC_PLUGIN_SDK_INCLUDE_DIR=/path/to/orthanc/Plugins/Include)
#   2. System install at /usr/include or /usr/local/include
#   3. Fetch the SDK header into third_party/orthanc-sdk/
#
# The Orthanc plugin SDK is a single C header — no build step required.

set(_sdk_include "")

# 1. Honour an explicit override
if(ORTHANC_PLUGIN_SDK_INCLUDE_DIR
   AND NOT "${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}" MATCHES "NOTFOUND$"
   AND EXISTS "${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}/orthanc/OrthancCPlugin.h")
    set(_sdk_include "${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}")
endif()

# 2. Try a system install. Use a private cache var so a NOTFOUND result
#    doesn't poison ORTHANC_PLUGIN_SDK_INCLUDE_DIR for the next step.
if(NOT _sdk_include)
    find_path(_orthanc_system_sdk_include
        NAMES orthanc/OrthancCPlugin.h
        PATHS /usr/include /usr/local/include /opt/homebrew/include
    )
    if(_orthanc_system_sdk_include
       AND NOT "${_orthanc_system_sdk_include}" MATCHES "NOTFOUND$"
       AND EXISTS "${_orthanc_system_sdk_include}/orthanc/OrthancCPlugin.h")
        set(_sdk_include "${_orthanc_system_sdk_include}")
    endif()
endif()

# 3. Fetch the header from the Orthanc Hg repo
if(NOT _sdk_include)
    set(_ORTHANC_SDK_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/orthanc-sdk")
    set(_ORTHANC_SDK_HEADER "${_ORTHANC_SDK_DIR}/orthanc/OrthancCPlugin.h")
    set(_ORTHANC_SDK_URL    "https://orthanc.uclouvain.be/hg/orthanc/raw-file/default/OrthancServer/Plugins/Include/orthanc/OrthancCPlugin.h")

    if(NOT EXISTS "${_ORTHANC_SDK_HEADER}")
        message(STATUS "Fetching Orthanc Plugin SDK header from ${_ORTHANC_SDK_URL}")
        file(MAKE_DIRECTORY "${_ORTHANC_SDK_DIR}/orthanc")
        file(DOWNLOAD
            "${_ORTHANC_SDK_URL}"
            "${_ORTHANC_SDK_HEADER}"
            STATUS  _dl_status
            TLS_VERIFY ON
        )
        list(GET _dl_status 0 _dl_code)
        list(GET _dl_status 1 _dl_msg)
        if(NOT _dl_code EQUAL 0)
            file(REMOVE "${_ORTHANC_SDK_HEADER}")
            message(FATAL_ERROR
                "Failed to download Orthanc Plugin SDK header.\n"
                "  URL    : ${_ORTHANC_SDK_URL}\n"
                "  status : ${_dl_code} -- ${_dl_msg}\n"
                "  Either install the orthanc-dev system package, or pass\n"
                "  -DORTHANC_PLUGIN_SDK_INCLUDE_DIR=/path/to/headers to cmake."
            )
        endif()
    endif()
    set(_sdk_include "${_ORTHANC_SDK_DIR}")
endif()

if(NOT EXISTS "${_sdk_include}/orthanc/OrthancCPlugin.h")
    message(FATAL_ERROR
        "Could not locate orthanc/OrthancCPlugin.h under: ${_sdk_include}")
endif()

# Publish as a cache var. FORCE so we overwrite any stale NOTFOUND.
set(ORTHANC_PLUGIN_SDK_INCLUDE_DIR "${_sdk_include}" CACHE PATH
    "Path to the Orthanc plugin SDK headers" FORCE)
message(STATUS "Orthanc Plugin SDK include: ${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}")
