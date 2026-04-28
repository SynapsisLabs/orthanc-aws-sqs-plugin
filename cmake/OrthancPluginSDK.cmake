# Locate (or download) the Orthanc Plugin SDK headers.
#
# Order of preference:
#   1. ORTHANC_PLUGIN_SDK_INCLUDE_DIR provided on the command line
#      (e.g. -DORTHANC_PLUGIN_SDK_INCLUDE_DIR=/path/to/orthanc/Plugins/Include)
#   2. System install at /usr/include/orthanc
#   3. Fetch a known release of the SDK headers from the upstream repo
#      into third_party/orthanc-sdk/
#
# The Orthanc plugin SDK is a small set of C headers — no build step required.

if(NOT ORTHANC_PLUGIN_SDK_INCLUDE_DIR)
    # Prefer system install if available
    find_path(ORTHANC_PLUGIN_SDK_INCLUDE_DIR
        NAMES orthanc/OrthancCPlugin.h
        PATHS /usr/include /usr/local/include
    )
endif()

if(NOT ORTHANC_PLUGIN_SDK_INCLUDE_DIR)
    # Fall back to fetching a pinned version of the headers
    set(_ORTHANC_SDK_VERSION "1.12.5")
    set(_ORTHANC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/orthanc-sdk")
    set(_ORTHANC_SDK_HEADER "${_ORTHANC_SDK_DIR}/orthanc/OrthancCPlugin.h")

    if(NOT EXISTS "${_ORTHANC_SDK_HEADER}")
        message(STATUS "Fetching Orthanc Plugin SDK ${_ORTHANC_SDK_VERSION}")
        file(MAKE_DIRECTORY "${_ORTHANC_SDK_DIR}/orthanc")
        file(DOWNLOAD
            "https://orthanc.uclouvain.be/hg/orthanc/raw-file/default/OrthancServer/Plugins/Include/orthanc/OrthancCPlugin.h"
            "${_ORTHANC_SDK_HEADER}"
            STATUS _dl_status
            TLS_VERIFY ON
        )
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            message(FATAL_ERROR
                "Failed to download Orthanc Plugin SDK header.\n"
                "Set -DORTHANC_PLUGIN_SDK_INCLUDE_DIR=/path/to/orthanc/Plugins/Include "
                "or install the orthanc-dev package and rerun cmake."
            )
        endif()
    endif()

    set(ORTHANC_PLUGIN_SDK_INCLUDE_DIR "${_ORTHANC_SDK_DIR}" CACHE PATH
        "Path to the Orthanc plugin SDK headers")
endif()

if(NOT EXISTS "${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}/orthanc/OrthancCPlugin.h")
    message(FATAL_ERROR
        "Could not find orthanc/OrthancCPlugin.h under "
        "ORTHANC_PLUGIN_SDK_INCLUDE_DIR=${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}"
    )
endif()

message(STATUS "Orthanc Plugin SDK include: ${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}")
