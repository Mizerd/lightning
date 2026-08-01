cmake_minimum_required(VERSION 3.21)
project(matrix-client
    VERSION 0.6.4
    DESCRIPTION "Lightning — native C++/Qt Matrix desktop client"
    LANGUAGES CXX
)

include(CTest)

# Human-facing release label. Keep in sync with project() and rust/Cargo.toml.
set(APP_VERSION_LABEL "0.6.4")

# Build metadata surfaced by `matrix-client --build-info` (booleans/strings
# only; never secrets). The source SHA is resolved from git at configure time
# and can be overridden by packaging (project 7 checks out a pinned commit).
set(LIGHTNING_SOURCE_SHA "" CACHE STRING "Source commit SHA for --build-info")
if(NOT LIGHTNING_SOURCE_SHA)
    execute_process(
        COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE _lightning_git_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _lightning_git_rc)
    if(_lightning_git_rc EQUAL 0)
        set(LIGHTNING_SOURCE_SHA "${_lightning_git_sha}")
    endif()
endif()
set(LIGHTNING_BUILD_TARGET "" CACHE STRING "Target triple for --build-info")
if(NOT LIGHTNING_BUILD_TARGET)
    if(CMAKE_CXX_COMPILER_TARGET)
        set(LIGHTNING_BUILD_TARGET "${CMAKE_CXX_COMPILER_TARGET}")
    elseif(WIN32)
        set(LIGHTNING_BUILD_TARGET "x86_64-pc-windows-gnu")
    else()
        set(LIGHTNING_BUILD_TARGET
            "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")
    endif()
endif()
# "unsigned-test", "release", or "source" (default). Packaging sets this.
set(LIGHTNING_ARTIFACT_KIND "source" CACHE STRING "Artifact kind for --build-info")

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

# Release/packaging guard: exclude the HTTP and mock backends from the shipped
# matrix-client binary entirely, so a production package can only ever run the
# Rust SDK backend (no --backend=http / --mock, no silent fallback). OFF for
# ordinary dev/test builds, which keep mock/http buildable and testable per the
# architecture contract; project 7 packaging sets it ON. When ON it REQUIRES
# ENABLE_RUST_SDK_BACKEND=ON (enforced below) and only affects the main target —
# the mock-based test executables compile their own MockMatrixClient and are
# unaffected.
option(LIGHTNING_RUST_ONLY
    "Ship ONLY the Rust backend: drop HTTP/mock from the matrix-client binary" OFF)
if(LIGHTNING_RUST_ONLY AND NOT ENABLE_RUST_SDK_BACKEND)
    message(FATAL_ERROR
        "LIGHTNING_RUST_ONLY=ON requires ENABLE_RUST_SDK_BACKEND=ON "
        "(a Rust-only build must have the Rust backend compiled in).")
endif()

# ---------------------------------------------------------------------------
# Development-only screenshot/demo mode. Compiles the --screenshot-demo entry
# point that boots the real UI on the in-memory mock backend with deterministic
# fake accounts/rooms/timelines for promotional screenshots. It is DEVELOPMENT
# ONLY and must be impossible to reach in a shipped binary:
#   * default OFF;
#   * a LIGHTNING_RUST_ONLY release FORCES it off (fatal error if combined) —
#     the mock backend it needs is not even compiled into a release binary;
#   * when OFF the matrix-client binary has no --screenshot-demo flag (preflight
#     never parses it) and --build-info reports screenshot_demo_compiled:false.
# The guarded code lives behind #ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO in
# main.cpp / AppController, so a default/release build compiles it to nothing.
# ---------------------------------------------------------------------------
option(LIGHTNING_ENABLE_SCREENSHOT_DEMO
    "Development only: compile the --screenshot-demo mode (never in a release)" OFF)
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO AND LIGHTNING_RUST_ONLY)
    message(FATAL_ERROR
        "LIGHTNING_ENABLE_SCREENSHOT_DEMO=ON conflicts with LIGHTNING_RUST_ONLY=ON: "
        "the screenshot demo is a development-only feature and must never be "
        "compiled into a Rust-only release binary.")
endif()

# ---------------------------------------------------------------------------
# Build-only GIF provider keys. Official release packages embed application
# GIPHY/KLIPY keys so installed clients work without user configuration. The
# key VALUES are read from the environment by a dedicated -P script and written
# to a generated header in the build tree only. They never reach a compiler
# command line, CMakeCache.txt, install rules, package contents, or logs. An
# ordinary source build leaves the header keyless, preserving the missing-key
# state. LIGHTNING_REQUIRE_GIF_KEYS makes the generator fail if either key is
# absent (official release mode); project 7 sets it for publish builds.
#
# The generator reads:  LIGHTNING_BUILD_GIPHY_API_KEY / LIGHTNING_BUILD_KLIPY_API_KEY
# (build-only names, distinct from the LIGHTNING_*_API_KEY runtime overrides).
# ---------------------------------------------------------------------------
option(LIGHTNING_REQUIRE_GIF_KEYS
    "Require embedded GIF provider keys (official release packaging)" OFF)
set(LIGHTNING_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT_DIR=${LIGHTNING_GENERATED_DIR}"
        "-DREQUIRE_KEYS=${LIGHTNING_REQUIRE_GIF_KEYS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateGifBuildKeys.cmake"
    RESULT_VARIABLE _gif_keys_rc)
if(NOT _gif_keys_rc EQUAL 0)
    message(FATAL_ERROR "GIF build-key generation failed")
endif()
# Re-run configure (and thus regenerate the header) if the script changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateGifBuildKeys.cmake")

# Convenience target to remove generated key material after packaging.
add_custom_target(scrub-gif-build-keys
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${LIGHTNING_GENERATED_DIR}/LightningGifBuildKeys.h"
    COMMENT "Removing generated GIF build-key header")

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    Network
    Sql
    Widgets
    # v0.7: native inline video/audio playback (QMediaPlayer/VideoOutput;
    # FFmpeg backend by default on the pinned nixpkgs qtmultimedia).
    Multimedia
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
    src/app/SessionDiagnostics.h
    src/app/SessionDiagnostics.cpp
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
    src/matrix/EventPreview.h
    src/matrix/EventPreview.cpp
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
    src/storage/WinCredStore.h
    src/storage/WinCredStore.cpp

    src/models/RoomListModel.h
    src/models/RoomListModel.cpp
    src/models/QuickSwitcherModel.h
    src/models/QuickSwitcherModel.cpp
    src/models/TimelineModel.h
    src/models/TimelineModel.cpp
    src/models/MessageHtml.h
    src/models/MessageHtml.cpp
    src/models/TimelineScrollController.h
    src/models/TimelineScrollController.cpp
    src/models/MessageComposer.h
    src/models/MessageComposer.cpp
    src/models/MentionTokenizer.h
    src/models/MentionTokenizer.cpp
    src/models/MentionSuggestionModel.h
    src/models/MentionSuggestionModel.cpp
    src/models/MentionHighlighter.h
    src/models/MentionHighlighter.cpp
    src/models/MarkdownFormat.h
    src/models/MarkdownFormat.cpp
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
    src/gif/GifBuildKeys.h
    src/gif/GifBuildKeys.cpp
    src/gif/GifKeyConfig.h
    src/gif/GifKeyConfig.cpp
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
    src/gif/GifProviderSelfTest.h
    src/gif/GifProviderSelfTest.cpp
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
    src/media/MediaPlaybackController.h
    src/media/MediaPlaybackController.cpp
    src/crypto/CryptoManager.h
    src/crypto/CryptoManager.cpp
    src/crypto/CryptoHealthModel.h
    src/crypto/CryptoHealthModel.cpp
    src/crypto/CryptoBootstrapModel.h
    src/crypto/CryptoBootstrapModel.cpp
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

# Development-only: the screenshot-demo scenario/control-panel controller.
# Compiled into the app graph ONLY when the demo option is on, so it (and the
# `app.demo` object it backs) is absent from every normal/release build. The
# demo compile flag is applied per-target where these sources are used.
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
    list(APPEND APP_SOURCES
        src/app/ScreenshotDemoController.h
        src/app/ScreenshotDemoController.cpp
    )
endif()

# Everything except main.cpp — reused by timeline-pane-qml-test so it can
# construct a real AppController and load the real QML module instead of
# re-declaring a parallel object graph. Keeps mock/http so mock-based tests
# still build even in a LIGHTNING_RUST_ONLY configuration.
set(APP_LIB_SOURCES ${APP_SOURCES})
list(REMOVE_ITEM APP_LIB_SOURCES src/main.cpp)

set(APP_QML_FILES
    qml/Main.qml
    qml/Icon.qml
    qml/IconButton.qml
    qml/AppButton.qml
    qml/SegmentedControl.qml
    qml/AppTextField.qml
    qml/AppComboBox.qml
    qml/AppSwitch.qml
    qml/LoginScreen.qml
    qml/SettingsScreen.qml
    qml/MainScreen.qml
    qml/HomePane.qml
    qml/SpacesRail.qml
    qml/RoomsPanel.qml
    qml/TimelinePane.qml
    qml/RoomDelegate.qml
    qml/MessageDelegate.qml
    qml/RoomActivityDelegate.qml
    qml/MessageComposerBar.qml
    qml/MentionPopup.qml
    qml/EmojiPicker.qml
    qml/CreatePollDialog.qml
    qml/VideoPlayerCard.qml
    qml/AudioPlayerCard.qml
    qml/VideoControlBar.qml
    qml/VideoViewerOverlay.qml
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
    qml/Skeleton.qml
    qml/AppMenu.qml
    qml/AppMenuItem.qml
    qml/AppMenuSeparator.qml
    qml/MenuKeycap.qml
    qml/MenuSectionLabel.qml
    qml/StatusChip.qml
    qml/QuickReactionStrip.qml
    qml/IdentityCard.qml
    qml/TrustCard.qml
    qml/StormNode.qml
    qml/TrustMeter.qml
    qml/AppTheme.qml
)

# Development-only: the screenshot-demo control panel QML is added to the module
# ONLY when the demo option is on, so a normal/release module never contains it
# (Main.qml's Loader references it by a runtime string, never a compiled type).
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
    list(APPEND APP_QML_FILES qml/DemoControlPanel.qml)
endif()

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

# Development-only screenshot-demo media fixtures. Small, deterministic, license-
# clear abstract images (see scripts/generate-demo-media.sh) bundled into the QML
# module ONLY when the demo option is on, so they render through the real media
# delegates. Excluded from every release artifact. Resolved at runtime as
# :/qt/qml/MatrixClient/resources/screenshot-demo/<file>.
set(DEMO_MEDIA_RESOURCES)
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
    set(DEMO_MEDIA_RESOURCES
        resources/screenshot-demo/avatar-alex.png
        resources/screenshot-demo/avatar-taylor.png
        resources/screenshot-demo/avatar-nova.png
        resources/screenshot-demo/avatar-maya.png
        resources/screenshot-demo/avatar-jordan.png
        resources/screenshot-demo/avatar-sam.png
        resources/screenshot-demo/avatar-aisha.png
        resources/screenshot-demo/avatar-noah.png
        resources/screenshot-demo/avatar-priya.png
        resources/screenshot-demo/avatar-leo.png
        resources/screenshot-demo/coast.png
        resources/screenshot-demo/portrait.png
        resources/screenshot-demo/square.png
        resources/screenshot-demo/artwork.png
        resources/screenshot-demo/shot-timeline.png
        resources/screenshot-demo/palette.png
        resources/screenshot-demo/timelapse.png
        resources/screenshot-demo/loop.gif
        resources/screenshot-demo/release-notes.txt
    )
endif()

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
        RESOURCES
            data/emoji-catalog.tsv
            # Bundled UI fonts (OFL; licenses alongside). Loaded in main.cpp.
            "data/fonts/Manrope[wght].ttf"
            "data/fonts/JetBrainsMono[wght].ttf"
            # v0.7 selectable UI families (Settings → Appearance → Font).
            "data/fonts/Inter[wght].ttf"
            "data/fonts/IBMPlexSans[wght].ttf"
            "data/fonts/SourceSans3[wght].ttf"
            "data/fonts/PlusJakartaSans[wght].ttf"
            # v0.6.5 brand face for the trust surface (SPEC 1r); never a
            # selectable body face.
            "data/fonts/SpaceGrotesk[wght].ttf"
            # Icon glyphs for interface chrome (Apache-2.0 subset; see
            # scripts/generate-icon-font.sh and qml/Icon.qml).
            data/fonts/MaterialSymbolsRounded-subset.ttf
            # Window icon for source runs (installed builds also get the
            # hicolor theme icon). 256px so the title-bar/taskbar fallback
            # stays crisp on high-DPI and on Windows, where there is no icon
            # theme to supply a larger size.
            data/icons/hicolor/256x256/apps/lightning.png
            # Development-only demo media fixtures (empty list in a release
            # build; see DEMO_MEDIA_RESOURCES above).
            ${DEMO_MEDIA_RESOURCES}
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
    # Build-only GIF key header (may be keyless); include path only, no values.
    target_include_directories(${target} PRIVATE "${LIGHTNING_GENERATED_DIR}")

    target_link_libraries(${target} PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Network
        Qt6::Sql
        Qt6::Widgets
        Qt6::Multimedia
    )
    # DBus is a Linux/BSD desktop concept: freedesktop notifications and the
    # session bus. Gate on platform as well as availability, so a Windows Qt
    # that happens to ship the Qt6 DBus module does NOT compile the DBus path in
    # and try to autolaunch a session bus at startup (the "dbus-1" lookup noise
    # observed on native Windows). NotificationManager degrades to a no-op.
    if(TARGET Qt6::DBus AND NOT WIN32 AND NOT APPLE)
        target_link_libraries(${target} PRIVATE Qt6::DBus)
        target_compile_definitions(${target} PRIVATE HAVE_QT_DBUS=1)
    endif()

    target_compile_definitions(${target} PRIVATE
        APP_VERSION="${APP_VERSION_LABEL}"
        LIGHTNING_SOURCE_SHA="${LIGHTNING_SOURCE_SHA}"
        LIGHTNING_BUILD_TARGET="${LIGHTNING_BUILD_TARGET}"
        LIGHTNING_ARTIFACT_KIND="${LIGHTNING_ARTIFACT_KIND}"
        LIGHTNING_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
    )

    if(HAVE_LIBSECRET)
        target_link_libraries(${target} PRIVATE PkgConfig::LIBSECRET)
        target_compile_definitions(${target} PRIVATE HAVE_LIBSECRET=1)
    endif()

    # Windows Credential Manager backend for SecretStore (CredWriteW/ReadW/
    # DeleteW/EnumerateW live in advapi32). WIN32 is true for both MSVC and the
    # MinGW cross-build; off Windows WinCredStore compiles as a no-op stub.
    if(WIN32)
        target_link_libraries(${target} PRIVATE advapi32)
        target_compile_definitions(${target} PRIVATE HAVE_WINCRED=1)
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

# The shipped executable's own source list. In a LIGHTNING_RUST_ONLY release we
# physically drop the HTTP and mock backend TUs from the binary (the shared
# APP_SOURCES/APP_LIB_SOURCES keep them so mock-based tests still build).
# AUTOMOC only processes headers that are target sources, so removing both the
# .h and .cpp keeps the QObject mock/http clients and their moc out of the
# binary; --build-info then reports http/mock_backend_compiled:false truthfully.
set(APP_MAIN_SOURCES ${APP_SOURCES})
if(LIGHTNING_RUST_ONLY)
    list(REMOVE_ITEM APP_MAIN_SOURCES
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/CppHttpMatrixClient.h
        src/matrix/CppHttpMatrixClient.cpp
    )
endif()

qt_add_executable(matrix-client ${APP_MAIN_SOURCES})
lightning_configure_qml_app(matrix-client)
# Per-target (never on the shared mock-based test targets): drive the C++
# #ifdef guards in AppController and the --build-info self-report.
if(LIGHTNING_RUST_ONLY)
    target_compile_definitions(matrix-client PRIVATE LIGHTNING_RUST_ONLY=1)
endif()
# Development-only screenshot demo: drives the #ifdef guards in main.cpp /
# AppController and the --build-info self-report. Only ever set on the shipped
# target in a dedicated development build (never a release; the option guard
# above rejects combining it with LIGHTNING_RUST_ONLY).
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
    target_compile_definitions(matrix-client PRIVATE LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
endif()

# On Windows the production binary is a GUI-subsystem PE so a normal
# double-click never flashes or leaves a console window. --version / --help /
# --build-info still print to a parent console when launched from one (main.cpp
# attaches to it). Only the production executable gets this — test binaries stay
# console-subsystem so CTest captures their output.
if(WIN32)
    set_target_properties(matrix-client PROPERTIES WIN32_EXECUTABLE ON)
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

    qt_add_executable(account-registry-test
        tests/AccountRegistryTest.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/auth/AccountManager.h
        src/auth/AccountManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(account-registry-test PRIVATE src)
    target_link_libraries(account-registry-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME account-registry COMMAND account-registry-test)

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

    # v0.6.5: the recentEmoji QStringList property feeding the quick-react
    # strip — MRU order, validity filtering, change signals.
    qt_add_executable(emoji-catalog-recent-emoji-test
        tests/EmojiCatalogRecentEmojiTest.cpp
        src/models/EmojiCatalog.h
        src/models/EmojiCatalog.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
    )
    qt_add_resources(emoji-catalog-recent-emoji-test "emoji-recent-test-data"
        PREFIX "/qt/qml/MatrixClient"
        FILES data/emoji-catalog.tsv)
    target_include_directories(emoji-catalog-recent-emoji-test PRIVATE src)
    target_link_libraries(emoji-catalog-recent-emoji-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME emoji-catalog-recent-emoji
        COMMAND emoji-catalog-recent-emoji-test)

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
        src/gif/GifBuildKeys.h
        src/gif/GifBuildKeys.cpp
        src/gif/GifKeyConfig.h
        src/gif/GifKeyConfig.cpp
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
    target_include_directories(gif-search-controller-test PRIVATE src "${LIGHTNING_GENERATED_DIR}")

    # GIF key configuration source of truth: safe env-file parsing, source
    # precedence (environment > env file > build key), empty-value fallback,
    # and controller refresh signaling.
    qt_add_executable(gif-key-config-test
        tests/GifKeyConfigTest.cpp
        ${GIF_CORE_SOURCES}
    )
    target_include_directories(gif-key-config-test PRIVATE src "${LIGHTNING_GENERATED_DIR}")
    target_link_libraries(gif-key-config-test PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME gif-key-config COMMAND gif-key-config-test)

    # One activation must produce exactly one send. The picker latch closes
    # the common case (a second click or Return landing while the popup is
    # still closing); this suite pins the controller-side backstop, which
    # holds regardless of caller — identical in-flight destination+GIF is
    # dropped, while a deliberate repeat after the first resolves is not.
    qt_add_executable(gif-send-controller-dedup-test
        tests/GifSendControllerDedupTest.cpp
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
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(gif-send-controller-dedup-test PRIVATE src)
    target_link_libraries(gif-send-controller-dedup-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME gif-send-controller-dedup COMMAND gif-send-controller-dedup-test)

    # NOT COVERED: history landing during an ACTIVE wheel animation.
    #
    # 9a0e41a's own regression test only simulates a direct contentY write, so
    # it never exercises the momentum-vs-pagination race. An attempt to cover
    # it here was withdrawn: the offscreen harness could be driven to keep the
    # motion engine animating across a pagination round trip, but the anchor
    # delegate then fell outside the ListView's created range and the test
    # could not read the geometry it needed to assert on. Rather than weaken
    # the assertion into something that passes on a settled engine — green
    # while covering nothing, which is the exact gap that prompted the test —
    # the case is left uncovered and recorded as NOT TESTED.
    #
    # The fix itself is sound by inspection: restoreCapturedAnchor() reads the
    # LIVE contentY and applies a relative shift, and does not branch on
    # whether a gesture or an animation is in flight, so momentum and touchpad
    # travel the same code path. That is reasoning, not evidence.

    # Threads use the same prepend-on-requestOlder pattern as the main
    # timeline but had no anchor mechanism at all, so loading older replies
    # while scrolled up teleported the view.
    qt_add_executable(thread-panel-pagination-anchor-qml-test
        tests/ThreadPanelPaginationAnchorQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(thread-panel-pagination-anchor-qml-test)
    target_link_libraries(thread-panel-pagination-anchor-qml-test PRIVATE Qt6::Test)
    add_test(NAME thread-panel-pagination-anchor-qml
             COMMAND thread-panel-pagination-anchor-qml-test)
    set_tests_properties(thread-panel-pagination-anchor-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # P0 regression suite: the account-identity <-> SDK-store binding. The
    # store path used to create a session must equal the one used to restore
    # it; adoption of a divergent-case store must be unambiguous, idempotent
    # and non-destructive; a reset that matched nothing must say so; and each
    # failure condition must carry its own reason code.
    qt_add_executable(session-store-identity-test
        tests/SessionStoreIdentityTest.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/matrix/RustSessionPolicy.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(session-store-identity-test PRIVATE src)
    target_link_libraries(session-store-identity-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME session-store-identity COMMAND session-store-identity-test)

    # Support-diagnostics redaction: credential-shaped values are injected
    # into every free-text field the export carries and must not survive,
    # while the metadata that makes a bug report actionable must.
    qt_add_executable(session-diagnostics-test
        tests/SessionDiagnosticsTest.cpp
        src/app/SessionDiagnostics.h
        src/app/SessionDiagnostics.cpp
    )
    target_include_directories(session-diagnostics-test PRIVATE src)
    target_link_libraries(session-diagnostics-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME session-diagnostics COMMAND session-diagnostics-test)

    # Provider-key resolution unit test (build-key fallback + precedence).
    qt_add_executable(gif-build-keys-test
        tests/GifBuildKeysTest.cpp
        src/gif/GifBuildKeys.h
        src/gif/GifBuildKeys.cpp
    )
    target_include_directories(gif-build-keys-test PRIVATE src "${LIGHTNING_GENERATED_DIR}")
    target_link_libraries(gif-build-keys-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME gif-build-keys COMMAND gif-build-keys-test)

    # Contract test for the build-key generator: synthetic canary embedding,
    # presence-only reporting, safe failure, and no value leakage.
    add_test(NAME gif-build-keys-generator
        COMMAND "${CMAKE_COMMAND}"
            "-DGENERATOR=${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateGifBuildKeys.cmake"
            "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/gif-keygen-test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/GifBuildKeysGeneratorTest.cmake")
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
        # MockMatrixClient's registry-backed restore needs these:
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
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

    # v0.6.5: the MemberProfilePopover single-instance convention (one shared
    # popover per view, delegates route through the view) had no guard since
    # 8a77ce8 — a per-delegate popover is a real, silent perf regression.
    qt_add_executable(profile-popover-contract-test
        tests/ProfilePopoverContractTest.cpp)
    target_compile_definitions(profile-popover-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(profile-popover-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME profile-popover-contract
        COMMAND profile-popover-contract-test)

    # v0.6.5: the Settings verification card's visibility complement (the
    # c259b60 invisible-failure fix) was unguarded — pin the predicates.
    qt_add_executable(verification-card-contract-test
        tests/VerificationCardContractTest.cpp)
    target_compile_definitions(verification-card-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(verification-card-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME verification-card-contract
        COMMAND verification-card-contract-test)

    # v0.6.5: 1a/1d context-menu contracts — grouping, accelerators,
    # quick-react liveness guard, radio flyout purity, signal-only mutation
    # routing, and the permalink/leave wiring.
    qt_add_executable(context-menu-contract-test
        tests/ContextMenuContractTest.cpp)
    target_compile_definitions(context-menu-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(context-menu-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME context-menu-contract COMMAND context-menu-contract-test)

    # v0.6.5: 1m emoji-picker redesign contract — category rail, grid
    # metrics, footer preview, preserved choose()/recents ordering.
    qt_add_executable(emoji-picker-contract-test
        tests/EmojiPickerContractTest.cpp)
    target_compile_definitions(emoji-picker-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(emoji-picker-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME emoji-picker-contract COMMAND emoji-picker-contract-test)

    # v0.6.5: 1n GIF-picker redesign contract — chrome restyle plus a
    # belt-and-suspenders re-assertion of the exact-selection invariants.
    qt_add_executable(gif-picker-redesign-contract-test
        tests/GifPickerRedesignContractTest.cpp)
    target_compile_definitions(gif-picker-redesign-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(gif-picker-redesign-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME gif-picker-redesign-contract
        COMMAND gif-picker-redesign-contract-test)

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

    # v0.7 timeline-stability suite: the initial-hydration presentation gate,
    # bottom-pinned stability under asynchronous row growth, scrolled-up
    # anchor preservation, and row-scoped Set-diff model updates — the live
    # room-open jump regressions, staged deterministically on the mock.
    qt_add_executable(timeline-hydration-qml-test
        tests/TimelineHydrationQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(timeline-hydration-qml-test)
    target_link_libraries(timeline-hydration-qml-test PRIVATE Qt6::Test)
    add_test(NAME timeline-hydration-qml COMMAND timeline-hydration-qml-test)
    set_tests_properties(timeline-hydration-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7: type-specific media placeholders — reserved aspect geometry for
    # image/video/sticker, compact audio/voice rows, the decrypting text
    # skeleton, and the shared Skeleton primitive's animation eligibility.
    qt_add_executable(media-placeholder-qml-test
        tests/MediaPlaceholderQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(media-placeholder-qml-test)
    target_link_libraries(media-placeholder-qml-test PRIVATE Qt6::Test)
    add_test(NAME media-placeholder-qml COMMAND media-placeholder-qml-test)
    set_tests_properties(media-placeholder-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7: avatar pipeline — explicit loading/ready/missing/failed states
    # through the real MediaBridge + MediaImageProvider, pixel-proven
    # transparency (no fallback fill beneath a decoded avatar), stable
    # identity-keyed fallback colours, and clean URL-change resets.
    qt_add_executable(avatar-pipeline-qml-test
        tests/AvatarPipelineQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(avatar-pipeline-qml-test)
    target_link_libraries(avatar-pipeline-qml-test PRIVATE Qt6::Test)
    add_test(NAME avatar-pipeline-qml COMMAND avatar-pipeline-qml-test)
    set_tests_properties(avatar-pipeline-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7 regression: clicking a favorite GIF must choose the exact
    # provider-qualified favorite from the visible model — never the first
    # Trending item, never an index-zero substitute.
    qt_add_executable(gif-picker-selection-qml-test
        tests/GifPickerSelectionQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(gif-picker-selection-qml-test)
    target_link_libraries(gif-picker-selection-qml-test PRIVATE Qt6::Test)
    add_test(NAME gif-picker-selection-qml
             COMMAND gif-picker-selection-qml-test)
    set_tests_properties(gif-picker-selection-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7: authenticated startup lifecycle — the Boot/RestoringSession
    # state, the never-instantiate-login guarantee for valid sessions, the
    # genuine no-account and failed-restore login states.
    qt_add_executable(startup-session-test
        tests/StartupSessionTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(startup-session-test)
    target_link_libraries(startup-session-test PRIVATE Qt6::Test)
    add_test(NAME startup-session COMMAND startup-session-test)
    set_tests_properties(startup-session PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7: full account-switch lifecycle on a real AppController (mock
    # backend): activation, switching state, room-target clearing, logout
    # fallback to a remaining account, scoped background-account removal.
    qt_add_executable(account-switch-test
        tests/AccountSwitchTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(account-switch-test)
    target_link_libraries(account-switch-test PRIVATE Qt6::Test)
    add_test(NAME account-switch COMMAND account-switch-test)
    set_tests_properties(account-switch PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.5.7: live-timeline diff translation + generation tracking. Pure
    # Qt (no cargo), so it runs in both build trees.
    qt_add_executable(rust-timeline-ingest-test
        tests/RustTimelineIngestTest.cpp
        src/matrix/RustTimelineIngest.cpp
        # v0.6.5: the ingest normalizes reply previews through the shared
        # preview choke point.
        src/matrix/EventPreview.cpp
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
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
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
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
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
        SETTINGS_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/SettingsScreen.qml"
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
        src/models/MentionTokenizer.h
        src/models/MentionTokenizer.cpp
        src/models/MarkdownFormat.h
        src/models/MarkdownFormat.cpp
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
        src/models/MentionTokenizer.h
        src/models/MentionTokenizer.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/models/AttachmentQueueModel.h
        src/models/AttachmentQueueModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        # MockMatrixClient's registry-backed restore needs these:
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
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

    # v0.7: verified-session bootstrap status state machine — phase
    # derivation from sanitized SDK state streams, key-count accumulation,
    # idempotency, and per-session reset isolation.
    qt_add_executable(crypto-bootstrap-model-test
        tests/CryptoBootstrapModelTest.cpp
        src/crypto/CryptoBootstrapModel.h
        src/crypto/CryptoBootstrapModel.cpp
    )
    target_include_directories(crypto-bootstrap-model-test PRIVATE src)
    target_link_libraries(crypto-bootstrap-model-test PRIVATE
        Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME crypto-bootstrap-model COMMAND crypto-bootstrap-model-test)

    # Verification UI state machine: sas_ready -> confirming (synchronous)
    # -> waiting_for_peer -> done, no-op guards, stale-flow rejection, and
    # logout clearing. Slots QSKIP without the Rust backend.
    qt_add_executable(verification-flow-test
        tests/VerificationFlowTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(verification-flow-test)
    target_link_libraries(verification-flow-test PRIVATE Qt6::Test)
    add_test(NAME verification-flow COMMAND verification-flow-test)
    set_tests_properties(verification-flow PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # One-line semantic event summaries for side surfaces (room list,
    # notifications): poll fallbacks, mention markdown, newline collapse,
    # bounded length. Pins the multi-line poll room-row regression.
    qt_add_executable(event-preview-test
        tests/EventPreviewTest.cpp
        src/matrix/EventPreview.h
        src/matrix/EventPreview.cpp
    )
    target_include_directories(event-preview-test PRIVATE src)
    target_link_libraries(event-preview-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME event-preview COMMAND event-preview-test)

    # v0.6.0 checkpoint 11: the pure notification decision policy — privacy
    # modes, mention metadata, local room modes, active-room suppression,
    # never-expose-ciphertext.
    qt_add_executable(notification-manager-test
        tests/NotificationManagerTest.cpp
        src/notifications/NotificationManager.h
        src/notifications/NotificationManager.cpp
        src/matrix/EventPreview.h
        src/matrix/EventPreview.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
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

    # v0.7: shared inline-playback coordinator — single audible owner,
    # owner-checked release, stopAll generation for room/account switches.
    qt_add_executable(media-playback-controller-test
        tests/MediaPlaybackControllerTest.cpp
        src/media/MediaPlaybackController.h
        src/media/MediaPlaybackController.cpp
    )
    target_include_directories(media-playback-controller-test PRIVATE src)
    target_link_libraries(media-playback-controller-test
        PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME media-playback-controller
             COMMAND media-playback-controller-test)

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
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
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
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
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

    # v0.6.5: matrix.to room permalink formatting (1d Copy room link) —
    # alias preferred, id fallback, messagePermalink's encoding convention.
    qt_add_executable(room-permalink-test
        tests/RoomPermalinkTest.cpp
        src/models/RoomListModel.h
        src/models/RoomListModel.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-permalink-test PRIVATE src)
    target_link_libraries(room-permalink-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-permalink COMMAND room-permalink-test)

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
        # MockMatrixClient's registry-backed restore needs these:
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
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

    # v0.7 design shell: interface-chrome icon hygiene (no emoji as chrome,
    # every Icon name mapped, subset font shipped).
    qt_add_executable(icon-chrome-test
        tests/IconChromeTest.cpp
    )
    target_compile_definitions(icon-chrome-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(icon-chrome-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME icon-chrome COMMAND icon-chrome-test)

    # v0.7: desktop-integration contract (launcher entry, hicolor icons,
    # install rules, Qt identity wiring).
    qt_add_executable(desktop-integration-test
        tests/DesktopIntegrationTest.cpp
    )
    target_compile_definitions(desktop-integration-test PRIVATE
        SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(desktop-integration-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME desktop-integration COMMAND desktop-integration-test)

    # v0.7: offscreen visual-verification suite. Renders each design theme on
    # a representative Fusion-styled surface set (swatches + Button + TextField
    # + open ComboBox popup + initials avatar), grabs deterministic PNG
    # snapshots as artifacts, and asserts sampled pixels match each theme's own
    # tokens — proving themes switch cleanly and popups inherit the themed
    # palette (the dropdown-goes-wrong-colour regression) without a display.
    qt_add_executable(theme-snapshot-test
        tests/ThemeSnapshotTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(theme-snapshot-test)
    target_link_libraries(theme-snapshot-test PRIVATE Qt6::Test)
    add_test(NAME theme-snapshot COMMAND theme-snapshot-test)
    set_tests_properties(theme-snapshot PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIGHTNING_SNAPSHOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/snapshots")

    # Whole-shell design acceptance: renders the real MainScreen in a window
    # (mock backend, bundled fonts) across the three design themes and
    # representative sizes, saves PNG snapshots, and asserts rendered
    # geometry (340px thread panel, composer placement) and token-sampled
    # pixels.
    qt_add_executable(design-acceptance-test
        tests/DesignAcceptanceTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(design-acceptance-test)
    target_link_libraries(design-acceptance-test PRIVATE Qt6::Test)
    add_test(NAME design-acceptance COMMAND design-acceptance-test)
    set_tests_properties(design-acceptance PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIGHTNING_SNAPSHOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/snapshots")

    # In-shell Settings (correction spec §3): rail/room list stay, timeline
    # region swaps, header + gear chip states, featured theme cards with
    # fixed palettes, instant theme switching, functional layout/text-size
    # controls, close restores chat.
    qt_add_executable(settings-shell-qml-test
        tests/SettingsShellQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(settings-shell-qml-test)
    target_link_libraries(settings-shell-qml-test PRIVATE Qt6::Test)
    add_test(NAME settings-shell-qml COMMAND settings-shell-qml-test)
    set_tests_properties(settings-shell-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Login-screen session repair: the classified failure card replaces the
    # old single-message dead end. Asserts the repair is invocable with ZERO
    # typing (the old button read an empty user field and could not run),
    # that it targets the account that actually failed rather than the active
    # one, per-reason action sets, destructive confirmation naming the
    # account, and that raw form text no longer drives a reset.
    qt_add_executable(login-repair-qml-test
        tests/LoginRepairQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(login-repair-qml-test)
    target_compile_definitions(login-repair-qml-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(login-repair-qml-test PRIVATE Qt6::Test)
    add_test(NAME login-repair-qml COMMAND login-repair-qml-test)
    set_tests_properties(login-repair-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Rebuilt main composer (SPEC §2): one card, toolbar row over input row,
    # exact control order/geometry, borderless input, markdown round-trip,
    # theme-tracked card pixels, zero QML warnings.
    qt_add_executable(composer-qml-test
        tests/ComposerQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(composer-qml-test)
    target_link_libraries(composer-qml-test PRIVATE Qt6::Test)
    add_test(NAME composer-qml COMMAND composer-qml-test)
    set_tests_properties(composer-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Creation UX (New Conversation / Create Room / Create Space dialog):
    # per-tab instantiation (the Space tab must instantiate NO encryption
    # control), shared-search clearing + focus handoff on tab switch, name
    # validation gating, busy/error states, existing-DM reuse ordering, and
    # keyboard result selection.
    qt_add_executable(creation-dialog-qml-test
        tests/CreationDialogQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(creation-dialog-qml-test)
    target_link_libraries(creation-dialog-qml-test PRIVATE Qt6::Test)
    add_test(NAME creation-dialog-qml COMMAND creation-dialog-qml-test)
    set_tests_properties(creation-dialog-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Startup platform guard: the DISPLAY / WAYLAND_DISPLAY preflight is an
    # X11/Wayland concept and must fire only on Unix-like non-macOS platforms.
    # Header-only pure predicate, so no app sources are linked.
    qt_add_executable(startup-checks-test
        tests/StartupChecksTest.cpp
        src/app/StartupChecks.h
    )
    target_include_directories(startup-checks-test PRIVATE src)
    target_link_libraries(startup-checks-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME startup-checks COMMAND startup-checks-test)

    # Compiled default backend (Rust when the SDK backend is built, else HTTP)
    # and --backend=NAME parsing. Includes AppController.h for the enum only —
    # no app sources are linked, just the header's inline/constexpr helpers.
    qt_add_executable(backend-selection-test
        tests/BackendSelectionTest.cpp
        src/app/BackendSelection.h
    )
    target_include_directories(backend-selection-test PRIVATE src "${LIGHTNING_GENERATED_DIR}")
    target_link_libraries(backend-selection-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
        Qt6::Network Qt6::Sql Qt6::Widgets Qt6::Multimedia Qt6::Test)
    target_compile_definitions(backend-selection-test PRIVATE APP_VERSION="${APP_VERSION_LABEL}")
    if(ENABLE_RUST_SDK_BACKEND)
        target_compile_definitions(backend-selection-test PRIVATE ENABLE_RUST_SDK_BACKEND=1)
    endif()
    add_test(NAME backend-selection COMMAND backend-selection-test)

    # Production-exclusion contract for the development-only screenshot demo:
    # runs the real matrix-client binary and asserts --build-info's
    # screenshot_demo_compiled line and (when the option is OFF, i.e. every
    # release/default build) that --screenshot-demo is rejected in preflight.
    qt_add_executable(screenshot-demo-exclusion-test
        tests/ScreenshotDemoExclusionTest.cpp
    )
    target_link_libraries(screenshot-demo-exclusion-test PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(screenshot-demo-exclusion-test PRIVATE
        "SCREENSHOT_DEMO_BINARY=\"$<TARGET_FILE:matrix-client>\"")
    if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
        target_compile_definitions(screenshot-demo-exclusion-test PRIVATE
            LIGHTNING_TEST_DEMO_COMPILED=1)
    endif()
    add_dependencies(screenshot-demo-exclusion-test matrix-client)
    add_test(NAME screenshot-demo-exclusion COMMAND screenshot-demo-exclusion-test)

    # Development-only screenshot-demo DATASET contract: structure (Spaces,
    # required room types, poll, thread), determinism (stable ids + fixed
    # clock), and safety (only fictional *.example identities). Exercises
    # MockMatrixClient::setScreenshotDemoMode directly.
    qt_add_executable(screenshot-demo-data-test
        tests/ScreenshotDemoDataTest.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(screenshot-demo-data-test PRIVATE src)
    target_link_libraries(screenshot-demo-data-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME screenshot-demo-data COMMAND screenshot-demo-data-test)

    # Development-only screenshot-demo MULTI-ACCOUNT contract: the three
    # fictional accounts, deterministic order + *.example identities, account-
    # local dataset isolation with mutation survival, deterministic reset, and
    # the in-memory (never production/libsecret) SecretStore. Drives a real
    # AppController(MockBackend, screenshotDemo=true) through beginScreenshotDemo
    # and the real switchToAccount path, so it needs the full app graph and the
    # demo compile flag. Only built when the demo option is on.
    if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
        qt_add_executable(screenshot-demo-accounts-test
            tests/ScreenshotDemoAccountsTest.cpp
            ${APP_LIB_SOURCES}
        )
        lightning_configure_qml_app(screenshot-demo-accounts-test)
        target_compile_definitions(screenshot-demo-accounts-test PRIVATE
            LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
        target_link_libraries(screenshot-demo-accounts-test PRIVATE Qt6::Test)
        add_test(NAME screenshot-demo-accounts
            COMMAND screenshot-demo-accounts-test)
        set_tests_properties(screenshot-demo-accounts PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Development-only screenshot-demo MEDIA contract: every image/video/GIF
        # row and avatar resolves to bundled fixture bytes through the mock media
        # bridge, correct MIME, no network. Bundles the demo fixtures so the
        # fetch path can resolve them. Only built when the demo option is on.
        qt_add_executable(screenshot-demo-media-test
            tests/ScreenshotDemoMediaTest.cpp
            src/matrix/MatrixClient.h
            src/matrix/MatrixClient.cpp
            src/matrix/MockMatrixClient.h
            src/matrix/MockMatrixClient.cpp
            src/matrix/MediaHelpers.h
            src/matrix/MediaHelpers.cpp
            src/app/SettingsManager.cpp
            src/storage/AppDataPaths.cpp
        )
        target_include_directories(screenshot-demo-media-test PRIVATE src)
        qt_add_resources(screenshot-demo-media-test "demo-media"
            PREFIX "/qt/qml/MatrixClient"
            FILES ${DEMO_MEDIA_RESOURCES})
        target_link_libraries(screenshot-demo-media-test PRIVATE
            Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
        add_test(NAME screenshot-demo-media COMMAND screenshot-demo-media-test)

        # Development-only screenshot-demo local-interactions contract: poll vote
        # change, invite accept/reject, mark unread, reactions and message send
        # mutate the in-memory demo scene through the real backend interface;
        # reset restores it; all are no-ops outside demo mode.
        qt_add_executable(screenshot-demo-interactions-test
            tests/ScreenshotDemoInteractionsTest.cpp
            src/matrix/MatrixClient.h
            src/matrix/MatrixClient.cpp
            src/matrix/MockMatrixClient.h
            src/matrix/MockMatrixClient.cpp
            src/matrix/MediaHelpers.h
            src/matrix/MediaHelpers.cpp
            src/app/SettingsManager.cpp
            src/storage/AppDataPaths.cpp
        )
        target_include_directories(screenshot-demo-interactions-test PRIVATE src)
        target_link_libraries(screenshot-demo-interactions-test PRIVATE
            Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
        add_test(NAME screenshot-demo-interactions
            COMMAND screenshot-demo-interactions-test)

        # Development-only screenshot-demo SCENARIO controller contract: every
        # required scenario activates to the right account/room/page/thread/
        # theme/window size; presets resolve; per-account selected rooms restore
        # on switch-back; panel toggles reach the mock; reset restores defaults.
        qt_add_executable(screenshot-demo-scenario-test
            tests/ScreenshotDemoScenarioTest.cpp
            ${APP_LIB_SOURCES}
        )
        lightning_configure_qml_app(screenshot-demo-scenario-test)
        target_compile_definitions(screenshot-demo-scenario-test PRIVATE
            LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
        target_link_libraries(screenshot-demo-scenario-test PRIVATE Qt6::Test)
        add_test(NAME screenshot-demo-scenario
            COMMAND screenshot-demo-scenario-test)
        set_tests_properties(screenshot-demo-scenario PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Development-only screenshot-demo CONTROL PANEL contract in a real
        # (offscreen) QML window: the panel loads with no QML warnings and no
        # layout gap, the boot scenario and window presets resize the actual
        # window, and hide/restore works.
        qt_add_executable(screenshot-demo-panel-qml-test
            tests/ScreenshotDemoPanelQmlTest.cpp
            ${APP_LIB_SOURCES}
        )
        lightning_configure_qml_app(screenshot-demo-panel-qml-test)
        target_compile_definitions(screenshot-demo-panel-qml-test PRIVATE
            LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
        target_link_libraries(screenshot-demo-panel-qml-test PRIVATE Qt6::Test)
        add_test(NAME screenshot-demo-panel-qml
            COMMAND screenshot-demo-panel-qml-test)
        set_tests_properties(screenshot-demo-panel-qml PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    # Composer formatting toolbar: markdown wrap/unwrap, line prefixes, link
    # scaffold, selection integrity, and active-state detection.
    qt_add_executable(markdown-format-test
        tests/MarkdownFormatTest.cpp
        src/models/MarkdownFormat.h
        src/models/MarkdownFormat.cpp
    )
    target_include_directories(markdown-format-test PRIVATE src)
    target_link_libraries(markdown-format-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME markdown-format COMMAND markdown-format-test)

    # v0.7 outgoing @-mentions: pure tokenizer (trigger boundaries, inline
    # code / fenced-block rejection, query capture + cap, ref shifting, and
    # matrix.to link expansion).
    qt_add_executable(mention-tokenizer-test
        tests/MentionTokenizerTest.cpp
        src/models/MentionTokenizer.h
        src/models/MentionTokenizer.cpp
    )
    target_include_directories(mention-tokenizer-test PRIVATE src)
    target_link_libraries(mention-tokenizer-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME mention-tokenizer COMMAND mention-tokenizer-test)

    # v0.7 outgoing @-mentions: the current-room member suggestion model —
    # request on activation, filter/rank/dedup, own-user exclusion, stale
    # op-id rejection, room-switch + logout clearing, membersChanged re-request.
    qt_add_executable(mention-suggestion-test
        tests/MentionSuggestionModelTest.cpp
        src/models/MentionSuggestionModel.h
        src/models/MentionSuggestionModel.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        # MockMatrixClient's registry-backed restore needs these:
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(mention-suggestion-test PRIVATE src)
    target_link_libraries(mention-suggestion-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME mention-suggestion COMMAND mention-suggestion-test)

    # Incoming formatted-body sanitizer: allowlist tags, drop dangerous
    # markup/content, validate hrefs, and rewrite matrix.to mentions.
    qt_add_executable(message-html-test
        tests/MessageHtmlTest.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
    )
    target_include_directories(message-html-test PRIVATE src)
    target_link_libraries(message-html-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME message-html COMMAND message-html-test)

    # Shared control system: rendered proof that AppButton kinds,
    # SegmentedControl, AppTextField, and the AppComboBox popup are flat,
    # token-driven, and theme-tracking — no Fusion bevels or gradients.
    qt_add_executable(control-system-test
        tests/ControlSystemTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(control-system-test)
    target_link_libraries(control-system-test PRIVATE Qt6::Test)
    add_test(NAME control-system COMMAND control-system-test)
    set_tests_properties(control-system PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Design button system (SPEC §1): offscreen pixel proof that Style A is
    # bare at rest with a soft hover tint, Style B is the accent-soft chip,
    # Style C is the accent fill, and keyboard focus draws the accent ring.
    qt_add_executable(button-system-test
        tests/ButtonSystemTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(button-system-test)
    target_link_libraries(button-system-test PRIVATE Qt6::Test)
    add_test(NAME button-system COMMAND button-system-test)
    set_tests_properties(button-system PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.5 shared menu language (SPEC §0): AppMenu/AppMenuItem metrics and
    # ink, keycap chips, section labels, status chips, danger rows, and the
    # cascading flyout with its Escape-innermost-first contract.
    qt_add_executable(menu-system-qml-test
        tests/MenuSystemQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(menu-system-qml-test)
    target_link_libraries(menu-system-qml-test PRIVATE Qt6::Test)
    add_test(NAME menu-system-qml COMMAND menu-system-qml-test)
    set_tests_properties(menu-system-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.5: 1q mention-popup behavior — role chips, query header, selected
    # row keycap, accessibility, plus source-scan sub-tests (needs QML_DIR).
    qt_add_executable(mention-popup-contract-test
        tests/MentionPopupContractTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(mention-popup-contract-test)
    target_compile_definitions(mention-popup-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(mention-popup-contract-test PRIVATE Qt6::Test)
    add_test(NAME mention-popup-contract COMMAND mention-popup-contract-test)
    set_tests_properties(mention-popup-contract PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.5: 1h account switcher — vertical identity cards, real-data-only
    # meta, live-id switch guard, preserved confirmations.
    qt_add_executable(identity-card-account-menu-test
        tests/IdentityCardAccountMenuTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(identity-card-account-menu-test)
    target_compile_definitions(identity-card-account-menu-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(identity-card-account-menu-test PRIVATE Qt6::Test)
    add_test(NAME identity-card-account-menu
        COMMAND identity-card-account-menu-test)
    set_tests_properties(identity-card-account-menu PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.5: 1p member profile — omitted affordances stay omitted, DM-reuse
    # preserved, role chips, clipboard round-trip.
    qt_add_executable(member-profile-popover-contract-test
        tests/MemberProfilePopoverContractTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(member-profile-popover-contract-test)
    target_compile_definitions(member-profile-popover-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(member-profile-popover-contract-test PRIVATE
        Qt6::Test)
    add_test(NAME member-profile-popover-contract
        COMMAND member-profile-popover-contract-test)
    set_tests_properties(member-profile-popover-contract PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.5: 1r trust card — brand-fixed rendering, complete/pending steps,
    # no QR affordance, no app.* coupling.
    qt_add_executable(trust-card-test
        tests/TrustCardTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(trust-card-test)
    target_compile_definitions(trust-card-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(trust-card-test PRIVATE Qt6::Test)
    add_test(NAME trust-card COMMAND trust-card-test)
    set_tests_properties(trust-card PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.5: 1t invite dialog — chips, already-member pre-check, room
    # header, per-user result states (this dialog had zero coverage before).
    qt_add_executable(invite-people-dialog-qml-test
        tests/InvitePeopleDialogQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(invite-people-dialog-qml-test)
    target_compile_definitions(invite-people-dialog-qml-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(invite-people-dialog-qml-test PRIVATE Qt6::Test)
    add_test(NAME invite-people-dialog-qml
        COMMAND invite-people-dialog-qml-test)
    set_tests_properties(invite-people-dialog-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.5: 1j/1k quick switcher — mode transitions, honest command
    # actions, live theme execution, Escape (the audit's G2 coverage gap).
    qt_add_executable(quick-switcher-modes-qml-test
        tests/QuickSwitcherModesQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_app(quick-switcher-modes-qml-test)
    target_compile_definitions(quick-switcher-modes-qml-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(quick-switcher-modes-qml-test PRIVATE Qt6::Test)
    add_test(NAME quick-switcher-modes-qml
        COMMAND quick-switcher-modes-qml-test)
    set_tests_properties(quick-switcher-modes-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

install(TARGETS matrix-client
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)

# Desktop integration: launcher entry + hicolor application icons. The icon
# name "lightning" matches the desktop file's Icon= key; the generated sizes
# come from scripts/generate-icons.sh (source artwork in data/icons/).
include(GNUInstallDirs)
install(FILES data/lightning.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
foreach(_icon_size 16 32 48 64 128 192 256 512)
    install(FILES
        "data/icons/hicolor/${_icon_size}x${_icon_size}/apps/lightning.png"
        DESTINATION
        "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_icon_size}x${_icon_size}/apps")
endforeach()
