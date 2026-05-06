# Unit tests for the pure (non-AWS, non-Orthanc-RPC) parts of the plugin.
#
# Build with:
#   cmake -S . -B build -DBUILD_TESTING=ON
#   cmake --build build --target unit_tests
#   ctest --test-dir build --output-on-failure

include(FetchContent)

# Pin to a known-good GoogleTest release. Bump as needed.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
# Don't override parent's compiler/linker settings on Windows.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# nlohmann/json — pulled in transitively by the plugin's normal build via the
# AWS SDK include dirs, but the test target doesn't link AWSSDK. Find it
# directly here. The package is installed via `nlohmann-json3-dev` on
# Debian/Ubuntu and `nlohmann-json` on Homebrew.
find_package(nlohmann_json REQUIRED)

add_executable(unit_tests
    configuration_test.cpp
    s3_event_parser_test.cpp
    # The units under test, plus their (logging) shim. Logging.cpp only calls
    # OrthancPluginLogXxx when SetContext() has been called — tests never call
    # it, so log output falls back to stderr without dlsym'ing into Orthanc.
    ${CMAKE_SOURCE_DIR}/src/Configuration.cpp
    ${CMAKE_SOURCE_DIR}/src/Logging.cpp
    ${CMAKE_SOURCE_DIR}/src/S3EventParser.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${ORTHANC_PLUGIN_SDK_INCLUDE_DIR}
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(unit_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
    )
endif()

# Coverage instrumentation (gcov-format). Applies only to the test binary
# and the project sources compiled into it — third-party deps and gtest are
# unaffected because we set this on the unit_tests target, not globally.
if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE requires GCC or Clang")
    endif()
    target_compile_options(unit_tests PRIVATE --coverage -O0 -g)
    target_link_options(unit_tests    PRIVATE --coverage)
endif()

include(GoogleTest)
gtest_discover_tests(unit_tests)
