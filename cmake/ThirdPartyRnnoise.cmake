# RNNoise (vendored in third_party/rnnoise, see its PROVENANCE.md).
#
# Nothing here reaches the network: no FetchContent, no submodule, no download
# of source or of the model. The weights are the generated C file in the tree.
#
# Provides:
#   LIGHTNING_ENABLE_RNNOISE            option, default ON
#   lightning_rnnoise                   STATIC C library (see PROVENANCE.md for local fixes)
#   lightning_noise_rnnoise             STATIC C++ library: calls::noise::RnnoiseSuppressor.
#                                       Linking it defines HAVE_RNNOISE=1 for the
#                                       consumer when RNNoise is enabled.
#   lightning_add_rnnoise_tests()       registers the unit test (call inside
#                                       BUILD_TESTING after Qt6::Test is found)

option(LIGHTNING_ENABLE_RNNOISE
    "Build the vendored RNNoise noise suppressor (third_party/rnnoise)" ON)

set(_rn_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise")

if(LIGHTNING_ENABLE_RNNOISE)
    # The top-level project is CXX only; RNNoise is C99.
    enable_language(C)

    set(_rn_src "${_rn_dir}/src")
    add_library(lightning_rnnoise STATIC
        ${_rn_src}/denoise.c
        ${_rn_src}/rnn.c
        ${_rn_src}/pitch.c
        ${_rn_src}/kiss_fft.c
        ${_rn_src}/celt_lpc.c
        ${_rn_src}/nnet.c
        ${_rn_src}/nnet_default.c
        ${_rn_src}/parse_lpcnet_weights.c
        ${_rn_src}/rnnoise_tables.c
        ${_rn_src}/rnnoise_data.c)
    set_target_properties(lightning_rnnoise PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(lightning_rnnoise
        PUBLIC "${_rn_dir}/include"
        PRIVATE "${_rn_src}")
    # RNNOISE_BUILD only affects symbol visibility of the (unused) shared build.
    target_compile_definitions(lightning_rnnoise PRIVATE RNNOISE_BUILD)

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        # Real-time code: optimise even in Debug (an unoptimised network is
        # an order of magnitude slower and would xrun a debug call).
        # Third-party: its warnings are not ours to chase.
        target_compile_options(lightning_rnnoise PRIVATE
            -w "$<$<CONFIG:Debug>:-O2>")
    elseif(MSVC)
        target_compile_options(lightning_rnnoise PRIVATE /w)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(lightning_rnnoise PUBLIC m)
    endif()

    # x86: runtime CPU dispatch between the plain, SSE4.1 and AVX2 kernels, so
    # one binary is portable AND fast (upstream's own recommendation). Without
    # it a baseline x86-64 build gets the SSE2 kernel only, and the network
    # costs several times more CPU on any AVX2 machine.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86|i[3-6]86)$"
            AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        target_sources(lightning_rnnoise PRIVATE
            ${_rn_src}/x86/x86cpu.c
            ${_rn_src}/x86/x86_dnn_map.c
            ${_rn_src}/x86/nnet_sse4_1.c
            ${_rn_src}/x86/nnet_avx2.c)
        target_compile_definitions(lightning_rnnoise PRIVATE RNN_ENABLE_X86_RTCD)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            # cpuid through inline asm (what upstream's configure selects).
            target_compile_definitions(lightning_rnnoise PRIVATE CPU_INFO_BY_ASM)
            set_source_files_properties(${_rn_src}/x86/nnet_sse4_1.c
                PROPERTIES COMPILE_OPTIONS "-msse4.1")
            set_source_files_properties(${_rn_src}/x86/nnet_avx2.c
                PROPERTIES COMPILE_OPTIONS "-mavx;-mfma;-mavx2")
        elseif(MSVC)
            target_compile_definitions(lightning_rnnoise PRIVATE
                OPUS_X86_MAY_HAVE_SSE OPUS_X86_MAY_HAVE_SSE2 OPUS_X86_MAY_HAVE_SSE4_1
                OPUS_X86_MAY_HAVE_AVX2)
            set_source_files_properties(${_rn_src}/x86/nnet_avx2.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        endif()
    endif()
endif()

# The C++ wrapper. Always defined so a consumer can link it unconditionally;
# with RNNoise OFF it builds the stub (createRnnoiseSuppressor() == nullptr).
add_library(lightning_noise_rnnoise STATIC
    src/calls/noise/RnnoiseSuppressor.h
    src/calls/noise/RnnoiseSuppressor.cpp)
set_target_properties(lightning_noise_rnnoise PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(lightning_noise_rnnoise
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/calls/noise")
if(LIGHTNING_ENABLE_RNNOISE)
    target_link_libraries(lightning_noise_rnnoise PUBLIC lightning_rnnoise)
    target_compile_definitions(lightning_noise_rnnoise PUBLIC HAVE_RNNOISE=1)
    message(STATUS "RNNoise: vendored third_party/rnnoise (v0.2), model compiled in")
else()
    message(STATUS "RNNoise: disabled (LIGHTNING_ENABLE_RNNOISE=OFF)")
endif()

function(lightning_add_rnnoise_tests)
    qt_add_executable(rnnoise-suppressor-test
        tests/RnnoiseSuppressorTest.cpp
        tests/RnnoiseTestSignals.h)
    target_include_directories(rnnoise-suppressor-test PRIVATE tests)
    target_link_libraries(rnnoise-suppressor-test PRIVATE
        lightning_noise_rnnoise Qt6::Core Qt6::Test)
    add_test(NAME rnnoise-suppressor COMMAND rnnoise-suppressor-test)
endfunction()
