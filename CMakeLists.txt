cmake_minimum_required(VERSION 3.16)
project(OrthancAwsSqs LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Use -fvisibility=hidden by default; only the Orthanc plugin entry points
# need to be exported.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(cmake/OrthancPluginSDK.cmake)

# ── AWS SDK for C++ ────────────────────────────────────────────────────────
# Requires AWS SDK >= 1.11. On Debian/Ubuntu install the
# libaws-cpp-sdk-{core,sqs,s3} packages (or build from source — see README).
find_package(AWSSDK REQUIRED COMPONENTS sqs s3)

# ── Plugin shared library ──────────────────────────────────────────────────
add_library(OrthancAwsSqs SHARED
    src/Plugin.cpp
    src/Configuration.cpp
    src/Logging.cpp
    src/SqsPoller.cpp
    src/S3Downloader.cpp
)

target_include_directories(OrthancAwsSqs PRIVATE
    ${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}
    src
)

target_link_libraries(OrthancAwsSqs PRIVATE
    ${AWSSDK_LINK_LIBRARIES}
    pthread
)

target_compile_definitions(OrthancAwsSqs PRIVATE
    ORTHANC_AWS_SQS_VERSION="${PROJECT_VERSION}"
)

# Final library naming: the shared object should NOT have a "lib" prefix
# on macOS (Orthanc on macOS expects ".dylib" without lib-prefix). On
# Linux the conventional "libOrthancAwsSqs.so" is fine.
if(APPLE)
    set_target_properties(OrthancAwsSqs PROPERTIES PREFIX "" SUFFIX ".dylib")
endif()

# ── Install rules ──────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS OrthancAwsSqs LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

# ── Strict warnings (catch issues early) ───────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(OrthancAwsSqs PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
        -Wno-unused-parameter
    )
endif()

message(STATUS "Build configuration:")
message(STATUS "  CXX standard         : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type           : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Orthanc SDK include  : ${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}")
message(STATUS "  AWS SDK link libs    : ${AWSSDK_LINK_LIBRARIES}")
