cmake_minimum_required(VERSION 3.21)
project(matrix-client
    VERSION 0.5.9
    DESCRIPTION "Lightning — native C++/Qt Matrix desktop client"
    LANGUAGES CXX
)

include(CTest)

# Human-facing release label. Keep in sync with project() and rust/Cargo.toml.
set(APP_VERSION_LABEL "0.5.9")

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
    src/app/ConversationController.h
    src/app/ConversationController.cpp
    src/app/RoomInfoController.h
    src/app/RoomInfoController.cpp
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
    src/matrix/RustTimelineIngest.h
    src/matrix/RustTimelineIngest.cpp
    src/matrix/RustSessionPolicy.h
    src/matrix/RustSessionPolicy.cpp
    src/matrix/SessionLifecycleGuard.h
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
    src/models/AttachmentQueueModel.h
    src/models/AttachmentQueueModel.cpp
    src/models/UserSearchModel.h
    src/models/UserSearchModel.cpp

    src/notifications/NotificationManager.h
    src/notifications/NotificationManager.cpp
    src/media/MediaManager.h
    src/media/MediaManager.cpp
    src/media/MediaBridge.h
    src/media/MediaBridge.cpp
    src/media/MediaImageProvider.h
    src/media/MediaImageProvider.cpp
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
    qml/SpacesRail.qml
    qml/RoomsPanel.qml
    qml/TimelinePane.qml
    qml/RoomDelegate.qml
    qml/MessageDelegate.qml
    qml/MessageComposerBar.qml
    qml/UserPicker.qml
    qml/NewConversationDialog.qml
    qml/InvitePeopleDialog.qml
    qml/RoomInfoPanel.qml
    qml/ImageViewerOverlay.qml
    qml/AccountMenu.qml
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

    # v0.5.0-prep+9: initial E2EE gate. Both directions verified live
    # against matrix.smetonis.net (Element Classic ↔ Lightning marker
    # round-trip: expect_text=seen + encrypted_send=ok). Flip so
    # CryptoManager::supportsE2ee() returns true for the Rust backend.
    # HTTP and Mock still return false regardless.
    target_compile_definitions(matrix-client PRIVATE RUST_SDK_E2EE_WIRED=1)
    message(STATUS "Rust SDK backend enabled (cargo profile: ${_cargo_profile}); initial E2EE gate open")
else()
    message(STATUS "Rust SDK backend disabled (use -DENABLE_RUST_SDK_BACKEND=ON to build it)")
endif()

if(MSVC)
    target_compile_options(matrix-client PRIVATE /W4 /permissive-)
else()
    target_compile_options(matrix-client PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    qt_add_executable(app-data-paths-test
        tests/AppDataPathsTest.cpp
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(app-data-paths-test PRIVATE src)
    target_link_libraries(app-data-paths-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME app-data-paths COMMAND app-data-paths-test)

    qt_add_executable(rust-session-lifecycle-test
        tests/RustSessionLifecycleTest.cpp
        src/storage/AppDataPaths.cpp
        src/matrix/RustSessionPolicy.cpp
    )
    target_include_directories(rust-session-lifecycle-test PRIVATE src)
    target_link_libraries(rust-session-lifecycle-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME rust-session-lifecycle COMMAND rust-session-lifecycle-test)

    qt_add_executable(settings-session-test
        tests/SettingsSessionTest.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(settings-session-test PRIVATE src)
    target_link_libraries(settings-session-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME settings-session COMMAND settings-session-test)

    # v0.5.7: live-timeline diff translation + generation tracking. Pure
    # Qt (no cargo), so it runs in both build trees.
    qt_add_executable(rust-timeline-ingest-test
        tests/RustTimelineIngestTest.cpp
        src/matrix/RustTimelineIngest.cpp
    )
    target_include_directories(rust-timeline-ingest-test PRIVATE src)
    target_link_libraries(rust-timeline-ingest-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME rust-timeline-ingest COMMAND rust-timeline-ingest-test)

    # v0.5.7: TimelineModel diff application (insert/replace/remove/
    # truncate/reset, local-echo reconciliation, invalid-index self-heal).
    qt_add_executable(timeline-model-diff-test
        tests/TimelineModelDiffTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(timeline-model-diff-test PRIVATE src)
    target_link_libraries(timeline-model-diff-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME timeline-model-diff COMMAND timeline-model-diff-test)

    # v0.5.7: CacheStore must refuse encrypted-room rows on every write
    # path — decrypted plaintext stays memory-only.
    qt_add_executable(cache-store-security-test
        tests/CacheStoreSecurityTest.cpp
        src/storage/CacheStore.cpp
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(cache-store-security-test PRIVATE src)
    target_link_libraries(cache-store-security-test PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME cache-store-security COMMAND cache-store-security-test)

    # v0.5.9: semantic theme tokens + WCAG contrast for critical pairs +
    # no stray hex colours in core view QML.
    qt_add_executable(theme-tokens-test
        tests/ThemeTokensTest.cpp
    )
    target_compile_definitions(theme-tokens-test PRIVATE
        APPTHEME_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/AppTheme.qml"
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
    )
    target_link_libraries(theme-tokens-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME theme-tokens COMMAND theme-tokens-test)

    # v0.5.9: attachment pipeline — validation, MIME-from-content, queue
    # state machine, retry, stale-completion rejection, lifecycle clears.
    qt_add_executable(attachment-queue-test
        tests/AttachmentQueueTest.cpp
        src/models/MessageComposer.h
        src/models/MessageComposer.cpp
        src/models/AttachmentQueueModel.h
        src/models/AttachmentQueueModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(attachment-queue-test PRIVATE src)
    target_link_libraries(attachment-queue-test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME attachment-queue COMMAND attachment-queue-test)

    # v0.5.9: user search + DM/room-creation/invite flows (debounce,
    # stale-result rejection, existing-DM reuse, duplicate-submission
    # blocking, partial invite success, sign-out invalidation).
    qt_add_executable(conversation-flow-test
        tests/ConversationFlowTest.cpp
        src/app/ConversationController.h
        src/app/ConversationController.cpp
        src/models/UserSearchModel.h
        src/models/UserSearchModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(conversation-flow-test PRIVATE src)
    target_link_libraries(conversation-flow-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME conversation-flow COMMAND conversation-flow-test)

    qt_add_executable(room-state-model-test
        tests/RoomStateModelTest.cpp
        src/models/RoomListModel.h
        src/models/RoomListModel.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-state-model-test PRIVATE src)
    target_link_libraries(room-state-model-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-state-model COMMAND room-state-model-test)
endif()

install(TARGETS matrix-client
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)
