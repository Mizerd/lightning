# DeepFilterNet (libDF vendored in third_party/deepfilternet, see its
# PROVENANCE.md), compiled into the Rust staticlib by cargo as the
# `deepfilternet` feature of rust/Cargo.toml (default ON) and called from C++
# through rust/include/matrix_denoise.h.
#
# Nothing here reaches the network: no FetchContent, no submodule, no
# download of source or of the model. libDF is a cargo PATH dependency, its
# crates.io dependencies are pinned in rust/Cargo.lock and fetched by the same
# locked step as every other crate, and the model is embedded with
# include_bytes! from the tarball in the tree.
#
# Include this file BEFORE the Rust block (it sets LIGHTNING_CARGO_FEATURE_ARGS,
# which the cargo command passes) and before the noise-suppression block
# (which links lightning_noise_deepfilternet when the target exists).
#
# Provides:
#   LIGHTNING_ENABLE_DEEPFILTERNET   option, default ON; effective only with
#                                    ENABLE_RUST_SDK_BACKEND=ON
#   LIGHTNING_CARGO_FEATURE_ARGS     extra cargo arguments
#                                    (--no-default-features when OFF)
#   lightning_noise_deepfilternet    STATIC C++ library:
#                                    calls::noise::DeepFilterSuppressor.
#                                    Linking it defines HAVE_DEEPFILTERNET=1
#                                    for the consumer when DeepFilterNet is in.
#   lightning_add_deepfilternet_tests()  registers the unit test (call inside
#                                    BUILD_TESTING after Qt6::Test is found)

option(LIGHTNING_ENABLE_DEEPFILTERNET
    "Build DeepFilterNet noise suppression into the Rust backend (third_party/deepfilternet)" ON)

set(LIGHTNING_CARGO_FEATURE_ARGS "")
set(_dfn_enabled OFF)
if(ENABLE_RUST_SDK_BACKEND AND LIGHTNING_ENABLE_DEEPFILTERNET)
    set(_dfn_enabled ON)
elseif(ENABLE_RUST_SDK_BACKEND)
    # The crate's only default feature is `deepfilternet`.
    set(LIGHTNING_CARGO_FEATURE_ARGS --no-default-features)
endif()

# Files cargo reads outside rust/src, so editing them re-runs cargo.
set(LIGHTNING_DEEPFILTERNET_CARGO_INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deepfilternet/libDF/Cargo.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deepfilternet/libDF/src/lib.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deepfilternet/libDF/src/tract.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deepfilternet/models/DeepFilterNet3_onnx.tar.gz"
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/include/matrix_denoise.h")

# The C++ wrapper. Always defined so a consumer can link it unconditionally;
# without DeepFilterNet it builds the stub (createDeepFilterSuppressor() ==
# nullptr, deepFilterAvailable() == false).
add_library(lightning_noise_deepfilternet STATIC
    src/calls/noise/DeepFilterSuppressor.h
    src/calls/noise/DeepFilterSuppressor.cpp)
set_target_properties(lightning_noise_deepfilternet PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(lightning_noise_deepfilternet
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/calls/noise"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rust/include")
if(_dfn_enabled)
    # matrix-client-rust and matrix-client-rust-build are defined further down
    # in CMakeLists.txt; both are resolved at generate time.
    target_link_libraries(lightning_noise_deepfilternet PUBLIC matrix-client-rust)
    if(UNIX AND NOT APPLE)
        target_link_libraries(lightning_noise_deepfilternet PUBLIC dl pthread m)
    endif()
    add_dependencies(lightning_noise_deepfilternet matrix-client-rust-build)
    target_compile_definitions(lightning_noise_deepfilternet PUBLIC HAVE_DEEPFILTERNET=1)
    message(STATUS "DeepFilterNet: vendored third_party/deepfilternet (libDF @ d375b2d8, DeepFilterNet3 model embedded)")
elseif(ENABLE_RUST_SDK_BACKEND)
    message(STATUS "DeepFilterNet: disabled (LIGHTNING_ENABLE_DEEPFILTERNET=OFF)")
else()
    message(STATUS "DeepFilterNet: unavailable (needs ENABLE_RUST_SDK_BACKEND=ON)")
endif()

function(lightning_add_deepfilternet_tests)
    qt_add_executable(deepfilter-suppressor-test
        tests/DeepFilterSuppressorTest.cpp)
    target_link_libraries(deepfilter-suppressor-test PRIVATE
        lightning_noise_deepfilternet Qt6::Core Qt6::Test)
    add_test(NAME deepfilter-suppressor COMMAND deepfilter-suppressor-test)
endfunction()
