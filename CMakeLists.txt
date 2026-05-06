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

# ── Plugin shared library (opt-out) ────────────────────────────────────────
# CI's tests-only job skips this with -DBUILD_PLUGIN=OFF so it doesn't need
# the AWS SDK installed. Default ON for normal builds.
option(BUILD_PLUGIN "Build the OrthancAwsSqs plugin shared library" ON)

if(BUILD_PLUGIN)
    # Requires AWS SDK >= 1.11. On Debian/Ubuntu install the
    # libaws-cpp-sdk-{core,sqs,s3} packages (or build from source — see README).
    find_package(AWSSDK REQUIRED COMPONENTS sqs s3)

    add_library(OrthancAwsSqs SHARED
        src/Plugin.cpp
        src/Configuration.cpp
        src/Logging.cpp
        src/SqsPoller.cpp
        src/S3Downloader.cpp
        src/S3EventParser.cpp
    )

    target_include_directories(OrthancAwsSqs PRIVATE
        ${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}
        src
    )

    target_link_libraries(OrthancAwsSqs PRIVATE
        ${AWSSDK_LINK_LIBRARIES}
    )
    # pthread is already in AWSSDK_LINK_LIBRARIES on Linux/macOS — don't add
    # it again to avoid the linker's "ignoring duplicate libraries" warning.

    target_compile_definitions(OrthancAwsSqs PRIVATE
        ORTHANC_AWS_SQS_VERSION="${PROJECT_VERSION}"
    )

    # Final library naming: the shared object should NOT have a "lib" prefix
    # on macOS (Orthanc on macOS expects ".dylib" without lib-prefix). On
    # Linux the conventional "libOrthancAwsSqs.so" is fine.
    if(APPLE)
        set_target_properties(OrthancAwsSqs PROPERTIES PREFIX "" SUFFIX ".dylib")
    elseif(UNIX)
        # Linux release bundles place AWS SDK shared libraries beside the plugin.
        # Let the dynamic loader resolve those adjacent dependencies without
        # requiring LD_LIBRARY_PATH or copying them into /usr/local/lib.
        set_target_properties(OrthancAwsSqs PROPERTIES
            BUILD_RPATH "\$ORIGIN"
            INSTALL_RPATH "\$ORIGIN"
        )
    endif()

    include(GNUInstallDirs)
    install(TARGETS OrthancAwsSqs LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(OrthancAwsSqs PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
            -Wno-unused-parameter
        )
    endif()
endif()

message(STATUS "Build configuration:")
message(STATUS "  CXX standard         : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type           : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Orthanc SDK include  : ${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}")
message(STATUS "  Build plugin         : ${BUILD_PLUGIN}")
if(BUILD_PLUGIN)
    message(STATUS "  AWS SDK link libs    : ${AWSSDK_LINK_LIBRARIES}")
endif()

# ── Tests (opt-in) ─────────────────────────────────────────────────────────
# Configure with `-DBUILD_TESTING=ON` to build the unit tests. Tests fetch
# GoogleTest via FetchContent, so the regular plugin build stays dep-light.
option(BUILD_TESTING   "Build the OrthancAwsSqs unit tests" OFF)
# `-DENABLE_COVERAGE=ON` adds gcov instrumentation to the test binary so
# `lcov`/`gcov` can produce a line-coverage report. Implies tests; CI uses it.
option(ENABLE_COVERAGE "Instrument the test binary with --coverage"  OFF)
if(BUILD_TESTING OR ENABLE_COVERAGE)
    enable_testing()
    add_subdirectory(tests)
endif()
