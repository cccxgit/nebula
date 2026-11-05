# In-process Tantivy (Rust) and its dependency-free C++ adapter.
find_program(NEBULA_CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin")
if(NEBULA_CARGO_EXECUTABLE)
    set(TANTIVY_PREBUILT_DEFAULT OFF)
else()
    set(TANTIVY_PREBUILT_DEFAULT ON)
endif()
option(NEBULA_TANTIVY_USE_PREBUILT
       "Use the bundled, checksum-verified Linux x86_64 Rust archive" ${TANTIVY_PREBUILT_DEFAULT})

set(TANTIVY_RUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust")
set(TANTIVY_TARGET_DIR "${CMAKE_BINARY_DIR}/tantivy-rust-target")
set(TANTIVY_ARCHIVE "${TANTIVY_TARGET_DIR}/release/libnebula_tantivy.a")
if(NEBULA_TANTIVY_USE_PREBUILT)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        message(FATAL_ERROR "Bundled Tantivy archive supports Linux x86_64 only; build Rust on the target platform")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(TANTIVY_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt")
    set(TANTIVY_PREBUILT_MANIFEST "${TANTIVY_PREBUILT_DIR}/manifest.json")
    set(TANTIVY_PREBUILT_BUNDLE "${TANTIVY_PREBUILT_DIR}/libnebula_tantivy-linux-x86_64-glibc2.35.a.gz")
    set(TANTIVY_PREBUILT_HELPER "${TANTIVY_PREBUILT_DIR}/extract.py")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${TANTIVY_PREBUILT_HELPER}" check "${TANTIVY_PREBUILT_MANIFEST}"
        RESULT_VARIABLE TANTIVY_HOST_CHECK
        OUTPUT_VARIABLE TANTIVY_HOST_MESSAGE
        ERROR_VARIABLE TANTIVY_HOST_ERROR
    )
    if(NOT TANTIVY_HOST_CHECK EQUAL 0)
        message(FATAL_ERROR "Bundled Tantivy archive incompatible with this host: ${TANTIVY_HOST_ERROR}")
    endif()
    add_custom_command(
        OUTPUT "${TANTIVY_ARCHIVE}"
        COMMAND "${Python3_EXECUTABLE}" "${TANTIVY_PREBUILT_HELPER}"
            extract "${TANTIVY_PREBUILT_MANIFEST}" "${TANTIVY_PREBUILT_BUNDLE}" "${TANTIVY_ARCHIVE}"
        DEPENDS "${TANTIVY_PREBUILT_MANIFEST}" "${TANTIVY_PREBUILT_BUNDLE}" "${TANTIVY_PREBUILT_HELPER}"
                "${TANTIVY_RUST_DIR}/Cargo.toml" "${TANTIVY_RUST_DIR}/Cargo.lock"
                "${TANTIVY_RUST_DIR}/src/lib.rs" "${TANTIVY_RUST_DIR}/src/query_compat.rs"
                "${TANTIVY_RUST_DIR}/vendor/include-flate/Cargo.toml"
                "${TANTIVY_RUST_DIR}/vendor/include-flate/src/lib.rs"
        COMMENT "Verifying and extracting bundled Tantivy archive (Rust not required)"
        VERBATIM
    )
    message(STATUS "Tantivy: using bundled Rust archive (${TANTIVY_HOST_MESSAGE})")
else()
    if(NOT NEBULA_CARGO_EXECUTABLE)
        message(FATAL_ERROR "Tantivy source build requires cargo; use -DNEBULA_TANTIVY_USE_PREBUILT=ON on a compatible Linux x86_64 host")
    endif()
    add_custom_command(
        OUTPUT "${TANTIVY_ARCHIVE}"
        COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_BUILD_JOBS=1"
            "CARGO_TARGET_DIR=${TANTIVY_TARGET_DIR}"
            "RUSTFLAGS=-C relocation-model=pic"
            ${NEBULA_CARGO_EXECUTABLE} build --release --locked
            --manifest-path "${TANTIVY_RUST_DIR}/Cargo.toml"
        DEPENDS "${TANTIVY_RUST_DIR}/Cargo.toml"
                "${TANTIVY_RUST_DIR}/Cargo.lock"
                "${TANTIVY_RUST_DIR}/src/lib.rs"
                "${TANTIVY_RUST_DIR}/src/query_compat.rs"
                "${TANTIVY_RUST_DIR}/vendor/include-flate/Cargo.toml"
                "${TANTIVY_RUST_DIR}/vendor/include-flate/src/lib.rs"
        WORKING_DIRECTORY "${TANTIVY_RUST_DIR}"
        COMMENT "Building embedded Tantivy search engine (one Rust job maximum)"
        VERBATIM
    )
    message(STATUS "Tantivy: building Rust source with Cargo")
endif()
add_custom_target(nebula_tantivy_build DEPENDS "${TANTIVY_ARCHIVE}")
add_library(nebula_tantivy_rust STATIC IMPORTED GLOBAL)
set_target_properties(nebula_tantivy_rust PROPERTIES IMPORTED_LOCATION "${TANTIVY_ARCHIVE}")
add_dependencies(nebula_tantivy_rust nebula_tantivy_build)

add_library(tantivy_adapter STATIC TantivyIndex.cpp)
target_include_directories(tantivy_adapter PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(tantivy_adapter PUBLIC nebula_tantivy_rust pthread dl m rt)
add_dependencies(tantivy_adapter nebula_tantivy_build)

if(ENABLE_TESTING)
    add_executable(tantivy_bridge_test tests/TantivyBridgeTest.cpp)
    target_link_libraries(tantivy_bridge_test PRIVATE tantivy_adapter)
    add_test(NAME tantivy_bridge_test COMMAND tantivy_bridge_test)
endif()
