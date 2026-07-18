cmake_minimum_required(VERSION 3.21)
project(matrix-client
    VERSION 0.6.0
    DESCRIPTION "Lightning — native C++/Qt Matrix desktop client"
    LANGUAGES CXX
)

include(CTest)

# Human-facing release label. Keep in sync with project() and rust/Cargo.toml.
set(APP_VERSION_LABEL "0.6.0")

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
# v0.6.0 checkpoint 11: freedesktop notifications (Linux-first). Optional so
# non-DBus platforms still build; NotificationManager degrades to a no-op.
find_package(Qt6 6.5 COMPONENTS DBus)

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
    src/models/QuickSwitcherModel.h
    src/models/QuickSwitcherModel.cpp
    src/models/TimelineModel.h
    src/models/TimelineModel.cpp
    src/models/TimelineScrollController.h
    src/models/TimelineScrollController.cpp
    src/models/MessageComposer.h
    src/models/MessageComposer.cpp
    src/models/EmojiCatalog.h
    src/models/EmojiCatalog.cpp
    src/models/AttachmentQueueModel.h
    src/models/AttachmentQueueModel.cpp
    src/models/UserSearchModel.h
    src/models/UserSearchModel.cpp
    src/models/UserLookup.h
    src/models/UserLookup.cpp
    src/models/PaginationController.h
    src/models/PaginationController.cpp
    src/models/ReadReceiptCoordinator.h
    src/models/ReadReceiptCoordinator.cpp
    src/models/LinkPreview.h
    src/models/LinkPreview.cpp
    src/models/LinkPreviewController.h
    src/models/LinkPreviewController.cpp
    src/gif/GifResponseParser.h
    src/gif/GifResponseParser.cpp
    src/gif/GifProvider.h
    src/gif/GifProvider.cpp
    src/gif/GifResultModel.h
    src/gif/GifResultModel.cpp
    src/gif/GifStoredModel.h
    src/gif/GifStoredModel.cpp
    src/gif/GifFavoritesModel.h
    src/gif/GifFavoritesModel.cpp
    src/gif/GifRecentModel.h
    src/gif/GifRecentModel.cpp
    src/gif/GifTransport.h
    src/gif/GifSearchController.h
    src/gif/GifSearchController.cpp
    src/gif/GifSendController.h
    src/gif/GifSendController.cpp
    src/gif/MatrixGifTransport.h
    src/gif/MatrixGifTransport.cpp

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
    src/crypto/CryptoHealthModel.h
    src/crypto/CryptoHealthModel.cpp
    src/crypto/E2eeDiagnostics.h
    src/crypto/E2eeDiagnostics.cpp
    src/spaces/SpaceManager.h
    src/spaces/SpaceManager.cpp
    src/threads/ThreadManager.h
    src/threads/ThreadManager.cpp
    src/threads/ThreadController.h
    src/threads/ThreadController.cpp
)

if(ENABLE_RUST_SDK_BACKEND)
    list(APPEND APP_SOURCES
        src/matrix/RustSdkMatrixClient.h
        src/matrix/RustSdkMatrixClient.cpp
        src/smoke/RustSdkSmokeTest.h
        src/smoke/RustSdkSmokeTest.cpp
    )
endif()

# Everything except main.cpp — reused by timeline-pane-qml-test so it can
# construct a real AppController and load the real QML module instead of
# re-declaring a parallel object graph.
set(APP_LIB_SOURCES ${APP_SOURCES})
list(REMOVE_ITEM APP_LIB_SOURCES src/main.cpp)

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
    qml/RoomActivityDelegate.qml
    qml/MessageComposerBar.qml
    qml/EmojiPicker.qml
    qml/UserPicker.qml
    qml/NewConversationDialog.qml
    qml/InvitePeopleDialog.qml
    qml/RoomInfoPanel.qml
    qml/ThreadPanel.qml
    qml/ThreadSummaryCard.qml
    qml/GifPicker.qml
    qml/QuickSwitcher.qml
    qml/ImageViewerOverlay.qml
    qml/AccountMenu.qml
    qml/MemberProfilePopover.qml
    qml/Avatar.qml
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

# Everything (app sources + compiled QML module) that a Qt executable needs
# to run the real UI, factored into a function so timeline-pane-qml-test can
# build a second binary wired up identically to matrix-client — a plain
# static "backing" library here proved unable to get Qt's static QML plugin
# auto-import to run reliably across target boundaries, so each Qt-executable
# target instead calls qt_add_qml_module on itself directly (the arrangement
# Qt documents and the one that's actually verified to work).
function(lightning_configure_qml_app target)
    qt_add_qml_module(${target}
        URI MatrixClient
        VERSION 1.0
        QML_FILES ${APP_QML_FILES}
        RESOURCES data/emoji-catalog.tsv
        RESOURCE_PREFIX /qt/qml
        # Every Qt-executable target that calls qt_add_qml_module registers
        # its own independent compiled copy of the same "MatrixClient" URI;
        # each one needs a distinct scratch directory so two targets in the
        # same build tree don't collide on it.
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml-modules/${target}"
    )

    target_include_directories(${target} PRIVATE src)
    # qmltyperegistrar's generated qml_register_types_MatrixClient() probes
    # for each QML_ELEMENT header with #if __has_include(<HeaderName.h>) — a
    # bare filename, not the "models/HeaderName.h" path used by quoted
    # includes elsewhere. Add each subdirectory holding a QML-registered type
    # so that angle-bracket lookup resolves.
    target_include_directories(${target} PRIVATE src/models src/threads src/crypto src/gif)

    target_link_libraries(${target} PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Network
        Qt6::Sql
        Qt6::Widgets
    )
    if(TARGET Qt6::DBus)
        target_link_libraries(${target} PRIVATE Qt6::DBus)
        target_compile_definitions(${target} PRIVATE HAVE_QT_DBUS=1)
    endif()

    target_compile_definitions(${target} PRIVATE
        APP_VERSION="${APP_VERSION_LABEL}"
        $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
    )

    if(HAVE_LIBSECRET)
        target_link_libraries(${target} PRIVATE PkgConfig::LIBSECRET)
        target_compile_definitions(${target} PRIVATE HAVE_LIBSECRET=1)
    endif()

    if(ENABLE_RUST_SDK_BACKEND)
        add_dependencies(${target} matrix-client-rust-build)
        target_link_libraries(${target} PRIVATE matrix-client-rust SQLite::SQLite3)
        if(UNIX AND NOT APPLE)
            # Rust static libs typically pull in libdl / libpthread / libm at
            # some scale — link them defensively so growing the Rust surface
            # later does not surprise us with link-time failures.
            target_link_libraries(${target} PRIVATE dl pthread m)
        endif()

        target_compile_definitions(${target} PRIVATE ENABLE_RUST_SDK_BACKEND=1)

        # v0.5.0-prep+9: initial E2EE gate. Both directions verified live
        # against matrix.smetonis.net (Element Classic ↔ Lightning marker
        # round-trip: expect_text=seen + encrypted_send=ok). Flip so
        # CryptoManager::supportsE2ee() returns true for the Rust backend.
        # HTTP and Mock still return false regardless.
        target_compile_definitions(${target} PRIVATE RUST_SDK_E2EE_WIRED=1)
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()
endfunction()

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
                ${_rust_dir}/src/rooms.rs
                ${_rust_dir}/src/timeline.rs
                ${_rust_dir}/include/matrix_rust.h
                ${_rust_dir}/Cargo.toml
                ${_rust_dir}/Cargo.lock
        COMMENT "Building Rust SDK backend (cargo ${_cargo_profile})"
        VERBATIM
    )

    add_custom_target(matrix-client-rust-build DEPENDS ${_rust_staticlib})

    add_library(matrix-client-rust STATIC IMPORTED GLOBAL)
    set_target_properties(matrix-client-rust PROPERTIES
        IMPORTED_LOCATION ${_rust_staticlib}
        INTERFACE_INCLUDE_DIRECTORIES "${_rust_dir}/include"
    )

    message(STATUS "Rust SDK backend enabled (cargo profile: ${_cargo_profile}); initial E2EE gate open")
else()
    message(STATUS "Rust SDK backend disabled (use -DENABLE_RUST_SDK_BACKEND=ON to build it)")
endif()

qt_add_executable(matrix-client ${APP_SOURCES})
lightning_configure_qml_app(matrix-client)

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

    qt_add_executable(emoji-catalog-test
        tests/EmojiCatalogTest.cpp
        src/models/EmojiCatalog.h
        src/models/EmojiCatalog.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
    )
    qt_add_resources(emoji-catalog-test "emoji-test-data"
        PREFIX "/qt/qml/MatrixClient"
        FILES data/emoji-catalog.tsv)
    target_include_directories(emoji-catalog-test PRIVATE src)
    target_link_libraries(emoji-catalog-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME emoji-catalog COMMAND emoji-catalog-test)

    qt_add_executable(gif-response-parser-test
        tests/GifResponseParserTest.cpp
        src/gif/GifResponseParser.h
        src/gif/GifResponseParser.cpp
    )
    target_include_directories(gif-response-parser-test PRIVATE src)
    target_link_libraries(gif-response-parser-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME gif-response-parser COMMAND gif-response-parser-test)

    qt_add_executable(gif-provider-test
        tests/GifProviderTest.cpp
        src/gif/GifResponseParser.h
        src/gif/GifResponseParser.cpp
        src/gif/GifProvider.h
        src/gif/GifProvider.cpp
    )
    target_include_directories(gif-provider-test PRIVATE src)
    target_link_libraries(gif-provider-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME gif-provider COMMAND gif-provider-test)

    set(GIF_CORE_SOURCES
        src/gif/GifResponseParser.h
        src/gif/GifResponseParser.cpp
        src/gif/GifProvider.h
        src/gif/GifProvider.cpp
        src/gif/GifResultModel.h
        src/gif/GifResultModel.cpp
        src/gif/GifStoredModel.h
        src/gif/GifStoredModel.cpp
        src/gif/GifFavoritesModel.h
        src/gif/GifFavoritesModel.cpp
        src/gif/GifRecentModel.h
        src/gif/GifRecentModel.cpp
        src/gif/GifTransport.h
        src/gif/GifSearchController.h
        src/gif/GifSearchController.cpp
    )

    qt_add_executable(gif-search-controller-test
        tests/GifSearchControllerTest.cpp
        ${GIF_CORE_SOURCES}
    )
    target_include_directories(gif-search-controller-test PRIVATE src)
    target_link_libraries(gif-search-controller-test PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME gif-search-controller COMMAND gif-search-controller-test)

    qt_add_executable(gif-collections-test
        tests/GifCollectionsTest.cpp
        src/gif/GifResponseParser.h
        src/gif/GifResponseParser.cpp
        src/gif/GifResultModel.h
        src/gif/GifResultModel.cpp
        src/gif/GifStoredModel.h
        src/gif/GifStoredModel.cpp
        src/gif/GifFavoritesModel.h
        src/gif/GifFavoritesModel.cpp
        src/gif/GifRecentModel.h
        src/gif/GifRecentModel.cpp
    )
    target_include_directories(gif-collections-test PRIVATE src)
    target_link_libraries(gif-collections-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME gif-collections COMMAND gif-collections-test)

    qt_add_executable(gif-send-controller-test
        tests/GifSendControllerTest.cpp
        src/gif/GifSendController.h
        src/gif/GifSendController.cpp
        src/gif/GifResponseParser.h
        src/gif/GifResponseParser.cpp
        src/gif/GifResultModel.h
        src/gif/GifResultModel.cpp
        src/gif/GifStoredModel.h
        src/gif/GifStoredModel.cpp
        src/gif/GifRecentModel.h
        src/gif/GifRecentModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
    )
    target_include_directories(gif-send-controller-test PRIVATE src)
    target_link_libraries(gif-send-controller-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME gif-send-controller COMMAND gif-send-controller-test)

    qt_add_executable(emoji-ui-contract-test tests/EmojiUiContractTest.cpp)
    target_compile_definitions(emoji-ui-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(emoji-ui-contract-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME emoji-ui-contract COMMAND emoji-ui-contract-test)

    # v0.5.12: guard the non-circular sizing/visibility contracts for the
    # two confirmation dialogs and timeline pagination header.
    qt_add_executable(qml-binding-contract-test tests/QmlBindingContractTest.cpp)
    target_compile_definitions(qml-binding-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(qml-binding-contract-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME qml-binding-contract COMMAND qml-binding-contract-test)

    # 0.5.14 checkpoint 1: a text scan cannot catch a QML type that was never
    # registered (the PaginationController ReferenceError regression). This
    # boots a real AppController on the mock backend and loads the real
    # TimelinePane.qml through the real "MatrixClient" QML module.
    qt_add_executable(timeline-pane-qml-test
        tests/TimelinePaneQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(timeline-pane-qml-test)
    target_link_libraries(timeline-pane-qml-test PRIVATE Qt6::Test)
    add_test(NAME timeline-pane-qml COMMAND timeline-pane-qml-test)
    set_tests_properties(timeline-pane-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

    # 0.5.14 checkpoint 2: state-activity grouping must be transparent
    # through virtual SDK rows (date dividers, read markers, timeline
    # start) and must still break on any visible message/media event.
    qt_add_executable(state-activity-grouping-test
        tests/StateActivityGroupingTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(state-activity-grouping-test PRIVATE src)
    target_link_libraries(state-activity-grouping-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME state-activity-grouping COMMAND state-activity-grouping-test)

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
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(conversation-flow-test PRIVATE src)
    target_link_libraries(conversation-flow-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME conversation-flow COMMAND conversation-flow-test)

    # v0.6.0: SDK-backed thread foundation — lifecycle (open/close/switch),
    # root/reply identity via the composite timeline id, failure states,
    # thread-only send path, encrypted-thread fixtures, participants.
    qt_add_executable(thread-controller-test
        tests/ThreadControllerTest.cpp
        src/threads/ThreadController.h
        src/threads/ThreadController.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/models/AttachmentQueueModel.h
        src/models/AttachmentQueueModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
    )
    target_include_directories(thread-controller-test PRIVATE src)
    target_link_libraries(thread-controller-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME thread-controller COMMAND thread-controller-test)

    # v0.6.0 checkpoint 7: read-only E2EE health model — readiness states,
    # tri-state trust, backup/recovery distinctions, generation isolation,
    # and the no-secrets contract.
    qt_add_executable(crypto-health-model-test
        tests/CryptoHealthModelTest.cpp
        src/crypto/CryptoHealthModel.h
        src/crypto/CryptoHealthModel.cpp
    )
    target_include_directories(crypto-health-model-test PRIVATE src)
    target_link_libraries(crypto-health-model-test PRIVATE
        Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME crypto-health-model COMMAND crypto-health-model-test)

    # v0.6.0 checkpoint 11: the pure notification decision policy — privacy
    # modes, mention metadata, local room modes, active-room suppression,
    # never-expose-ciphertext.
    qt_add_executable(notification-manager-test
        tests/NotificationManagerTest.cpp
        src/notifications/NotificationManager.h
        src/notifications/NotificationManager.cpp
    )
    target_include_directories(notification-manager-test PRIVATE src)
    target_link_libraries(notification-manager-test PRIVATE
        Qt6::Core Qt6::Test)
    if(TARGET Qt6::DBus)
        target_link_libraries(notification-manager-test PRIVATE Qt6::DBus)
        target_compile_definitions(notification-manager-test
            PRIVATE HAVE_QT_DBUS=1)
    endif()
    add_test(NAME notification-manager COMMAND notification-manager-test)

    # v0.5.11: safe link previews — URL extraction with scheme allow-list,
    # GIF MIME classification, encrypted-room privacy gating, bounded cache,
    # stale rejection, settings defaults.
    qt_add_executable(link-preview-test
        tests/LinkPreviewTest.cpp
        src/models/LinkPreview.h
        src/models/LinkPreview.cpp
        src/models/LinkPreviewController.h
        src/models/LinkPreviewController.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(link-preview-test PRIVATE src)
    target_link_libraries(link-preview-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME link-preview COMMAND link-preview-test)

    # v0.5.11: avatar/media request layer — dedup, per-size keys, bounded
    # LRU, sign-out stale rejection, failure marking with explicit retry.
    qt_add_executable(media-bridge-test
        tests/MediaBridgeTest.cpp
        src/media/MediaBridge.h
        src/media/MediaBridge.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(media-bridge-test PRIVATE src)
    target_link_libraries(media-bridge-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME media-bridge COMMAND media-bridge-test)

    # v0.5.11: bare-localpart user lookup — candidate derivation from the
    # authenticated account's server, profile confirmation before exposure,
    # merge deduplication with provenance, stale-result rejection.
    qt_add_executable(user-lookup-test
        tests/UserLookupTest.cpp
        src/models/UserSearchModel.h
        src/models/UserSearchModel.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(user-lookup-test PRIVATE src)
    target_link_libraries(user-lookup-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME user-lookup COMMAND user-lookup-test)

    # v0.5.11: backward-pagination policy — single-flight, viewport-fill
    # budget, no-progress stop, retry, stale room/reset/sign-out isolation.
    qt_add_executable(pagination-controller-test
        tests/PaginationControllerTest.cpp
        src/models/PaginationController.h
        src/models/PaginationController.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(pagination-controller-test PRIVATE src)
    target_link_libraries(pagination-controller-test PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME pagination-controller COMMAND pagination-controller-test)

    # v0.5.19: device-aware wheel-scroll math — per-notch distance/ordering,
    # coalescing, partial-delta accumulation, direction reversal, pixel vs
    # angle distinction, bound clamping, motion cancellation.
    qt_add_executable(timeline-scroll-controller-test
        tests/TimelineScrollControllerTest.cpp
        src/models/TimelineScrollController.h
        src/models/TimelineScrollController.cpp
    )
    target_include_directories(timeline-scroll-controller-test PRIVATE src)
    target_link_libraries(timeline-scroll-controller-test PRIVATE
        Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME timeline-scroll-controller COMMAND timeline-scroll-controller-test)

    # v0.5.11: automatic read receipts — visibility conjunction, debounce
    # cancellation, eligibility, duplicate suppression, ordering.
    qt_add_executable(read-receipt-coordinator-test
        tests/ReadReceiptCoordinatorTest.cpp
        src/models/ReadReceiptCoordinator.h
        src/models/ReadReceiptCoordinator.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(read-receipt-coordinator-test PRIVATE src)
    target_link_libraries(read-receipt-coordinator-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME read-receipt-coordinator COMMAND read-receipt-coordinator-test)

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

    # v0.6.1: quick switcher result model — matching, categories, bounded
    # results, routing payload, logout clearing.
    qt_add_executable(quick-switcher-test
        tests/QuickSwitcherTest.cpp
        src/models/QuickSwitcherModel.h
        src/models/QuickSwitcherModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
    )
    target_include_directories(quick-switcher-test PRIVATE src)
    target_link_libraries(quick-switcher-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME quick-switcher COMMAND quick-switcher-test)

    # v0.6.1: E2EE diagnostics redaction — never reveals full ids, stable for
    # correlation.
    qt_add_executable(e2ee-diagnostics-test
        tests/E2eeDiagnosticsTest.cpp
        src/crypto/E2eeDiagnostics.h
        src/crypto/E2eeDiagnostics.cpp
    )
    target_include_directories(e2ee-diagnostics-test PRIVATE src)
    target_link_libraries(e2ee-diagnostics-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME e2ee-diagnostics COMMAND e2ee-diagnostics-test)
endif()

install(TARGETS matrix-client
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)
