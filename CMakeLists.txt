cmake_minimum_required(VERSION 3.21)
project(matrix-client
    VERSION 0.5.0
    DESCRIPTION "Lightning — native C++/Qt Matrix desktop client"
    LANGUAGES CXX
)

# Human-facing label. Keep the prep suffix until Matrix Rust SDK encrypted
# rooms round-trip in both directions and the E2EE gate can be enabled.
set(APP_VERSION_LABEL "0.5.0-prep+6")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(ENABLE_RUST_SDK_BACKEND
    "Build the Matrix Rust SDK backend (requires cargo in PATH)" OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    Network
    Sql
    Widgets
)

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# Optional libsecret (Freedesktop Secret Service) backend for SecretStore.
# When absent, LibSecretStore compiles as a no-op stub and the factory falls
# back to InsecureFallbackSecretStore (QSettings) with a visible warning.
# ---------------------------------------------------------------------------
find_package(PkgConfig)
set(HAVE_LIBSECRET OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
    if(LIBSECRET_FOUND)
        set(HAVE_LIBSECRET ON)
        message(STATUS "libsecret-1 found (${LIBSECRET_VERSION}) — enabling native secure storage")
    else()
        message(STATUS "libsecret-1 not found — SecretStore will use insecure QSettings fallback")
    endif()
else()
    message(STATUS "pkg-config not found — cannot probe libsecret; using insecure fallback")
endif()

set(APP_SOURCES
    src/main.cpp

    src/app/AppController.h
    src/app/AppController.cpp
    src/app/SettingsManager.h
    src/app/SettingsManager.cpp

    src/auth/AuthManager.h
    src/auth/AuthManager.cpp
    src/auth/AccountManager.h
    src/auth/AccountManager.cpp

    src/matrix/MatrixClient.h
    src/matrix/MatrixClient.cpp
    src/matrix/MockMatrixClient.h
    src/matrix/MockMatrixClient.cpp
    src/matrix/CppHttpMatrixClient.h
    src/matrix/CppHttpMatrixClient.cpp
    src/matrix/MediaHelpers.h
    src/matrix/MediaHelpers.cpp
    src/matrix/RoomInfo.h
    src/matrix/TimelineEvent.h

    src/storage/AppDataPaths.h
    src/storage/AppDataPaths.cpp
    src/storage/CacheStore.h
    src/storage/CacheStore.cpp
    src/storage/SecretStore.h
    src/storage/SecretStore.cpp
    src/storage/InsecureFallbackSecretStore.h
    src/storage/InsecureFallbackSecretStore.cpp
    src/storage/LibSecretStore.h
    src/storage/LibSecretStore.cpp

    src/models/RoomListModel.h
    src/models/RoomListModel.cpp
    src/models/TimelineModel.h
    src/models/TimelineModel.cpp
    src/models/MessageComposer.h
    src/models/MessageComposer.cpp

    src/notifications/NotificationManager.h
    src/notifications/NotificationManager.cpp
    src/media/MediaManager.h
    src/media/MediaManager.cpp
    src/crypto/CryptoManager.h
    src/crypto/CryptoManager.cpp
    src/spaces/SpaceManager.h
    src/spaces/SpaceManager.cpp
    src/threads/ThreadManager.h
    src/threads/ThreadManager.cpp
)

if(ENABLE_RUST_SDK_BACKEND)
    list(APPEND APP_SOURCES
        src/matrix/RustSdkMatrixClient.h
        src/matrix/RustSdkMatrixClient.cpp
        src/smoke/RustSdkSmokeTest.h
        src/smoke/RustSdkSmokeTest.cpp
    )
endif()

set(APP_QML_FILES
    qml/Main.qml
    qml/LoginScreen.qml
    qml/SettingsScreen.qml
    qml/MainScreen.qml
    qml/RoomListPane.qml
    qml/TimelinePane.qml
    qml/RoomDelegate.qml
    qml/MessageDelegate.qml
    qml/MessageComposerBar.qml
    qml/AppTheme.qml
)

# Strip the "qml/" prefix so QML files sit at the module root and can be
# resolved via QQmlApplicationEngine::loadFromModule("MatrixClient", "Main").
foreach(_qml_file IN LISTS APP_QML_FILES)
    get_filename_component(_qml_name ${_qml_file} NAME)
    set_source_files_properties(${_qml_file}
        PROPERTIES QT_RESOURCE_ALIAS ${_qml_name})
endforeach()

# AppTheme is a singleton; mark it so `pragma Singleton` is honored across
# the module import boundary.
set_source_files_properties(qml/AppTheme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(matrix-client ${APP_SOURCES})

qt_add_qml_module(matrix-client
    URI MatrixClient
    VERSION 1.0
    QML_FILES ${APP_QML_FILES}
    RESOURCE_PREFIX /qt/qml
)

target_include_directories(matrix-client PRIVATE src)

target_link_libraries(matrix-client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
)

target_compile_definitions(matrix-client PRIVATE
    APP_VERSION="${APP_VERSION_LABEL}"
    $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
)

if(HAVE_LIBSECRET)
    target_link_libraries(matrix-client PRIVATE PkgConfig::LIBSECRET)
    target_compile_definitions(matrix-client PRIVATE HAVE_LIBSECRET=1)
endif()

# ---------------------------------------------------------------------------
# Optional Rust SDK backend. Uses `cargo build` to produce a static library
# and links it into the C++ executable. Kept off by default so a fresh
# `cmake -B build` never requires the Rust toolchain.
# ---------------------------------------------------------------------------
if(ENABLE_RUST_SDK_BACKEND)
    find_package(SQLite3 REQUIRED)

    find_program(CARGO_EXECUTABLE cargo)
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR
            "ENABLE_RUST_SDK_BACKEND=ON but 'cargo' was not found in PATH. "
            "Install a Rust toolchain (e.g. `nix profile install nixpkgs#cargo`) "
            "or reconfigure with -DENABLE_RUST_SDK_BACKEND=OFF.")
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(_cargo_profile release)
        set(_cargo_profile_flag --release)
    else()
        set(_cargo_profile debug)
        set(_cargo_profile_flag "")
    endif()

    set(_rust_dir "${CMAKE_SOURCE_DIR}/rust")
    set(_rust_out_dir "${CMAKE_BINARY_DIR}/rust")
    set(_rust_staticlib "${_rust_out_dir}/${_cargo_profile}/libmatrix_client_rust.a")

    add_custom_command(
        OUTPUT ${_rust_staticlib}
        COMMAND ${CMAKE_COMMAND} -E env
            CARGO_TARGET_DIR=${_rust_out_dir}
            ${CARGO_EXECUTABLE} build ${_cargo_profile_flag}
                --offline --locked
                --manifest-path ${_rust_dir}/Cargo.toml
        DEPENDS ${_rust_dir}/src/lib.rs
                ${_rust_dir}/include/matrix_rust.h
                ${_rust_dir}/Cargo.toml
                ${_rust_dir}/Cargo.lock
        COMMENT "Building Rust SDK backend (cargo ${_cargo_profile})"
        VERBATIM
    )

    add_custom_target(matrix-client-rust-build DEPENDS ${_rust_staticlib})
    add_dependencies(matrix-client matrix-client-rust-build)

    add_library(matrix-client-rust STATIC IMPORTED GLOBAL)
    set_target_properties(matrix-client-rust PROPERTIES
        IMPORTED_LOCATION ${_rust_staticlib}
        INTERFACE_INCLUDE_DIRECTORIES "${_rust_dir}/include"
    )

    target_link_libraries(matrix-client PRIVATE matrix-client-rust SQLite::SQLite3)
    if(UNIX AND NOT APPLE)
        # Rust static libs typically pull in libdl / libpthread / libm at
        # some scale — link them defensively so growing the Rust surface
        # later does not surprise us with link-time failures.
        target_link_libraries(matrix-client PRIVATE dl pthread m)
    endif()

    target_compile_definitions(matrix-client PRIVATE ENABLE_RUST_SDK_BACKEND=1)
    message(STATUS "Rust SDK backend enabled (cargo profile: ${_cargo_profile})")
else()
    message(STATUS "Rust SDK backend disabled (use -DENABLE_RUST_SDK_BACKEND=ON to build it)")
endif()

if(MSVC)
    target_compile_options(matrix-client PRIVATE /W4 /permissive-)
else()
    target_compile_options(matrix-client PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

install(TARGETS matrix-client
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)
