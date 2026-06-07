# Compile the test runner together with the helper source under test. The
# helpers are pure C with no ESP-IDF HTTP/TLS dependency, so they build cleanly
# for the `linux` target; only Unity is required.
idf_component_register(
    SRCS
        "test_ota_version.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../main/ota_version.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../main/runtime_policy.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../../main"
    REQUIRES unity
)
