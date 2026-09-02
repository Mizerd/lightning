cmake_minimum_required(VERSION 3.21)
project(lightning
    VERSION 0.8.3
    DESCRIPTION "Lightning — native C++/Qt Matrix desktop client"
    LANGUAGES CXX
)

include(CTest)

# Human-facing release label. Keep in sync with project() and rust/Cargo.toml.
set(APP_VERSION_LABEL "0.8.3")

# Build metadata surfaced by `lightning-matrix --build-info` (booleans/strings
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

# Optional compiler cache for local development. Packaging passes its own
# -DCMAKE_<LANG>_COMPILER_LAUNCHER when it wants one (lightning-deploy's
# configure-build.sh does), and an explicit setting always wins over this
# block. It is deliberately confined to source builds -- an official package
# sets LIGHTNING_ARTIFACT_KIND to something other than "source", so no release
# artifact can silently pick up a developer's cache. Absent ccache is not an
# error: the build simply proceeds uncached.
option(LIGHTNING_DISABLE_CCACHE "Never auto-enable ccache, even for source builds" OFF)
if(NOT LIGHTNING_DISABLE_CCACHE
        AND NOT CMAKE_CXX_COMPILER_LAUNCHER
        AND LIGHTNING_ARTIFACT_KIND STREQUAL "source")
    find_program(LIGHTNING_CCACHE_PROGRAM ccache)
    if(LIGHTNING_CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${LIGHTNING_CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${LIGHTNING_CCACHE_PROGRAM}")
        message(STATUS "ccache enabled for this source build (${LIGHTNING_CCACHE_PROGRAM})")
    endif()
endif()

# Update-manifest signature verification. Ed25519 through OpenSSL 3's EVP API;
# Lightning implements no signature algorithm of its own. REQUIRED on purpose:
# an updater that silently loses its ability to verify a signature would be far
# worse than one that fails to configure. Only libcrypto is used, never libssl
# (TLS stays Qt's job). Every packaging target already provides it --
# mingw64-openssl in the Windows cross image, libssl-dev on Debian, and
# openssl-devel on Fedora.
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# zlib for the updater helper's ZIP reader (portable Windows archives are
# deflate). Declared HERE, before the test targets below, because CMake is
# sequential: when this ran after them, ZLIB::ZLIB did not yet exist and
# updater-zip-safety-test was built WITHOUT LIGHTNING_UPDATER_HAVE_ZLIB. The
# suite then exercised only stored entries and silently skipped the inflate
# path that every real portable ZIP uses -- the shipped configuration was the
# untested one. Optional: without zlib the helper still handles stored entries
# and refuses deflated ones with a typed error.
find_package(ZLIB)

# The install/package type this build will report to the updater. Packaging
# sets it per artifact (windows-msi, linux-deb, ...); a source build stays
# "development", which can never install an update automatically.
set(LIGHTNING_INSTALL_TYPE "development" CACHE STRING
    "Package type reported to the updater (see src/update/InstallType.h)")

# Update-manifest trust root. Empty by default, and an empty key means
# verification fails closed -- a build with no key compiled in can check for
# updates but can never accept one. Official builds pass the real value.
set(LIGHTNING_UPDATE_PUBKEY_2026A "" CACHE STRING
    "Base64 Ed25519 public key for update-manifest key id lightning-release-2026a")
set(LIGHTNING_UPDATE_HOST "gitlab.smetonis.net" CACHE STRING
    "Canonical host: the ONLY source of update metadata, and the fallback for artifacts")
set(LIGHTNING_UPDATE_PROJECT_ID "6" CACHE STRING
    "GitLab project id hosting the update package registry")

# Hosts that may serve artifact BYTES in addition to the canonical host, so the
# GitHub mirror can carry the download traffic. This is deliberately narrow:
# these hosts may supply bytes that are then checked against the SHA-256 in the
# signed manifest, and nothing else. They can never serve the manifest, its
# signature or the release notes, and nothing they return influences which
# version is installed -- see docs/updates.md and src/update/UpdateEndpoints.h.
# github.com issues the redirect; the *.githubusercontent.com hosts serve the
# object it redirects to.
set(LIGHTNING_UPDATE_MIRROR_HOSTS
    "github.com;objects.githubusercontent.com;release-assets.githubusercontent.com"
    CACHE STRING
    "Semicolon-separated hosts allowed to serve update ARTIFACTS (never metadata)")

# The cache value is a CMake list, and a list expands to SEPARATE arguments
# inside target_compile_definitions -- "a;b;c" becomes -DX="a -Db -Dc", which
# gcc rejects outright ("macro names must be identifiers"). Hand the macro a
# comma-separated string instead; the parser in UpdateEndpoints.cpp accepts
# either separator. ($<SEMICOLON> does NOT help here: the split happens before
# the generator expression is evaluated. Verified both ways with a probe.)
string(REPLACE ";" "," LIGHTNING_UPDATE_MIRROR_HOSTS_MACRO
    "${LIGHTNING_UPDATE_MIRROR_HOSTS}")

# v0.7.3: where a MIRRORED copy of the signed manifest pair lives, used only
# when the canonical host cannot be reached. Empty disables the fallback and
# restores canonical-only metadata exactly. GitLab remains the authority: the
# canonical host is always tried first, the Ed25519 signature is verified
# identically whichever copy answered, and the client still refuses anything
# that is not strictly newer than what is installed -- so the worst a stale
# or hostile mirror can achieve here is to offer NO update, never a
# downgrade and never an unsigned one.
set(LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE
    "https://github.com/Mizerd/lightning/releases/download/update-latest"
    CACHE STRING
    "Base URL of the mirrored manifest pair, or empty to disable the fallback")

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
# lightning-matrix binary entirely, so a production package can only ever run the
# Rust SDK backend (no --backend=http / --mock, no silent fallback). OFF for
# ordinary dev/test builds, which keep mock/http buildable and testable per the
# architecture contract; project 7 packaging sets it ON. When ON it REQUIRES
# ENABLE_RUST_SDK_BACKEND=ON (enforced below) and only affects the main target —
# the mock-based test executables compile their own MockMatrixClient and are
# unaffected.
option(LIGHTNING_RUST_ONLY
    "Ship ONLY the Rust backend: drop HTTP/mock from the lightning-matrix binary" OFF)
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
#   * when OFF the lightning-matrix binary has no --screenshot-demo flag (preflight
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
# QPlatformScreen, for the X11 screen-share fallback's native screen
# rectangle. OPTIONAL and requested separately: a private module is only
# defined when it is asked for by name, and a Qt packaged without private
# headers simply will not have it. The one call site is guarded on
# LIGHTNING_HAVE_QPA_SCREEN below, so a Qt without it still builds and the
# fallback refuses honestly rather than capturing a guessed rectangle.
find_package(Qt6 6.5 QUIET COMPONENTS GuiPrivate)
# LOUD WHEN IT MATTERS. A Qt without private headers is a legitimate source
# build, and the fallback refuses honestly there. It is NOT legitimate in a
# package that means to ship the feature: the X11 picker would list nothing
# and every no-portal user would be told to install a portal, with no line
# anywhere saying why. That is precisely the silence that shipped 0.8.0's
# Linux packages with no media engine, so packaging opts into the same
# treatment it gets for GStreamer.
#
# Debian/Ubuntu: qt6-base-private-dev. Fedora: qt6-qtbase-private-devel.
# Both carry `qpa/qplatformscreen.h` AND the Qt6GuiPrivate CMake package;
# the plain -dev packages carry neither (verified in debian:13.6-slim and
# fedora:44).
if(LIGHTNING_REQUIRE_WEBRTC AND UNIX AND NOT APPLE
        AND NOT TARGET Qt6::GuiPrivate)
    message(FATAL_ERROR
        "LIGHTNING_REQUIRE_WEBRTC=ON but Qt6::GuiPrivate was not found. The "
        "X11 screen-share fallback needs QPlatformScreen for a screen's "
        "native rectangle, and without it this build would list no displays "
        "when the desktop has no xdg-desktop-portal. Install the Qt private "
        "development headers (Debian/Ubuntu: qt6-base-private-dev; Fedora: "
        "qt6-qtbase-private-devel).")
endif()
# v0.6.0 checkpoint 11: freedesktop notifications (Linux-first). Optional so
# non-DBus platforms still build; NotificationManager degrades to a no-op.
find_package(Qt6 6.5 COMPONENTS DBus)

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# Localization (Qt Linguist).
#
# The catalogs are hand-maintained .ts files under i18n/ and English is the
# SOURCE language, so there is deliberately no lightning_en.ts: selecting
# English installs no catalog and every qsTr() renders its own source string.
#
# LinguistTools is OPTIONAL on purpose. lrelease lives in a separate distro
# package from the Qt development headers (Debian: qt6-tools-dev-tools, not
# qt6-base-dev), and a hard requirement here would break every packaging job
# that has not added it. Without it the build succeeds and the application is
# English-only, LocalizationManager::translationsAvailable() reports false,
# and the Settings page says so instead of offering ten identical languages.
# ---------------------------------------------------------------------------
# `en` is in the list even though English is the SOURCE language: Qt renders
# a `%n room(s)` source string's "(s)" literally when no catalog is loaded, so
# English needs a catalog carrying its plural forms and (after
# -removeidentical) essentially nothing else.
set(LIGHTNING_LANGUAGE_CODES en zh_CN hi es ar fr bn pt ru id)
set(LIGHTNING_TS_FILES "")
foreach(_lang IN LISTS LIGHTNING_LANGUAGE_CODES)
    list(APPEND LIGHTNING_TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/i18n/lightning_${_lang}.ts")
endforeach()

find_package(Qt6 6.5 QUIET COMPONENTS LinguistTools)
set(LIGHTNING_QM_FILES "")
set(LIGHTNING_TRANSLATIONS_ENABLED OFF)
if(Qt6LinguistTools_FOUND)
    set(LIGHTNING_QM_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n")
    file(MAKE_DIRECTORY "${LIGHTNING_QM_DIR}")
    foreach(_ts IN LISTS LIGHTNING_TS_FILES)
        get_filename_component(_base "${_ts}" NAME_WE)
        set(_qm "${LIGHTNING_QM_DIR}/${_base}.qm")
        # -removeidentical drops entries whose translation equals the source
        # (which would cost bytes and buy nothing); -nounfinished keeps
        # UNFINISHED entries out of the binary catalog, so a half-translated
        # string falls back to English instead of shipping a draft.
        add_custom_command(
            OUTPUT "${_qm}"
            COMMAND Qt6::lrelease -removeidentical -nounfinished -silent "${_ts}" -qm "${_qm}"
            DEPENDS "${_ts}"
            COMMENT "lrelease ${_base}.qm"
            VERBATIM)
        list(APPEND LIGHTNING_QM_FILES "${_qm}")
    endforeach()
    add_custom_target(lightning-translations ALL DEPENDS ${LIGHTNING_QM_FILES})
    set_source_files_properties(${LIGHTNING_QM_FILES} PROPERTIES GENERATED TRUE)
    set(LIGHTNING_TRANSLATIONS_ENABLED ON)

    # Manual only — never wired into ALL. lupdate REWRITES the tracked .ts
    # files, so running it as a side effect of an ordinary build would edit
    # the working tree under the developer. See docs/localization.md.
    add_custom_target(update-translations
        COMMAND Qt6::lupdate
            -locations relative -no-obsolete
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/qml"
            -ts ${LIGHTNING_TS_FILES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "lupdate: refreshing i18n/*.ts from src/ and qml/"
        VERBATIM)
    message(STATUS "Localization: enabled (${LIGHTNING_LANGUAGE_CODES})")
else()
    message(WARNING
        "Qt6 LinguistTools not found — building WITHOUT translations. The UI "
        "will be English-only regardless of the language setting. Install the "
        "Qt Linguist tools (Debian/Ubuntu: qt6-tools-dev-tools) to enable it.")
endif()

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

# Optional GStreamer webrtcbin voice-call media engine (v0.7.x round 3).
# AUTO: built when the GStreamer WebRTC dev files are present. Packaged
# builds without them keep CallController's honest no-media refusal — the
# engine additionally re-probes its element factories at RUNTIME before it
# is ever registered.
option(LIGHTNING_ENABLE_WEBRTC
    "Build the GStreamer webrtcbin voice-call media engine when available" ON)
# REQUIRE it, for a build that must not silently ship without calling.
#
# The option above is AUTO by design: a source build on a machine with no
# GStreamer should still produce a working client, and the runtime refusal is
# honest. But "available" is decided by a pkg-config probe that says STATUS
# and carries on, and a packaging job cannot hear the difference between "we
# looked and it is not there" and "we forgot to install it". Lightning 0.8.0
# shipped every Linux package with the media engine compiled out for exactly
# that reason, and the artifacts said `call media engine built in: no` while
# every job reported success.
#
# So a build that INTENDS to ship calling passes -DLIGHTNING_REQUIRE_WEBRTC=ON
# and the probe becomes fatal. Packaging turns it on; a developer's source
# build leaves it off and keeps the graceful degradation.
option(LIGHTNING_REQUIRE_WEBRTC
    "Fail configuration if the GStreamer WebRTC dev files are missing" OFF)
set(HAVE_LIGHTNING_WEBRTC OFF)
if(LIGHTNING_ENABLE_WEBRTC AND PkgConfig_FOUND)
    # gstreamer-app-1.0 is for the received-video appsink: decoded frames are
    # pulled out and pushed into a QML VideoOutput's QVideoSink, because the
    # pinned toolchain ships no qml6glsink and a package cannot depend on a
    # plugin that may be absent.
    pkg_check_modules(GSTWEBRTC IMPORTED_TARGET
        gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0
        gstreamer-app-1.0
        # gstreamer-video-1.0: the force-key-unit event a PLI becomes.
        gstreamer-video-1.0
        # gstreamer-rtp-1.0: Lightning's own VP8 payloader subclasses
        # GstRTPBasePayload, because rtpvp8pay parses the bitstream and so
        # cannot payload an end-to-end encrypted frame (RtpVp8Payloader.h).
        gstreamer-rtp-1.0)
    if(GSTWEBRTC_FOUND)
        set(HAVE_LIGHTNING_WEBRTC ON)
        message(STATUS "GStreamer WebRTC found (${GSTWEBRTC_gstreamer-webrtc-1.0_VERSION}) — building the voice-call media engine")
    elseif(LIGHTNING_REQUIRE_WEBRTC)
        message(FATAL_ERROR
            "LIGHTNING_REQUIRE_WEBRTC=ON but the GStreamer WebRTC development "
            "files were not found. This build would ship with calling, screen "
            "sharing and camera compiled out, and would answer \"Calling isn't "
            "available here.\" Install the development packages for "
            "gstreamer-1.0, gstreamer-webrtc-1.0, gstreamer-sdp-1.0, "
            "gstreamer-app-1.0, gstreamer-video-1.0 and gstreamer-rtp-1.0, or "
            "configure with -DLIGHTNING_REQUIRE_WEBRTC=OFF to accept a build "
            "with no media engine.")
    else()
        message(STATUS "GStreamer WebRTC not found — voice calls stay signaling-only (honest refusal)")
    endif()
elseif(LIGHTNING_REQUIRE_WEBRTC)
    # Both switches set, contradicting each other. Refusing beats guessing
    # which one the caller meant.
    message(FATAL_ERROR
        "LIGHTNING_REQUIRE_WEBRTC=ON contradicts LIGHTNING_ENABLE_WEBRTC=OFF "
        "(or PkgConfig is unavailable). Pick one.")
endif()

set(APP_SOURCES
    src/main.cpp

    src/app/AppController.h
    src/app/AppController.cpp
    src/app/ConversationController.h
    src/app/ConversationController.cpp
    src/app/CustomAppIcon.h
    src/app/CustomAppIcon.cpp
    src/app/CustomThemeStore.h
    src/app/CustomThemeStore.cpp
    src/app/FontManager.h
    src/app/FontManager.cpp
    src/app/StormBandPainter.h
    src/app/StormBandPainter.cpp
    src/app/RoomInfoController.h
    src/app/RoomInfoController.cpp
    src/app/PinnedMessagesController.h
    src/app/PinnedMessagesController.cpp
    src/app/RoomUpgradeController.h
    src/app/RoomUpgradeController.cpp
    src/app/RoomDiscoveryController.h
    src/app/RoomDiscoveryController.cpp
    src/app/UiaController.h
    src/app/UiaController.cpp
    src/app/ModerationController.h
    src/app/ModerationController.cpp
    src/app/ForwardController.h
    src/app/ForwardController.cpp
    src/app/DraftStore.h
    src/app/DraftStore.cpp
    src/app/RichComposerBridge.h
    src/app/RichComposerBridge.cpp
    src/app/SessionDiagnostics.h
    src/app/SessionDiagnostics.cpp
    src/app/GuiStallTracer.h
    src/app/GuiStallTracer.cpp
    src/app/ShortcutRegistry.h
    src/app/ShortcutRegistry.cpp
    src/app/SyncLatencyTracer.h
    src/app/SyncLatencyTracer.cpp

    src/calls/CallController.h
    src/calls/CallController.cpp
    src/matrix/RtcSession.h
    src/calls/RtcController.h
    src/calls/RtcController.cpp
    src/calls/SfuCallController.h
    src/calls/SfuCallController.cpp
    src/calls/CallParticipantModel.h
    src/calls/CallParticipantModel.cpp
    src/calls/CallShareModel.h
    src/calls/CallShareModel.cpp
    src/calls/CallStageState.h
    src/calls/CallStageState.cpp
    src/calls/CallDeviceController.h
    src/calls/CallDeviceController.cpp
    src/calls/ScreenCastPortal.h
    src/calls/ScreenCastPortal.cpp
    # Header only: AUTOMOC supplies the CallMediaBackend metaobject; no
    # implementation exists in the tree (the media seam — tests use a fake).
    src/calls/CallMediaBackend.h
    src/calls/SdpStore.h
    src/app/SettingsManager.h
    src/app/SettingsManager.cpp

    src/i18n/LocalizationManager.h
    src/i18n/LocalizationManager.cpp

    src/auth/AuthManager.h
    src/auth/AuthManager.cpp
    src/auth/AccountManager.h
    src/auth/AccountManager.cpp
    # Loopback redirect endpoint for OAuth browser sign-in. Built into every
    # configuration: it is plain Qt Network with no Rust dependency, and the
    # Rust backend is the only thing that drives it.
    src/auth/OAuthCallbackServer.h
    src/auth/OAuthCallbackServer.cpp

    src/matrix/CallSignal.h
    src/matrix/MatrixClient.h
    src/matrix/MatrixClient.cpp
    src/matrix/MockMatrixClient.h
    src/matrix/MockMatrixClient.cpp
    src/matrix/CppHttpMatrixClient.h
    src/matrix/CppHttpMatrixClient.cpp
    src/matrix/BridgeNetwork.h
    src/matrix/BridgeNetwork.cpp
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
    src/storage/PortableMode.h
    src/storage/PortableMode.cpp
    src/storage/CacheStore.h
    src/storage/CacheStore.cpp
    src/storage/SecretStore.h
    src/storage/SecretStore.cpp
    src/storage/PortableSecretStore.h
    src/storage/PortableSecretStore.cpp
    src/storage/InsecureFallbackSecretStore.h
    src/storage/InsecureFallbackSecretStore.cpp
    src/storage/LibSecretStore.h
    src/storage/LibSecretStore.cpp
    src/storage/WinCredStore.h
    src/storage/WinCredStore.cpp

    src/models/RoomListModel.h
    src/models/RoomListModel.cpp
    src/models/DirectAvatarResolver.h
    src/models/DirectAvatarResolver.cpp
    src/models/SpaceChannelModel.h
    src/models/SpaceChannelModel.cpp
    src/models/QuickSwitcherModel.h
    src/models/QuickSwitcherModel.cpp
    src/models/TimelineModel.h
    src/models/TimelineModel.cpp
    src/models/ReverseListProxyModel.h
    src/models/ReverseListProxyModel.cpp
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
    src/models/SlashCommands.h
    src/models/SlashCommands.cpp
    src/models/RichComposition.h
    src/models/RichComposition.cpp
    src/models/EmojiCatalog.h
    src/models/EmojiCatalog.cpp
    src/models/AttachmentQueueModel.h
    src/models/AttachmentQueueModel.cpp
    src/models/UserSearchModel.h
    src/models/UserSearchModel.cpp
    src/models/RoomDirectorySearchModel.h
    src/models/RoomDirectorySearchModel.cpp
    src/models/MessageSearchController.h
    src/models/MessageSearchController.cpp
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
    src/gif/GifStarredModel.h
    src/gif/GifStarredModel.cpp
    src/gif/GifSavedModel.h
    src/gif/GifSavedModel.cpp
    src/gif/GifStarredStore.h
    src/gif/GifStarredStore.cpp
    src/gif/GifTransport.h
    src/gif/GifSearchController.h
    src/gif/GifSearchController.cpp
    src/gif/GifSendController.h
    src/gif/GifSendController.cpp
    src/gif/GifProviderSelfTest.h
    src/gif/GifProviderSelfTest.cpp
    src/gif/MatrixGifTransport.h
    src/gif/MatrixGifTransport.cpp

    # MSC2545 image packs: stickers and custom emoji.
    src/stickers/StickerPack.h
    src/stickers/StickerPack.cpp
    src/stickers/StickerPackModel.h
    src/stickers/StickerPackModel.cpp
    src/stickers/StickerImageModel.h
    src/stickers/StickerImageModel.cpp
    src/stickers/StickerPackManager.h
    src/stickers/StickerPackManager.cpp

    src/notifications/NotificationManager.h
    src/notifications/NotificationManager.cpp
    src/notifications/FallbackAvatar.h
    src/notifications/FallbackAvatar.cpp
    src/theme/IdentityColors.h
    src/theme/IdentityColors.cpp
    src/theme/IdentityPalette.h
    src/media/MediaManager.h
    src/media/MediaManager.cpp
    src/app/UrlLauncher.h
    src/app/UrlLauncher.cpp
    src/media/MediaBridge.h
    src/media/MediaBridge.cpp
    src/media/PlayableFileWriter.h
    src/media/PlayableFileWriter.cpp
    src/media/MediaImageProvider.h
    src/media/MediaImageProvider.cpp
    src/media/MediaVisibilityStore.h
    src/media/MediaVisibilityStore.cpp
    src/app/TrayIcon.h
    src/app/TrayIcon.cpp
    src/spaces/RailLayoutStore.h
    src/spaces/RailLayoutStore.cpp
    src/spaces/RailEntryModel.h
    src/spaces/RailEntryModel.cpp
    src/profile/ProfileBannerManager.h
    src/profile/ProfileBannerManager.cpp
    src/profile/ProfileBioManager.h
    src/profile/ProfileBioManager.cpp
    src/profile/ProfileBadges.h
    src/profile/ProfileBadges.cpp
    src/media/StagedImageStore.h
    src/media/StagedImageStore.cpp
    src/media/StagedImageProvider.h
    src/media/StagedImageProvider.cpp
    src/media/ImageCropper.h
    src/media/ImageCropper.cpp
    src/media/MediaPlaybackController.h
    src/media/MediaPlaybackController.cpp
    src/media/VideoPosterExtractor.h
    src/media/VideoPosterExtractor.cpp
    src/media/VoiceRecorder.h
    src/media/VoiceRecorder.cpp
    src/crypto/CryptoManager.h
    src/crypto/CryptoManager.cpp
    src/crypto/CryptoHealthModel.h
    src/crypto/CryptoHealthModel.cpp
    src/crypto/BackupController.h
    src/crypto/BackupController.cpp
    src/models/ScheduledSendController.h
    src/models/ScheduledSendController.cpp
    src/models/ActivityModel.h
    src/models/ActivityModel.cpp
    src/crypto/CryptoBootstrapModel.h
    src/crypto/CryptoBootstrapModel.cpp
    src/crypto/E2eeDiagnostics.h
    src/crypto/E2eeDiagnostics.cpp
    src/crypto/QrImageProvider.h
    src/crypto/QrImageProvider.cpp
    src/spaces/SpaceManager.h
    src/spaces/SpaceManager.cpp
    src/threads/ThreadManager.h
    src/threads/ThreadManager.cpp
    src/threads/ThreadController.h
    src/threads/ThreadController.cpp
    src/presence/PresenceManager.h
    src/presence/PresenceManager.cpp

    # Application updates. Deliberately free of every Matrix dependency: the
    # updater is a separate trust boundary and must not be able to reach an
    # account, a token or a store. See docs/updates.md.
    src/update/Version.h
    src/update/Version.cpp
    src/update/InstallType.h
    src/update/InstallType.cpp
    src/update/UpdateTrustStore.h
    src/update/UpdateTrustStore.cpp
    src/update/SignatureVerifier.h
    src/update/SignatureVerifier.cpp
    src/update/UpdateEndpoints.h
    src/update/UpdateEndpoints.cpp
    src/update/UpdateManifest.h
    src/update/UpdateManifest.cpp
    src/update/UpdateDownloader.h
    src/update/UpdateDownloader.cpp
    src/update/UpdateManager.h
    src/update/UpdateManager.cpp
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
if(HAVE_LIGHTNING_WEBRTC)
    list(APPEND APP_SOURCES
        # ONE GStreamer init for the process, with the bundled plugin path
        # applied BEFORE it. Both backends below used to init independently
        # and only one of them set the path, so whichever was probed first
        # decided whether a packaged build had a plugin registry at all.
        src/calls/GstBootstrap.h
        src/calls/GstBootstrap.cpp
        src/calls/GstCallMediaBackend.h
        src/calls/GstCallMediaBackend.cpp
        src/calls/SfuMediaEngine.h
        src/calls/SfuMediaEngine.cpp
        src/calls/RtpVp8Payloader.h
        src/calls/RtpVp8Payloader.cpp
        # Window capture, ours because nothing shippable does it: the only
        # capture element in the Windows SDK set takes a monitor index, and
        # the one with a window handle is the UCRT-built d3d11 plugin this
        # project cannot link against. Compiles to nothing off Windows.
        src/calls/WindowCaptureSrc.h
        src/calls/WindowCaptureSrc.cpp
        # Serves those captures as preview tiles to the picker.
        src/calls/ShareSourceImageProvider.h
        src/calls/ShareSourceImageProvider.cpp
        # Only reachable through the SFU engine, and the one file that needs
        # OpenSSL directly — keeping it here rather than in the shared app
        # sources avoids linking libcrypto into every test target.
        src/calls/CallFrameCryptor.h
        src/calls/CallFrameCryptor.cpp
        # Received video: the pipeline decoded frames and dropped them into a
        # fakesink until this existed.
        src/calls/SfuVideoRouter.h
        src/calls/SfuVideoRouter.cpp)
endif()

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
    # 2026-08-21 control-system round: themed replacements for the Qt Basic
    # controls that were reaching production unstyled — square-cornered
    # dialogs with grey stock buttons, body-text-grey progress bars and
    # spinners, and a scrollbar that never saw a Lightning colour.
    qml/AppDialog.qml
    qml/ImageCropDialog.qml
    qml/AppProgressBar.qml
    qml/AppBusyIndicator.qml
    qml/AppScrollBar.qml
    qml/LoginScreen.qml
    qml/SettingsScreen.qml
    qml/SmoothWheelArea.qml
    qml/MiddleClickScroller.qml
    qml/VoicePreviewBar.qml
    qml/UpdatesSettingsSection.qml
    qml/UpdateAvailableDialog.qml
    qml/MainScreen.qml
    qml/HomePane.qml
    qml/SpacesRail.qml
    qml/FolderTile.qml
    qml/RoomsPanel.qml
    qml/TimelinePane.qml
    qml/RoomDelegate.qml
    # The room row context menu, shared by both navigation layouts.
    qml/RoomActionsMenu.qml
    # 2026-08-23 navigation layouts: the room-list column is now a host with
    # two interchangeable presenters.
    qml/RoomListClassicPresenter.qml
    qml/RoomChannelsPresenter.qml
    qml/ChannelDelegate.qml
    qml/ChannelCategoryHeader.qml
    qml/ChannelNavRow.qml
    # A Space IS a Matrix room: this is the room-settings backend behind a
    # Space-shaped surface, reached from the rail's menu.
    qml/SpaceSettingsDialog.qml
    qml/UnreadBadge.qml
    # "There is a call in this room", shared by BOTH room-row layouts. One
    # component because the rule — and what it may cost — is the same in both,
    # and it must never ask RtcController to refresh (that falls back to a
    # full /state per idle room).
    qml/RoomCallGlyph.qml
    # Choose which display to share, on the platforms with no xdg portal.
    # Never opens on Linux — the portal owns the picker there.
    qml/ScreenSharePicker.qml
    qml/NavigationLayoutCard.qml
    qml/MessageDelegate.qml
    # Element-style local image hiding: keeps the media box's exact geometry.
    qml/MediaHiddenPlaceholder.qml
    qml/RoomActivityDelegate.qml
    qml/ShortcutRow.qml
    qml/MessageComposerBar.qml
    qml/MentionPopup.qml
    qml/SlashCommandPopup.qml
    qml/RoomUpgradeDialog.qml
    qml/EditHistoryDialog.qml
    qml/EventSourceDialog.qml
    qml/StatusDialog.qml
    qml/SendLaterDialog.qml
    qml/ActivityCenterPanel.qml
    qml/AnchoredPopup.qml
    qml/PopupResizeGrip.qml
    qml/EmojiPicker.qml
    qml/CreatePollDialog.qml
    qml/VideoPlayerCard.qml
    qml/AudioPlayerCard.qml
    qml/MediaVolumeControl.qml
    qml/VideoControlBar.qml
    qml/VideoViewerOverlay.qml
    qml/UserPicker.qml
    qml/NewConversationDialog.qml
    qml/DiscoverJoinDialog.qml
    qml/MessageSearchDialog.qml
    qml/SearchPanel.qml
    qml/SearchMemberSection.qml
    qml/MediaListThumbnail.qml
    qml/InvitePeopleDialog.qml
    qml/RoomInfoPanel.qml
    qml/ThreadPanel.qml
    qml/ThreadSummaryCard.qml
    qml/ThemePreviewDemo.qml
    qml/ThemeEditorDialog.qml
    qml/ColorPickerPanel.qml
    qml/GifPicker.qml
    qml/StickerPicker.qml
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
    qml/PresenceDot.qml
    qml/VerificationPanel.qml
    qml/VerificationDialog.qml
    qml/UiaPromptDialog.qml
    qml/ReportMessageDialog.qml
    qml/ForwardMessageDialog.qml
    qml/UpdateAvailablePrompt.qml
    qml/IncomingCallPrompt.qml
    qml/CallControlButton.qml
    qml/CallDeviceChevron.qml
    qml/CallDeviceMenu.qml
    qml/CallShareOptionsMenu.qml
    qml/CallDeviceSettings.qml
    qml/CallHeaderBar.qml
    qml/CallTileBadge.qml
    qml/CallParticipantTile.qml
    qml/CallSpeakerBubbles.qml
    qml/CallStage.qml
    qml/CallTileGrid.qml
    qml/CallShareTile.qml
    qml/VoiceConnectedBar.qml
    qml/RoomCallBanner.qml
    qml/CallEventDelegate.qml
    qml/VerifySessionPrompt.qml
    qml/QuickReactionStrip.qml
    qml/IdentityCard.qml
    qml/TrustCard.qml
    qml/StormBand.qml
    qml/StormNode.qml
    qml/TrustMeter.qml
    qml/CodeBlock.qml
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
        # v0.6.7: animated fixtures for the GIF picker's grid, derived from the
        # stills above by scripts/generate-demo-gifs.sh (same license, same
        # provenance). Demo mode has no network and no provider key, so the
        # picker can only be photographed against bundled frames.
        resources/screenshot-demo/gif-coast.gif
        resources/screenshot-demo/gif-city.gif
        resources/screenshot-demo/gif-artwork.gif
        resources/screenshot-demo/gif-portrait.gif
        resources/screenshot-demo/gif-interface.gif
        resources/screenshot-demo/gif-square.gif
        resources/screenshot-demo/gif-palette.gif
        resources/screenshot-demo/release-notes.txt
    )
endif()

# Everything (app sources + compiled QML module) that a Qt executable needs
# to run the real UI, factored into a function so timeline-pane-qml-test can
# build a second binary wired up identically to lightning-matrix — a plain
# static "backing" library here proved unable to get Qt's static QML plugin
# auto-import to run reliably across target boundaries, so each Qt-executable
# target instead calls qt_add_qml_module on itself directly (the arrangement
# Qt documents and the one that's actually verified to work).
function(_lightning_configure_qml_module target _scope)
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

    target_include_directories(${target} ${_scope} src)

    # Compiled translation catalogs, embedded so a packaged application
    # carries them without a data directory. LocalizationManager loads
    # ":/i18n/lightning_<code>.qm". The resource NAME is per-target because
    # every Qt-executable target registers its own copy of the QML module.
    if(LIGHTNING_TRANSLATIONS_ENABLED)
        qt_add_resources(${target} "lightning_i18n_${target}"
            PREFIX "/i18n"
            BASE "${LIGHTNING_QM_DIR}"
            FILES ${LIGHTNING_QM_FILES})
        add_dependencies(${target} lightning-translations)
        target_compile_definitions(${target} ${_scope} LIGHTNING_HAS_TRANSLATIONS=1)
    endif()
    # qmltyperegistrar's generated qml_register_types_MatrixClient() probes
    # for each QML_ELEMENT header with #if __has_include(<HeaderName.h>) — a
    # bare filename, not the "models/HeaderName.h" path used by quoted
    # includes elsewhere. Add each subdirectory holding a QML-registered type
    # so that angle-bracket lookup resolves.
    target_include_directories(${target} ${_scope} src/models src/threads src/crypto src/gif src/update src/calls src/theme src/stickers)
    # Build-only GIF key header (may be keyless); include path only, no values.
    target_include_directories(${target} ${_scope} "${LIGHTNING_GENERATED_DIR}")

    target_link_libraries(${target} ${_scope}
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Network
        Qt6::Sql
        Qt6::Widgets
        Qt6::Multimedia
        OpenSSL::Crypto
    )
    # The X11 screen-share fallback needs one thing no public Qt API exposes:
    # a screen's NATIVE, root-relative rectangle. `QScreen::geometry()` cannot
    # be converted into one -- Qt leaves its top-left in native pixels and
    # scales only the SIZE, and `devicePixelRatio()` is a rounded presentation
    # value rather than the native/logical factor (measured on a real 2x4K
    # desktop: dpr 2.00 where the true scale was 1.5). `QScreen::handle()`
    # returns it exactly, and that needs QPlatformScreen's declaration.
    #
    # INTERFACE-ONLY: Qt6::GuiPrivate adds private include directories and
    # links Qt6::Gui. It introduces no new shared library and therefore no
    # packaging change. Linux only, which is the only platform whose capture
    # element addresses a rectangle at all -- Windows and macOS take a display
    # INDEX, so their translation units never see a private Qt header.
    if(UNIX AND NOT APPLE AND TARGET Qt6::GuiPrivate)
        target_link_libraries(${target} ${_scope} Qt6::GuiPrivate)
        target_compile_definitions(${target} ${_scope}
            LIGHTNING_HAVE_QPA_SCREEN=1)
    endif()

    # Update trust configuration. These are the ONLY values the updater takes
    # from the build; everything else about an update is decided by compiled-in
    # code, never by remote metadata.
    target_compile_definitions(${target} ${_scope}
        LIGHTNING_INSTALL_TYPE="${LIGHTNING_INSTALL_TYPE}"
        LIGHTNING_UPDATE_PUBKEY_2026A="${LIGHTNING_UPDATE_PUBKEY_2026A}"
        LIGHTNING_UPDATE_HOST="${LIGHTNING_UPDATE_HOST}"
        LIGHTNING_UPDATE_MIRROR_HOSTS="${LIGHTNING_UPDATE_MIRROR_HOSTS_MACRO}"
        LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE="${LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE}"
        LIGHTNING_UPDATE_PROJECT_ID="${LIGHTNING_UPDATE_PROJECT_ID}"
    )
    # DBus is a Linux/BSD desktop concept: freedesktop notifications and the
    # session bus. Gate on platform as well as availability, so a Windows Qt
    # that happens to ship the Qt6 DBus module does NOT compile the DBus path in
    # and try to autolaunch a session bus at startup (the "dbus-1" lookup noise
    # observed on native Windows). NotificationManager degrades to a no-op.
    if(TARGET Qt6::DBus AND NOT WIN32 AND NOT APPLE)
        target_link_libraries(${target} ${_scope} Qt6::DBus)
        target_compile_definitions(${target} ${_scope} HAVE_QT_DBUS=1)
    endif()

    target_compile_definitions(${target} ${_scope}
        APP_VERSION="${APP_VERSION_LABEL}"
        LIGHTNING_SOURCE_SHA="${LIGHTNING_SOURCE_SHA}"
        LIGHTNING_BUILD_TARGET="${LIGHTNING_BUILD_TARGET}"
        LIGHTNING_ARTIFACT_KIND="${LIGHTNING_ARTIFACT_KIND}"
        LIGHTNING_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
    )

    if(HAVE_LIBSECRET)
        target_link_libraries(${target} ${_scope} PkgConfig::LIBSECRET)
        target_compile_definitions(${target} ${_scope} HAVE_LIBSECRET=1)
    endif()

    if(HAVE_LIGHTNING_WEBRTC)
        target_link_libraries(${target} ${_scope} PkgConfig::GSTWEBRTC)
        target_compile_definitions(${target} ${_scope} HAVE_LIGHTNING_WEBRTC=1)
    endif()

    # Windows Credential Manager backend for SecretStore (CredWriteW/ReadW/
    # DeleteW/EnumerateW live in advapi32). WIN32 is true for both MSVC and the
    # MinGW cross-build; off Windows WinCredStore compiles as a no-op stub.
    if(WIN32)
        # gdi32/user32 for the WINDOW CAPTURE source (WindowCaptureSrc.cpp):
        # CreateDIBSection, BitBlt, StretchBlt, GdiFlush, PrintWindow.
        #
        # NOT implied. Measured in the cross-builder: the same translation
        # unit links with SEVEN undefined references without -lgdi32 and zero
        # with it, so relying on Qt to drag them in transitively would be a
        # link failure waiting for the next Qt version to stop doing it.
        # dwmapi is deliberately absent — WindowCaptureSrc resolves
        # DwmGetWindowAttribute with GetProcAddress so the packaging closure
        # gains no new import.
        target_link_libraries(${target} ${_scope} advapi32 gdi32 user32)
        target_compile_definitions(${target} ${_scope} HAVE_WINCRED=1)
    endif()

    if(ENABLE_RUST_SDK_BACKEND)
        add_dependencies(${target} matrix-client-rust-build)
        target_link_libraries(${target} ${_scope} matrix-client-rust SQLite::SQLite3)
        if(UNIX AND NOT APPLE)
            # Rust static libs typically pull in libdl / libpthread / libm at
            # some scale — link them defensively so growing the Rust surface
            # later does not surprise us with link-time failures.
            target_link_libraries(${target} ${_scope} dl pthread m)
        endif()

        target_compile_definitions(${target} ${_scope} ENABLE_RUST_SDK_BACKEND=1)

        # v0.5.0-prep+9: initial E2EE gate. Both directions verified live
        # against matrix.smetonis.net (Element Classic ↔ Lightning marker
        # round-trip: expect_text=seen + encrypted_send=ok). Flip so
        # CryptoManager::supportsE2ee() returns true for the Rust backend.
        # HTTP and Mock still return false regardless.
        target_compile_definitions(${target} ${_scope} RUST_SDK_E2EE_WIRED=1)
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()
endfunction()

# The shipped executable keeps everything target-private; the shared test
# library (below) needs the same configuration to PROPAGATE to the test
# executables that link it, so the visibility is a parameter.
function(lightning_configure_qml_app target)
    _lightning_configure_qml_module(${target} PRIVATE)
endfunction()

# Test executables link the one prebuilt copy of the application instead of
# recompiling all of APP_LIB_SOURCES per target. Linking the generated
# ...plugin target is what runs the static QML module's type registration.
function(lightning_configure_qml_test target)
    # Qt runs qmlimportscanner once per QML-linking executable AT CONFIGURE
    # TIME, serially: 51 invocations cost 119.6s of a 121s configure here
    # (measured with cmake --profiling-format=google-trace). The scan exists to
    # link static QML plugins; against a SHARED Qt build the plugins are
    # resolved at runtime instead, which is why Qt itself skips the scan for
    # its own test executables in shared builds. Our tests are never deployed,
    # so skipping it changes nothing they rely on -- the MatrixClient module is
    # linked explicitly through lightning-app-testlibplugin above.
    set_target_properties(${target} PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    target_link_libraries(${target} PRIVATE
        lightning-app-testlib
        lightning-app-testlibplugin
        Qt6::Test)
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

    # Enumerate the crate's sources instead of hand-listing them. The previous
    # explicit list named 5 of the 11 files in rust/src, so editing oauth.rs,
    # search.rs, uia.rs, discover.rs, ignore.rs or gifs.rs did NOT re-run cargo
    # and the build silently linked a stale libmatrix_client_rust.a.
    # CONFIGURE_DEPENDS re-runs the glob when a source is added or removed.
    file(GLOB_RECURSE _rust_sources CONFIGURE_DEPENDS "${_rust_dir}/src/*.rs")

    add_custom_command(
        OUTPUT ${_rust_staticlib}
        COMMAND ${CMAKE_COMMAND} -E env
            CARGO_TARGET_DIR=${_rust_out_dir}
            ${CARGO_EXECUTABLE} build ${_cargo_profile_flag}
                --offline --locked
                --manifest-path ${_rust_dir}/Cargo.toml
        DEPENDS ${_rust_sources}
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

# The application target and binary: `lightning-matrix`. The product is
# "Lightning"; packaging gives the installed artifact its platform name
# (Lightning.exe, Lightning.app). The persistent application identity
# (QSettings organization/application names, store paths) is a separate,
# stable literal in src/main.cpp and does not follow this name.
qt_add_executable(lightning-matrix ${APP_MAIN_SOURCES})
lightning_configure_qml_app(lightning-matrix)
# Per-target (never on the shared mock-based test targets): drive the C++
# #ifdef guards in AppController and the --build-info self-report.
if(LIGHTNING_RUST_ONLY)
    target_compile_definitions(lightning-matrix PRIVATE LIGHTNING_RUST_ONLY=1)
endif()
# Development-only screenshot demo: drives the #ifdef guards in main.cpp /
# AppController and the --build-info self-report. Only ever set on the shipped
# target in a dedicated development build (never a release; the option guard
# above rejects combining it with LIGHTNING_RUST_ONLY).
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
    target_compile_definitions(lightning-matrix PRIVATE LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
    # v0.6.7: also define it TREE-WIDE in this configuration. Demo sources are
    # gated by being added to the source list at all, but they are then
    # compiled into every Qt-executable target here (the qml-app targets each
    # rebuild the app sources), while this define reached only lightning-matrix.
    # Any demo-only API guarded by the macro therefore existed in one target
    # and not the others, and a caller in a shared source failed to compile —
    # which is exactly what the GIF demo catalogue hit.
    #
    # This is still development-only: the option defaults OFF and is rejected
    # outright for a Rust-only release build, so no release artifact sees it.
    add_compile_definitions(LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
endif()

# On Windows the production binary is a GUI-subsystem PE so a normal
# double-click never flashes or leaves a console window. --version / --help /
# --build-info still print to a parent console when launched from one (main.cpp
# attaches to it). Only the production executable gets this — test binaries stay
# console-subsystem so CTest captures their output.
if(WIN32)
    set_target_properties(lightning-matrix PROPERTIES WIN32_EXECUTABLE ON)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    # ------------------------------------------------------------------
    # One prebuilt copy of the application for the Qt-executable tests.
    #
    # Every one of these tests used to list ${APP_LIB_SOURCES} directly, so
    # each test executable recompiled all ~150 application translation units
    # and registered its own private copy of the MatrixClient QML module.
    # Measured from build-rust/.ninja_log: that was 601 of the 621 CPU-minutes
    # of compilation in a full build (32 targets x ~18.6 min each), against
    # 19.6 CPU-minutes for lightning-matrix itself.
    #
    # Building it once as a static QML module library and linking it keeps
    # every test's behaviour identical -- same sources, same defines, same
    # module URI -- while compiling them once. This lives entirely inside
    # BUILD_TESTING, and every packaging build configures with
    # -DBUILD_TESTING=OFF, so no release artifact is affected.
    qt_add_library(lightning-app-testlib STATIC ${APP_LIB_SOURCES})
    _lightning_configure_qml_module(lightning-app-testlib PUBLIC)
    target_link_libraries(lightning-app-testlib PUBLIC Qt6::Test)

    # Consumed above. Blanking it here is what makes every `${APP_LIB_SOURCES}`
    # in the test targets below expand to NOTHING, so each of those
    # qt_add_executable() calls compiles only the test's own translation unit
    # and picks the application up by linking lightning-app-testlib. If you are
    # reading one of those targets and wondering where its ~150 sources went:
    # this line is the answer.
    set(APP_LIB_SOURCES "")

    # SignPath Foundation OSS code-signing prerequisites that live in this
    # repository: the Code signing policy document and its required attribution
    # and roles, the privacy policy, both being linked from the project home
    # page, one canonical release version across CMake/Cargo/user agent, the
    # honest "unsigned" disclosure, and no committed SignPath secrets. Needs no
    # compiler, so it runs in both build trees.
    add_test(NAME signpath-compliance
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/SignPathComplianceTest.cmake")

    qt_add_executable(app-data-paths-test
        tests/AppDataPathsTest.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(app-data-paths-test PRIVATE src)
    target_link_libraries(app-data-paths-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME app-data-paths COMMAND app-data-paths-test)

    # 2026-08-21: Windows true portability. Portable mode has to be decided
    # BEFORE the first QSettings exists — after that the registry has already
    # been touched and redirecting would leave state in two places — so the
    # decision runs where there is no QCoreApplication and the executable
    # directory comes from a platform API rather than applicationDirPath().
    # The POLICY carries no platform guard, which is what lets all of it be
    # tested here. The relocation cases are the point of the whole feature:
    # state written under one root must resolve from another, with nothing
    # retaining the old absolute path.
    qt_add_executable(portable-mode-test
        tests/PortableModeTest.cpp
        src/storage/PortableMode.cpp
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(portable-mode-test PRIVATE src)
    target_link_libraries(portable-mode-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME portable-mode COMMAND portable-mode-test)

    # The portable session store. Deliberately does NOT compile PortableMode:
    # the store takes its directory as a constructor argument, so this suite
    # is hermetic and needs neither the portable decision nor its test seam.
    # SecretStore.h only — SecretStore.cpp would drag in the WinCred and
    # libsecret backends this suite exists to be independent of.
    qt_add_executable(portable-secret-store-test
        tests/PortableSecretStoreTest.cpp
        src/storage/PortableSecretStore.h
        src/storage/PortableSecretStore.cpp
        src/storage/SecretStore.h
    )
    target_include_directories(portable-secret-store-test PRIVATE src)
    target_link_libraries(portable-secret-store-test
        PRIVATE Qt6::Core Qt6::Test OpenSSL::Crypto)
    add_test(NAME portable-secret-store COMMAND portable-secret-store-test)

    qt_add_executable(rust-session-lifecycle-test
        tests/RustSessionLifecycleTest.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        src/matrix/RustSessionPolicy.cpp
    )
    target_include_directories(rust-session-lifecycle-test PRIVATE src)
    target_link_libraries(rust-session-lifecycle-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME rust-session-lifecycle COMMAND rust-session-lifecycle-test)

    qt_add_executable(custom-theme-test
        tests/CustomThemeTest.cpp
        src/app/CustomThemeStore.h
        src/app/CustomThemeStore.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(custom-theme-test PRIVATE src)
    target_compile_definitions(custom-theme-test PRIVATE
        APPTHEME_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/AppTheme.qml")
    target_link_libraries(custom-theme-test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME custom-theme COMMAND custom-theme-test)

    # The Spaces rail's own arrangement (drag order + folders). Pure over a
    # model snapshot, so it needs no rail, no ListView and no homeserver.
    qt_add_executable(rail-layout-test
        tests/RailLayoutTest.cpp
        src/spaces/RailLayoutStore.h
        src/spaces/RailLayoutStore.cpp
        # The drag lives in the MODEL, so the gesture — preview reorder,
        # grouping, and what a release writes — is testable without a rail,
        # a ListView or a pointer.
        src/spaces/RailEntryModel.h
        src/spaces/RailEntryModel.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(rail-layout-test PRIVATE src)
    target_link_libraries(rail-layout-test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME rail-layout COMMAND rail-layout-test)

    qt_add_executable(localization-test
        tests/LocalizationTest.cpp
        src/i18n/LocalizationManager.h
        src/i18n/LocalizationManager.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(localization-test PRIVATE src)
    # The catalogs are read from the SOURCE tree (the .ts files, for the
    # placeholder contract) and from the BUILD tree (the compiled .qm, to
    # prove one actually loads). Neither is a resource here: this target
    # deliberately does not build the QML module.
    target_compile_definitions(localization-test PRIVATE
        LIGHTNING_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(LIGHTNING_TRANSLATIONS_ENABLED)
        target_compile_definitions(localization-test PRIVATE
            LIGHTNING_HAS_TRANSLATIONS=1
            LIGHTNING_QM_DIR="${LIGHTNING_QM_DIR}")
        add_dependencies(localization-test lightning-translations)
        # The catalogs are embedded here too, at the SAME :/i18n prefix the
        # application uses, so the test exercises the real resource lookup
        # rather than a filesystem path that only exists in a build tree.
        qt_add_resources(localization-test "lightning_i18n_localization_test"
            PREFIX "/i18n"
            BASE "${LIGHTNING_QM_DIR}"
            FILES ${LIGHTNING_QM_FILES})
    else()
        # Without Linguist tools there is no .qm to load; the test still runs
        # its policy and .ts assertions and skips the loading one.
        target_compile_definitions(localization-test PRIVATE
            LIGHTNING_QM_DIR="${CMAKE_CURRENT_BINARY_DIR}/i18n")
    endif()
    target_link_libraries(localization-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME localization COMMAND localization-test)

    qt_add_executable(settings-session-test
        tests/SettingsSessionTest.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/media/MediaVisibilityStore.h
        src/media/MediaVisibilityStore.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(settings-session-test PRIVATE src)
    target_link_libraries(settings-session-test PRIVATE Qt6::Core Qt6::Test)
    # The preview-default case reads docs/privacy.md: the code default and the
    # published promise must move together, and once did not.
    target_compile_definitions(settings-session-test PRIVATE
        REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME settings-session COMMAND settings-session-test)

    # v0.9 (phase 11) "Send later": server (MSC4140) vs local queue policy,
    # encrypted-room plaintext never persisted, "sending" persisted BEFORE
    # dispatch, nothing leaves while disconnected, a mid-send crash is
    # reported and never re-fired, sign-out drops the memory queue.
    qt_add_executable(scheduled-send-test
        tests/ScheduledSendTest.cpp
        src/models/ScheduledSendController.h
        src/models/ScheduledSendController.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/media/MediaVisibilityStore.h
        src/media/MediaVisibilityStore.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(scheduled-send-test PRIVATE src)
    target_link_libraries(scheduled-send-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME scheduled-send COMMAND scheduled-send-test)

    # v0.9 (phase 2) Activity Center model: classification (mentions,
    # replies to me, replies in my threads, reactions to my messages,
    # invites, whole-word keywords), own/echo/state exclusion, the model's
    # own seen state with only the marker persisted, bounded newest-first,
    # account switch drops everything, server seed honours the read flag.
    qt_add_executable(activity-model-test
        tests/ActivityModelTest.cpp
        src/models/ActivityModel.h
        src/models/ActivityModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(activity-model-test PRIVATE src)
    target_link_libraries(activity-model-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME activity-model COMMAND activity-model-test)

    qt_add_executable(account-registry-test
        tests/AccountRegistryTest.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/auth/AccountManager.h
        src/auth/AccountManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
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
        src/storage/PortableMode.cpp
    )
    qt_add_resources(emoji-catalog-test "emoji-test-data"
        PREFIX "/qt/qml/MatrixClient"
        FILES data/emoji-catalog.tsv)
    target_include_directories(emoji-catalog-test PRIVATE src)
    target_link_libraries(emoji-catalog-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME emoji-catalog COMMAND emoji-catalog-test)

    # Custom application icon: byte-sniffed raster whitelist, SVG refusal,
    # bounded decode, circular 512px normalization, persisted enable flag,
    # and the device-global app-data path.
    qt_add_executable(custom-app-icon-test
        tests/CustomAppIconTest.cpp
        src/app/CustomAppIcon.h
        src/app/CustomAppIcon.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(custom-app-icon-test PRIVATE src)
    target_link_libraries(custom-app-icon-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)

    # User-selectable and user-importable fonts. The link list is part of the
    # assertion: FontManager sees Qt, SettingsManager and the app-data paths
    # and NOTHING ELSE — no MatrixClient, no network, nothing that could hand
    # it a path chosen by anybody but the person at the keyboard.
    qt_add_executable(font-manager-test
        tests/FontManagerTest.cpp
        src/app/FontManager.h
        src/app/FontManager.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(font-manager-test PRIVATE src)
    target_compile_definitions(font-manager-test PRIVATE
        SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(font-manager-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME font-manager COMMAND font-manager-test)
    add_test(NAME custom-app-icon COMMAND custom-app-icon-test)

    # The VAAPI texture-export warning gate: pass-through window, storm
    # suppression, periodic summaries, unrelated messages untouched.
    qt_add_executable(vaapi-log-gate-test tests/VaapiLogGateTest.cpp)
    target_include_directories(vaapi-log-gate-test PRIVATE src)
    target_link_libraries(vaapi-log-gate-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME vaapi-log-gate COMMAND vaapi-log-gate-test)

    # Storm Band tile painter: deterministic procedural pixel-art layers,
    # palette derivation from theme inputs, defensive id parsing.
    qt_add_executable(storm-band-painter-test
        tests/StormBandPainterTest.cpp
        src/app/StormBandPainter.h
        src/app/StormBandPainter.cpp
    )
    target_include_directories(storm-band-painter-test PRIVATE src)
    target_link_libraries(storm-band-painter-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
    add_test(NAME storm-band-painter COMMAND storm-band-painter-test)
    set_tests_properties(storm-band-painter PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Storm Band QML contract: loads the real component offscreen, pins the
    # reduced-motion stop, pointer transparency, fixed height, and the
    # no-WebView constraint.
    qt_add_executable(storm-band-qml-test
        tests/StormBandQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(storm-band-qml-test)
    target_compile_definitions(storm-band-qml-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(storm-band-qml-test PRIVATE Qt6::Test)
    add_test(NAME storm-band-qml COMMAND storm-band-qml-test)
    set_tests_properties(storm-band-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
        src/storage/PortableMode.cpp
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
        src/gif/GifStarredModel.h
        src/gif/GifStarredModel.cpp
        src/gif/GifSavedModel.h
        src/gif/GifSavedModel.cpp
        src/gif/GifStarredStore.h
        src/gif/GifStarredStore.cpp
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
        src/storage/PortableMode.cpp
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
    lightning_configure_qml_test(thread-panel-pagination-anchor-qml-test)
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
        src/storage/PortableMode.cpp
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
        src/gif/GifStarredModel.h
        src/gif/GifStarredModel.cpp
        src/gif/GifSavedModel.h
        src/gif/GifSavedModel.cpp
    )
    target_include_directories(gif-collections-test PRIVATE src)
    target_link_libraries(gif-collections-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME gif-collections COMMAND gif-collections-test)

    # "Star a chat GIF" local store: content-hash dedup, item/byte-size caps
    # that REFUSE rather than silently evict, account-scope isolation (two
    # openFor() directories never see each other's rows), unstar deleting
    # the file, and a stale/tampered index entry never surfacing an
    # unplayable tile.
    qt_add_executable(gif-starred-store-test
        tests/GifStarredStoreTest.cpp
        src/gif/GifResponseParser.h
        src/gif/GifResponseParser.cpp
        src/gif/GifResultModel.h
        src/gif/GifResultModel.cpp
        src/gif/GifStoredModel.h
        src/gif/GifStoredModel.cpp
        src/gif/GifStarredModel.h
        src/gif/GifStarredModel.cpp
        src/gif/GifStarredStore.h
        src/gif/GifStarredStore.cpp
    )
    target_include_directories(gif-starred-store-test PRIVATE src)
    target_link_libraries(gif-starred-store-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME gif-starred-store COMMAND gif-starred-store-test)

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
        src/storage/PortableMode.cpp
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

    # v0.6.6 UX rework: the Discord-style hover star that replaced the GIF
    # starring context-menu rows — gating parity, HoverHandler-only hover
    # detection (never a MouseArea), one activation function reached by tap/
    # keyboard/accessibility action, mediaKey-keyed resolution, and live
    # refresh from the store's own signals.
    qt_add_executable(gif-hover-star-contract-test
        tests/GifHoverStarContractTest.cpp)
    target_compile_definitions(gif-hover-star-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(gif-hover-star-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME gif-hover-star-contract COMMAND gif-hover-star-contract-test)

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
    lightning_configure_qml_test(timeline-pane-qml-test)
    target_link_libraries(timeline-pane-qml-test PRIVATE Qt6::Test)
    add_test(NAME timeline-pane-qml COMMAND timeline-pane-qml-test)
    set_tests_properties(timeline-pane-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-26: the rail's drop-to-make-a-folder gesture, driven by a REAL
    # POINTER. RailLayoutTest covers the model and passed happily through two
    # rounds in which the gesture was structurally unreachable, because it
    # hands the model the row production could never produce. This one presses,
    # moves and releases on the real SpacesRail item tree and asserts on the
    # STORE — the only kind of test that could have caught either defect.
    qt_add_executable(rail-drag-qml-test
        tests/RailDragQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(rail-drag-qml-test)
    target_link_libraries(rail-drag-qml-test PRIVATE Qt6::Test)
    add_test(NAME rail-drag-qml COMMAND rail-drag-qml-test)
    set_tests_properties(rail-drag-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7 timeline-stability suite: the initial-hydration presentation gate,
    # bottom-pinned stability under asynchronous row growth, scrolled-up
    # anchor preservation, and row-scoped Set-diff model updates — the live
    # room-open jump regressions, staged deterministically on the mock.
    qt_add_executable(timeline-hydration-qml-test
        tests/TimelineHydrationQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(timeline-hydration-qml-test)
    target_link_libraries(timeline-hydration-qml-test PRIVATE Qt6::Test)
    add_test(NAME timeline-hydration-qml COMMAND timeline-hydration-qml-test)
    set_tests_properties(timeline-hydration-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Follow-up to timeline-state-flood-perf (pure C++): measures the real
    # QML pipeline (real AppController/MockMatrixClient, the real compiled
    # TimelinePane.qml/MessageDelegate.qml) for a near-top pagination page of
    # contiguous state-change rows (e.g. repeated display-name changes with
    # no messages between them) versus the same page size of ordinary
    # messages, and for several consecutive pages growing the same state
    # group. See the test file header for exactly what is measured.
    qt_add_executable(timeline-state-flood-qml-perf-test
        tests/TimelineStateFloodQmlPerfTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(timeline-state-flood-qml-perf-test)
    target_link_libraries(timeline-state-flood-qml-perf-test PRIVATE Qt6::Test)
    add_test(NAME timeline-state-flood-qml-perf
             COMMAND timeline-state-flood-qml-perf-test)
    set_tests_properties(timeline-state-flood-qml-perf PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7: type-specific media placeholders — reserved aspect geometry for
    # image/video/sticker, compact audio/voice rows, the decrypting text
    # skeleton, and the shared Skeleton primitive's animation eligibility.
    qt_add_executable(media-placeholder-qml-test
        tests/MediaPlaceholderQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(media-placeholder-qml-test)
    target_link_libraries(media-placeholder-qml-test PRIVATE Qt6::Test)
    add_test(NAME media-placeholder-qml COMMAND media-placeholder-qml-test)
    set_tests_properties(media-placeholder-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-26: the link-preview CONSENT gate. It used to stack a host
    # row, a wrapped two-line privacy sentence and a full-width button —
    # about four message lines of timeline for one unloaded link. This
    # suite pins the compact replacement: one band, strictly smaller than
    # the state the consent click leads to, with the privacy fact still
    # stated before consent and the button still the only thing that
    # consents.
    qt_add_executable(link-preview-qml-test
        tests/LinkPreviewQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(link-preview-qml-test)
    target_link_libraries(link-preview-qml-test PRIVATE Qt6::Test)
    add_test(NAME link-preview-qml COMMAND link-preview-qml-test)
    set_tests_properties(link-preview-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7.2: the audio card's cover-art box keeps the artwork's aspect at
    # every size. Bounding only the height left a tall cover in a full-width
    # box at the cap, letterboxed with dead surface down both sides.
    qt_add_executable(audio-cover-art-aspect-test
        tests/AudioCoverArtAspectTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(audio-cover-art-aspect-test)
    target_link_libraries(audio-cover-art-aspect-test PRIVATE Qt6::Test)
    add_test(NAME audio-cover-art-aspect COMMAND audio-cover-art-aspect-test)
    set_tests_properties(audio-cover-art-aspect PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7: avatar pipeline — explicit loading/ready/missing/failed states
    # through the real MediaBridge + MediaImageProvider, pixel-proven
    # transparency (no fallback fill beneath a decoded avatar), stable
    # identity-keyed fallback colours, and clean URL-change resets.
    qt_add_executable(avatar-pipeline-qml-test
        tests/AvatarPipelineQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(avatar-pipeline-qml-test)
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
    lightning_configure_qml_test(gif-picker-selection-qml-test)
    target_link_libraries(gif-picker-selection-qml-test PRIVATE Qt6::Test)
    add_test(NAME gif-picker-selection-qml
             COMMAND gif-picker-selection-qml-test)
    set_tests_properties(gif-picker-selection-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-28 tester round: the Saved tab's PROVIDER half rendered for the
    # first time (every earlier saved-tab test cleared favorites and covered
    # only the local byte store), the empty-state copy asserted as visible,
    # a real press-drag on the resize grip over a real Flickable, and the
    # nav row's right-aligned list tabs asserted as geometry at two widths.
    qt_add_executable(gif-saved-tab-qml-test
        tests/GifSavedTabQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(gif-saved-tab-qml-test)
    target_link_libraries(gif-saved-tab-qml-test PRIVATE Qt6::Test)
    add_test(NAME gif-saved-tab-qml COMMAND gif-saved-tab-qml-test)
    set_tests_properties(gif-saved-tab-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7: authenticated startup lifecycle — the Boot/RestoringSession
    # state, the never-instantiate-login guarantee for valid sessions, the
    # genuine no-account and failed-restore login states.
    qt_add_executable(startup-session-test
        tests/StartupSessionTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(startup-session-test)
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
    lightning_configure_qml_test(account-switch-test)
    target_link_libraries(account-switch-test PRIVATE Qt6::Test)
    add_test(NAME account-switch COMMAND account-switch-test)
    set_tests_properties(account-switch PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-20 (C7): setting the signed-in account's own display name on a
    # real AppController (mock backend). The caller-owned op id and the
    # lifecycle guard are the point: a completion from a superseded generation
    # or a foreign op must never update the cached identity. Also pins the
    # code-point ceiling (never a byte slice — that splits a surrogate pair),
    # explicit clear vs an empty-value refusal, the unicode round trip, and
    # the registry re-fetch that a confirmed write must trigger, because sync
    # does not feed that cache.
    qt_add_executable(own-profile-test
        tests/OwnProfileTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(own-profile-test)
    target_link_libraries(own-profile-test PRIVATE Qt6::Test)
    add_test(NAME own-profile COMMAND own-profile-test)
    set_tests_properties(own-profile PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.6.6 review (M2): AppController::isChatGifStarred/unstarChatGif's
    # two-tier design (session-only fast path + durable content-hash
    # fallback through MediaBridge) had no executable coverage — only
    # MediaBridgeTest.cpp/GifStarredStoreTest.cpp exercising each half in
    # isolation. Drives a real AppController (mock backend) with its
    # MediaBridge repointed at a small controllable FakeClient.
    qt_add_executable(app-controller-chat-gif-starred-test
        tests/AppControllerChatGifStarredTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(app-controller-chat-gif-starred-test)
    target_link_libraries(app-controller-chat-gif-starred-test PRIVATE Qt6::Test)
    add_test(NAME app-controller-chat-gif-starred
             COMMAND app-controller-chat-gif-starred-test)
    set_tests_properties(app-controller-chat-gif-starred PROPERTIES
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

    # 2026-08-20 (C3): the member-profile sentence matrix. The Rust bridge no
    # longer builds an English sentence for m.room.member profile changes — it
    # sends typed old/new values — so the wording is built here, translatably,
    # from the actor's RESOLVED display name. Names are untrusted plain text;
    # these cases pin set/changed/cleared/avatar/both, the honest fallback
    # when a value is missing (never empty quotes), and that markup in a name
    # is never interpreted.
    qt_add_executable(profile-activity-test
        tests/ProfileActivityTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(profile-activity-test PRIVATE src)
    target_link_libraries(profile-activity-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME profile-activity COMMAND profile-activity-test)

    # The hover toolbar is one per row, anchored in place; the row simply
    # does not clip, so a one-line message no longer truncates it. (An
    # earlier overlay-mapped shared instance was reverted — rows are rotated
    # 180 degrees.) Exactly one row may show a bar at a time.
    qt_add_executable(message-action-bar-fit-test
        tests/MessageActionBarFitTest.cpp)
    target_compile_definitions(message-action-bar-fit-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(message-action-bar-fit-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME message-action-bar-fit COMMAND message-action-bar-fit-test)

    # Every scrollable pane must route the wheel through SmoothWheelArea, so
    # the whole app matches the timeline's feel instead of Qt's defaults.
    qt_add_executable(scroll-consistency-contract-test
        tests/ScrollConsistencyContractTest.cpp)
    target_compile_definitions(scroll-consistency-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(scroll-consistency-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME scroll-consistency-contract
        COMMAND scroll-consistency-contract-test)

    # RUNTIME counterpart to scroll-consistency-contract: that test only
    # scans QML source for "SmoothWheelArea" and cannot see a null
    # scrollTarget, which makes a pane completely unscrollable (onWheel
    # accepts the event and returns).
    qt_add_executable(smooth-wheel-area-runtime-test
        tests/SmoothWheelAreaRuntimeTest.cpp)
    lightning_configure_qml_test(smooth-wheel-area-runtime-test)
    target_link_libraries(smooth-wheel-area-runtime-test PRIVATE Qt6::Test)
    add_test(NAME smooth-wheel-area-runtime
             COMMAND smooth-wheel-area-runtime-test)
    set_tests_properties(smooth-wheel-area-runtime PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-20 (C1): qml/CodeBlock.qml's own behaviour. A fenced block used
    # to render inside the body's one RichText item, where Qt does not wrap
    # <pre> and the delegate root is deliberately clip:false — so a terminal
    # transcript painted straight out of the timeline. These cases pin the
    # properties that make that impossible: a long line becomes horizontal
    # SCROLL RANGE and never geometry, the gutter is never part of a copy,
    # and a vertical wheel notch still belongs to the timeline.
    # The Channels layout's rows, measured in real geometry. Every one of
    # them was 0px tall — see the file header — and six source-scanning
    # contract suites passed the whole time.
    qt_add_executable(channel-row-geometry-qml-test
        tests/ChannelRowGeometryQmlTest.cpp)
    lightning_configure_qml_test(channel-row-geometry-qml-test)
    target_link_libraries(channel-row-geometry-qml-test PRIVATE Qt6::Test)
    add_test(NAME channel-row-geometry-qml
        COMMAND channel-row-geometry-qml-test)
    set_tests_properties(channel-row-geometry-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(code-block-qml-test tests/CodeBlockQmlTest.cpp)
    lightning_configure_qml_test(code-block-qml-test)
    target_link_libraries(code-block-qml-test PRIVATE Qt6::Test)
    add_test(NAME code-block-qml COMMAND code-block-qml-test)
    set_tests_properties(code-block-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Measurement (not a fix): quantifies the cost of the state-activity
    # grouping mechanism (TimelineModel::stateGroupLeaderRow /
    # stateGroupEntriesFrom / emitPresentationGroupingChanged) under a long
    # run of contiguous state-change rows (e.g. repeated display-name
    # changes with no messages between them), and contrasts it with the
    # same row count of ordinary messages. See the test file header for the
    # exact mechanism and the closed-form costs it asserts against.
    qt_add_executable(timeline-state-flood-perf-test
        tests/TimelineStateFloodPerfTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(timeline-state-flood-perf-test PRIVATE src)
    target_link_libraries(timeline-state-flood-perf-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME timeline-state-flood-perf COMMAND timeline-state-flood-perf-test)

    # v0.5.7: CacheStore must refuse encrypted-room rows on every write
    # path — decrypted plaintext stays memory-only.
    qt_add_executable(cache-store-security-test
        tests/CacheStoreSecurityTest.cpp
        src/storage/CacheStore.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(cache-store-security-test PRIVATE src)
    target_link_libraries(cache-store-security-test PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME cache-store-security COMMAND cache-store-security-test)

    # v0.5.9: semantic theme tokens + WCAG contrast for critical pairs +
    # no stray hex colours in core view QML.
    qt_add_executable(theme-tokens-test
        tests/ThemeTokensTest.cpp
        src/theme/IdentityColors.h
        src/theme/IdentityColors.cpp
    )
    target_include_directories(theme-tokens-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(theme-tokens-test PRIVATE
        APPTHEME_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/AppTheme.qml"
        SETTINGS_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/SettingsScreen.qml"
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
    )
    target_link_libraries(theme-tokens-test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME theme-tokens COMMAND theme-tokens-test)

    # v0.5.9: attachment pipeline — validation, MIME-from-content, queue
    # state machine, retry, stale-completion rejection, lifecycle clears.
    qt_add_executable(attachment-queue-test
        tests/AttachmentQueueTest.cpp
        src/models/MessageComposer.h
        src/models/MessageComposer.cpp
        # v0.7.x: the composer's draft hooks pull in the store.
        src/app/DraftStore.h
        src/app/DraftStore.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        src/models/MentionTokenizer.h
        src/models/MentionTokenizer.cpp
        src/models/MarkdownFormat.h
        src/models/MarkdownFormat.cpp
        src/models/SlashCommands.h
        src/models/SlashCommands.cpp
        src/models/AttachmentQueueModel.h
        src/models/AttachmentQueueModel.cpp
        src/media/StagedImageStore.h
        src/media/StagedImageStore.cpp
        # v0.7: the queue owns send-side video poster extraction. The suite
        # installs the poster hook, so no QMediaPlayer is ever constructed.
        src/media/VideoPosterExtractor.h
        src/media/VideoPosterExtractor.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(attachment-queue-test PRIVATE src)
    target_link_libraries(attachment-queue-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME attachment-queue COMMAND attachment-queue-test)

    # v0.7.2: poster extraction runs on its own thread. The first QVideoSink
    # in a process costs ~931 ms (lazy Qt Multimedia backend init), and it
    # used to be built inline on the GUI thread the moment a video scrolled
    # into view. Unlike the suite above, this one DOES construct a real
    # QMediaPlayer — that is the point.
    qt_add_executable(video-poster-threading-test
        tests/VideoPosterThreadingTest.cpp
        src/media/VideoPosterExtractor.h
        src/media/VideoPosterExtractor.cpp
    )
    target_include_directories(video-poster-threading-test PRIVATE src)
    target_link_libraries(video-poster-threading-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME video-poster-threading
             COMMAND video-poster-threading-test)

    # 2026-08-20 (C8): playable materialization runs on its own thread.
    # MediaBridge::writePlayableFile() wrote the whole decrypted A/V payload
    # synchronously on the GUI thread — its own comment said so, and the
    # gui-stall tracer's "playable-write" category existed to attribute it.
    # This suite measures a synchronous baseline with the same payload on the
    # same filesystem, then proves the call returns before the bytes are on
    # disk and the caller's thread keeps beating through the write.
    qt_add_executable(playable-write-threading-test
        tests/PlayableWriteThreadingTest.cpp
        src/media/PlayableFileWriter.h
        src/media/PlayableFileWriter.cpp
    )
    target_include_directories(playable-write-threading-test PRIVATE src)
    target_link_libraries(playable-write-threading-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME playable-write-threading
             COMMAND playable-write-threading-test)
    set_tests_properties(video-poster-threading PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # v0.7.3: the initials disc a notification carries when an identity has
    # no avatar. Pins the palette, hash and initials rule against qml's, so
    # the disc in a notification matches the one in the room list.
    qt_add_executable(notification-avatar-test
        tests/NotificationAvatarTest.cpp
        src/notifications/FallbackAvatar.h
        src/notifications/FallbackAvatar.cpp
        src/theme/IdentityColors.h
        src/theme/IdentityColors.cpp
    )
    target_include_directories(notification-avatar-test PRIVATE src)
    target_link_libraries(notification-avatar-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME notification-avatar COMMAND notification-avatar-test)
    set_tests_properties(notification-avatar PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

    # 2026-08-14: kick/ban moderation through RoomInfoController — snapshot
    # permission ingestion, SDK-flag gating, one-in-flight discipline,
    # sanitized result mapping, stale-result rejection on room switch.
    qt_add_executable(room-info-moderation-test
        tests/RoomInfoModerationTest.cpp
        src/app/RoomInfoController.h
        src/app/RoomInfoController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-info-moderation-test PRIVATE src)
    target_link_libraries(room-info-moderation-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-info-moderation COMMAND room-info-moderation-test)

    # v0.7.x room administration: member power levels, join rule and
    # canonical alias through RoomInfoController — Matrix rule enforcement
    # before dispatch, custom numeric levels preserved rather than rounded
    # into presets, no optimistic local write, stale-answer rejection.
    qt_add_executable(room-power-levels-test
        tests/RoomPowerLevelsTest.cpp
        src/app/RoomInfoController.h
        src/app/RoomInfoController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-power-levels-test PRIVATE src)
    target_link_libraries(room-power-levels-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-power-levels COMMAND room-power-levels-test)

    # v0.7.x pinned messages: PinnedMessagesController policy — snapshot
    # ingestion, the complete-id-list contract behind isPinned(), offering
    # exactly the action that applies, no optimistic apply, remote-change
    # re-read, room-switch and sign-out clearing.
    qt_add_executable(pinned-messages-test
        tests/PinnedMessagesTest.cpp
        src/app/PinnedMessagesController.h
        src/app/PinnedMessagesController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(pinned-messages-test PRIVATE src)
    target_link_libraries(pinned-messages-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME pinned-messages COMMAND pinned-messages-test)

    # MSC2545 stickers and custom emoji: StickerPackManager policy — snapshot
    # ingestion and usage narrowing, the selection rules, canSave (plain mxc
    # only, never gated on data nobody fetched), the send destination and the
    # no-room-fallback thread rule, bounded emoticon lookup, and generation
    # isolation across sign-out.
    qt_add_executable(sticker-packs-test
        tests/StickerPacksTest.cpp
        src/stickers/StickerPack.h
        src/stickers/StickerPack.cpp
        src/stickers/StickerPackModel.h
        src/stickers/StickerPackModel.cpp
        src/stickers/StickerImageModel.h
        src/stickers/StickerImageModel.cpp
        src/stickers/StickerPackManager.h
        src/stickers/StickerPackManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(sticker-packs-test PRIVATE src)
    target_link_libraries(sticker-packs-test PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME sticker-packs COMMAND sticker-packs-test)

    # v0.7.x room upgrades: the banner-and-link contract. An unknown
    # successor stays Unknown (never "inaccessible"), a joined one navigates
    # without joining, an invited one joins BEFORE navigating, a failed join
    # leaves the user in the old room with the reason shown, the successor
    # must point BACK for the chain to count, and the room list DEMOTES a
    # superseded room rather than filtering it. Above all: observing a
    # tombstone never moves the user or joins anything.
    qt_add_executable(room-upgrade-test
        tests/RoomUpgradeTest.cpp
        src/app/RoomUpgradeController.h
        src/app/RoomUpgradeController.cpp
        src/app/RoomDiscoveryController.h
        src/app/RoomDiscoveryController.cpp
        src/models/RoomDirectorySearchModel.h
        src/models/RoomDirectorySearchModel.cpp
        src/models/RoomListModel.h
        src/models/RoomListModel.cpp
        src/matrix/BridgeNetwork.h
        src/matrix/BridgeNetwork.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-upgrade-test PRIVATE src)
    target_link_libraries(room-upgrade-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-upgrade COMMAND room-upgrade-test)

    # v0.7.x Space settings: a Space IS a Matrix room, so its name, topic and
    # avatar go through the SAME permission-gated room-edit backend, each
    # gated on that state event's own required level. The avatar controls
    # were missing entirely — a Space could be renamed but never given a
    # picture from inside Lightning.
    # 2026-08-26: this was a pure source scan. It now also instantiates
    # RoomInfoController against a fake client to prove what the Permissions
    # matrix will and will not DISPATCH, so it needs the controller and the
    # client base the way room-info-moderation-test does — a scan alone cannot
    # tell an offered write from a refused one.
    qt_add_executable(space-settings-contract-test
        tests/SpaceSettingsContractTest.cpp
        src/app/RoomInfoController.h
        src/app/RoomInfoController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(space-settings-contract-test PRIVATE src)
    target_compile_definitions(space-settings-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(space-settings-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME space-settings-contract
        COMMAND space-settings-contract-test)

    # v0.7.x read markers: "Mark as read" resolved its target from the LOADED
    # timeline, which on the Rust backend only ever holds the OPEN room — so
    # marking any other room read sent nothing and silently did nothing. Pins
    # the capable-backend route, the legacy timeline-walk fallback, local-echo
    # skipping, and inert handling of unknown targets.
    qt_add_executable(mark-room-read-test
        tests/MarkRoomReadTest.cpp
        src/models/RoomListModel.h
        src/models/RoomListModel.cpp
        src/matrix/BridgeNetwork.h
        src/matrix/BridgeNetwork.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(mark-room-read-test PRIVATE src)
    target_link_libraries(mark-room-read-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME mark-room-read COMMAND mark-room-read-test)

    # v0.7.x message forwarding: ForwardController policy — refusal of
    # redacted/local-echo/undecryptable sources, plain-body text forwarding
    # with NO relation (a forwarded thread reply must land as an ordinary
    # message), media RE-FETCHED and re-uploaded rather than mxc-copied, a
    # failure that leaves the dialog open, and the stale-fetch race across a
    # second begin()/cancel().
    qt_add_executable(forward-message-test
        tests/ForwardMessageTest.cpp
        src/app/ForwardController.h
        src/app/ForwardController.cpp
        src/media/MediaBridge.h
        src/media/MediaBridge.cpp
        src/storage/PortableMode.cpp
        src/media/PlayableFileWriter.h
        src/media/PlayableFileWriter.cpp
        src/app/GuiStallTracer.cpp
        src/media/VideoPosterExtractor.h
        src/media/VideoPosterExtractor.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(forward-message-test PRIVATE src)
    target_link_libraries(forward-message-test
        PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME forward-message COMMAND forward-message-test)

    # v0.7.x: the thread-participant fetch bound — concurrency cap, FIFO
    # queue, slot release on success AND on a failed (empty) answer,
    # deduplication against cached/in-flight/queued roots, and room-switch
    # invalidation of queued-but-not-started work.
    qt_add_executable(thread-facepile-bound-test
        tests/ThreadFacepileBoundTest.cpp
        src/threads/ThreadManager.h
        src/threads/ThreadManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(thread-facepile-bound-test PRIVATE src)
    target_link_libraries(thread-facepile-bound-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME thread-facepile-bound COMMAND thread-facepile-bound-test)

    # v0.7.x Discover / Join: directory search model (debounce, paging,
    # stale-op rejection) and the discovery controller (join/knock error
    # categories, wait-for-room, space children cache, sign-out clears).
    qt_add_executable(room-discovery-test
        tests/RoomDiscoveryTest.cpp
        src/app/RoomDiscoveryController.h
        src/app/RoomDiscoveryController.cpp
        src/models/RoomDirectorySearchModel.h
        src/models/RoomDirectorySearchModel.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        # MockMatrixClient's registry-backed restore needs these:
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(room-discovery-test PRIVATE src)
    target_link_libraries(room-discovery-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME room-discovery COMMAND room-discovery-test)

    # v0.7.x server-side message search: debounce, roomName resolution,
    # paging, stale superseding, scope changes, sign-out clears.
    qt_add_executable(message-search-test
        tests/MessageSearchControllerTest.cpp
        src/models/MessageSearchController.h
        src/models/MessageSearchController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(message-search-test PRIVATE src)
    target_link_libraries(message-search-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME message-search COMMAND message-search-test)

    # v0.7.x UIA + device sign-out: challenge round-trip, wrong-password
    # retry, cancel, current-device guard, OAuth console URL. Credentials
    # never persist — the suite asserts flow state only.
    qt_add_executable(uia-session-test
        tests/UiaSessionTest.cpp
        src/app/UiaController.h
        src/app/UiaController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(uia-session-test PRIVATE src)
    target_link_libraries(uia-session-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME uia-session COMMAND uia-session-test)

    # v0.7.x personal moderation: ignore/unignore round trips, remote list
    # pushes, report prompt flow, and the notification ignore guard.
    qt_add_executable(moderation-test
        tests/ModerationTest.cpp
        src/app/ModerationController.h
        src/app/ModerationController.cpp
        src/notifications/NotificationManager.h
        src/notifications/NotificationManager.cpp
        src/notifications/FallbackAvatar.h
        src/notifications/FallbackAvatar.cpp
        src/theme/IdentityColors.h
        src/theme/IdentityColors.cpp
        src/matrix/EventPreview.h
        src/matrix/EventPreview.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(moderation-test PRIVATE src)
    target_link_libraries(moderation-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    if(TARGET Qt6::DBus)
        target_link_libraries(moderation-test PRIVATE Qt6::DBus)
        target_compile_definitions(moderation-test PRIVATE HAVE_QT_DBUS=1)
    endif()
    add_test(NAME moderation COMMAND moderation-test)

    # v0.7.x drafts: scoping, encrypted-room memory-only policy, debounce
    # staleness, send/clear retirement, account isolation.
    qt_add_executable(draft-store-test
        tests/DraftStoreTest.cpp
        src/app/DraftStore.h
        src/app/DraftStore.cpp
        src/models/MessageComposer.h
        src/models/MessageComposer.cpp
        src/models/MentionTokenizer.h
        src/models/MentionTokenizer.cpp
        src/models/MarkdownFormat.h
        src/models/MarkdownFormat.cpp
        src/models/SlashCommands.h
        src/models/SlashCommands.cpp
        src/models/AttachmentQueueModel.h
        src/models/AttachmentQueueModel.cpp
        src/media/StagedImageStore.h
        src/media/StagedImageStore.cpp
        src/media/VideoPosterExtractor.h
        src/media/VideoPosterExtractor.cpp
        src/threads/ThreadController.h
        src/threads/ThreadController.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        # Header only (settings-session-test precedent): AUTOMOC supplies
        # the SecretStore metaobject; the .cpp would drag in the real
        # backends this suite must not depend on.
        src/storage/SecretStore.h
    )
    target_include_directories(draft-store-test PRIVATE src)
    target_link_libraries(draft-store-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Qml Qt6::Test)
    add_test(NAME draft-store COMMAND draft-store-test)

    # 2026-08-18: opt-in GUI stall heartbeat (LIGHTNING_GUI_STALL_TRACE) —
    # detection, duration measurement, Scope attribution, healthy-loop
    # silence.
    qt_add_executable(gui-stall-trace-test
        tests/GuiStallTraceTest.cpp
        src/app/GuiStallTracer.h
        src/app/GuiStallTracer.cpp
    )
    target_include_directories(gui-stall-trace-test PRIVATE src)
    target_link_libraries(gui-stall-trace-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME gui-stall-trace COMMAND gui-stall-trace-test)

    # 2026-08-18 tester report #2: contract pins for the round's QML
    # surfaces (copy image, reply thumbs, receipt popover honesty, space
    # invite + nested subspaces, rail indent, initial avatars).
    qt_add_executable(tester-report-2-contract-test
        tests/TesterReport2ContractTest.cpp
    )
    target_compile_definitions(tester-report-2-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(tester-report-2-contract-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME tester-report-2-contract
             COMMAND tester-report-2-contract-test)
    set_tests_properties(tester-report-2-contract PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-19 Element-parity round: the unified Space Home "Rooms and
    # spaces" list (Joined badges, capability-gated selection UI), the
    # rail's inline space expansion, and the reader popover's per-reader
    # timestamps — plus the non-exclusive-TapHandler guards on all of it.
    qt_add_executable(element-parity-contract-test
        tests/ElementParityContractTest.cpp
    )
    target_compile_definitions(element-parity-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(element-parity-contract-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME element-parity-contract
             COMMAND element-parity-contract-test)
    set_tests_properties(element-parity-contract PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-19: SpaceManager::setSpaceChildSuggested plumbing — pending
    # ops, foreign-op rejection, refused-send failure, logout isolation.
    qt_add_executable(space-child-suggest-test
        tests/SpaceChildSuggestTest.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(space-child-suggest-test PRIVATE src)
    target_link_libraries(space-child-suggest-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME space-child-suggest COMMAND space-child-suggest-test)

    # 2026-08-19 scroll round 2: the sliding window in ReverseListProxyModel.
    # Per-frame cost was MEASURED to scale with total instantiated items, so
    # the proxy has to be able to take rows back, not merely delay them. Every
    # window transition must be one insert/remove at one end, the mapping must
    # stay exactly invertible, and a live message must not slide a reader who
    # is deep in history.
    qt_add_executable(reverse-list-proxy-window-test
        tests/ReverseListProxyWindowTest.cpp
        src/models/ReverseListProxyModel.h
        src/models/ReverseListProxyModel.cpp
        # The proxy attributes its row construction for stall tracing, so a
        # standalone target needs the tracer too (same reason media-bridge
        # and forward already carry it).
        src/app/GuiStallTracer.h
        src/app/GuiStallTracer.cpp
    )
    target_include_directories(reverse-list-proxy-window-test PRIVATE src)
    target_link_libraries(reverse-list-proxy-window-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME reverse-list-proxy-window
             COMMAND reverse-list-proxy-window-test)

    # 2026-08-18 voice-call signaling pipes: MSC2746 state machine —
    # ringing, glare, busy auto-reject, cross-device settlement, invite
    # lifetime, LRU absorption, honest placeCall refusal, logout reset.
    qt_add_executable(call-controller-test
        tests/CallControllerTest.cpp
        src/calls/CallController.h
        src/calls/CallController.cpp
        src/matrix/RtcSession.h
        src/calls/RtcController.h
        src/calls/RtcController.cpp
        src/calls/SfuCallController.h
        src/calls/SfuCallController.cpp
        # SfuCallController.h answers `windowCaptureSupported` inline from
        # wincap::available(), so every target that includes the header needs
        # the symbol. Off Windows this file compiles to `fitInto` plus a
        # handful of stubs and pulls in no GStreamer at all.
        src/calls/WindowCaptureSrc.h
        src/calls/WindowCaptureSrc.cpp
        src/calls/CallParticipantModel.h
        src/calls/CallParticipantModel.cpp
        src/calls/CallShareModel.h
        src/calls/CallShareModel.cpp
        src/calls/CallStageState.h
        src/calls/CallStageState.cpp
        src/calls/ScreenCastPortal.h
        src/calls/ScreenCastPortal.cpp
        src/calls/CallMediaBackend.h
        src/calls/SdpStore.h
        src/matrix/CallSignal.h
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        # Header only (settings-session-test precedent): AUTOMOC supplies
        # the SecretStore metaobject; the .cpp would drag in the real
        # backends this suite must not depend on.
        src/storage/SecretStore.h
    )
    target_include_directories(call-controller-test PRIVATE src)
    target_link_libraries(call-controller-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    # SfuCallController reads a screen's NATIVE rectangle off QPlatformScreen
    # for the X11 screen-share fallback; see the note in
    # _lightning_configure_qml_module. Interface target: include paths only.
    if(UNIX AND NOT APPLE AND TARGET Qt6::GuiPrivate)
        target_link_libraries(call-controller-test PRIVATE Qt6::GuiPrivate)
        target_compile_definitions(call-controller-test PRIVATE
            LIGHTNING_HAVE_QPA_SCREEN=1)
    endif()
    # The Linux fallback's regression guard reads the controller's own source:
    # whether a capture rectangle is DERIVED from devicePixelRatio() is not
    # observable from the API, and deriving it again is the defect.
    target_compile_definitions(call-controller-test PRIVATE
        "SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    add_test(NAME call-controller COMMAND call-controller-test)

    # 2026-08-18 round 2: the ring policy wired to its real owners on a
    # full AppController — ignore/mute/backlog gates, ringForCalls, missed
    # classification, notification-decline routing.
    qt_add_executable(call-ring-policy-test
        tests/CallRingPolicyTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(call-ring-policy-test)
    target_link_libraries(call-ring-policy-test PRIVATE Qt6::Test)
    add_test(NAME call-ring-policy COMMAND call-ring-policy-test)
    set_tests_properties(call-ring-policy PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Screenshot harness for the in-call header bar. Not a test: it renders
    # the real component in a real window and saves a PNG, so a visual
    # complaint can be checked visually. Built only under BUILD_TESTING.
    qt_add_executable(call-bar-shot
        tests/CallBarShot.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(call-bar-shot)
    target_link_libraries(call-bar-shot PRIVATE Qt6::Test)

    # 2026-08-23 MatrixRTC phase 2: LiveKit-compatible frame encryption.
    # Asserts the WIRE FORMAT against livekit-client's own implementation —
    # header sizes, trailer layout, and the exact IV construction — plus the
    # safety properties (no key means no output, a tampered frame fails, a
    # wrong key index is dropped rather than guessed).
    qt_add_executable(call-frame-cryptor-test
        tests/CallFrameCryptorTest.cpp
        src/calls/CallFrameCryptor.cpp
    )
    target_include_directories(call-frame-cryptor-test PRIVATE src)
    target_link_libraries(call-frame-cryptor-test
        PRIVATE Qt6::Core Qt6::Test OpenSSL::Crypto)
    add_test(NAME call-frame-cryptor COMMAND call-frame-cryptor-test)

    # 2026-08-23: routing received video to a QML VideoOutput. Standalone
    # (two files, no app graph) because the class it tests deliberately knows
    # nothing about calls — it is a sink table with lifetime discipline, and
    # the defects it prevents are a dead tile still receiving frames and one
    # participant's video landing in another's tile.
    qt_add_executable(sfu-video-router-test
        tests/SfuVideoRouterTest.cpp
        src/calls/SfuVideoRouter.cpp
    )
    target_include_directories(sfu-video-router-test PRIVATE src)
    target_link_libraries(sfu-video-router-test
        PRIVATE Qt6::Core Qt6::Multimedia Qt6::Test)
    add_test(NAME sfu-video-router COMMAND sfu-video-router-test)

    # Navigation layouts: SpaceChannelModel's flat, GLOBAL list of Space
    # folders. The defects it exists to prevent are the ones the previous
    # active-Space design shipped with — a layout that silently became the
    # other layout at Home, and a subspace's rooms listed twice because the
    # only available child accessor was TRANSITIVE.
    qt_add_executable(space-channels-test
        tests/SpaceChannelsTest.cpp
        src/models/SpaceChannelModel.h
        src/models/SpaceChannelModel.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        # The rooms come from the CLIENT, not from RoomListModel: that model is
        # scoped to the active Space and filtered by the chips, which would
        # make this layout's global groups vanish the moment a Space was
        # selected.
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/spaces/RailLayoutStore.h
        src/spaces/RailLayoutStore.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(space-channels-test PRIVATE src)
    target_link_libraries(space-channels-test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME space-channels COMMAND space-channels-test)

    # 2026-08-23: the host/presenter SEPARATION. A source-scan suite on
    # purpose — what it defends is which file contains what, which no runtime
    # assertion can see. Chiefly: a presenter must never reach up into the
    # host by id (the 2026-08-19 dead-click shape), only one presenter is
    # instantiated, Channels never renders without a Space, and every new
    # theme token is DERIVED rather than added to eleven palettes.
    qt_add_executable(navigation-layout-contract-test
        tests/NavigationLayoutContractTest.cpp
    )
    target_compile_definitions(navigation-layout-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(navigation-layout-contract-test
        PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME navigation-layout-contract
             COMMAND navigation-layout-contract-test)

    # 2026-08-23 MatrixRTC observation: the session store's op-id/room
    # matching and generation isolation, poke coalescing (including a poke
    # that lands during an in-flight read), slot-closed suppression, one
    # face per person across devices, and the join-block reasons staying
    # distinguishable.
    qt_add_executable(rtc-session-test
        tests/RtcSessionTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(rtc-session-test)
    target_link_libraries(rtc-session-test PRIVATE Qt6::Test)
    add_test(NAME rtc-session COMMAND rtc-session-test)
    set_tests_properties(rtc-session PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-18 round 2: incoming-call corner card contracts + a real
    # offscreen instantiation against a live ringing CallController.
    qt_add_executable(call-ui-contract-test
        tests/CallUiContractTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(call-ui-contract-test)
    target_link_libraries(call-ui-contract-test PRIVATE Qt6::Test)
    target_compile_definitions(call-ui-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME call-ui-contract COMMAND call-ui-contract-test)
    set_tests_properties(call-ui-contract PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-18 round 3: the REAL WebRTC handshake — two GstCallMediaBackend
    # instances in one process (test-tone mode, no audio devices) complete a
    # full ICE + DTLS-SRTP negotiation over loopback host candidates.
    if(HAVE_LIGHTNING_WEBRTC)
        qt_add_executable(call-media-loopback-test
            tests/CallMediaLoopbackTest.cpp
            src/calls/CallMediaBackend.h
            src/calls/GstBootstrap.h
            src/calls/GstBootstrap.cpp
            src/calls/GstCallMediaBackend.h
            src/calls/GstCallMediaBackend.cpp
        )
        target_include_directories(call-media-loopback-test PRIVATE src)
        target_link_libraries(call-media-loopback-test PRIVATE
            Qt6::Core Qt6::Test PkgConfig::GSTWEBRTC)
        add_test(NAME call-media-loopback COMMAND call-media-loopback-test)

        # 2026-08-23: SfuMediaEngine had NO test while becoming the engine
        # every call runs through — the .well-known discovery fix turned it
        # from unreachable into the default path in one commit, and the next
        # real run died instantly on pressing call. Test-source mode only:
        # this exercises pipeline construction, the crypto pad probes and
        # teardown, not the network.
        qt_add_executable(sfu-media-engine-test
            tests/SfuMediaEngineTest.cpp
            src/calls/GstBootstrap.h
            src/calls/GstBootstrap.cpp
            src/calls/SfuMediaEngine.h
            src/calls/SfuMediaEngine.cpp
            src/calls/RtpVp8Payloader.h
            src/calls/RtpVp8Payloader.cpp
            src/calls/WindowCaptureSrc.h
            src/calls/WindowCaptureSrc.cpp
            src/calls/CallFrameCryptor.h
            src/calls/CallFrameCryptor.cpp
            src/calls/SfuVideoRouter.h
            src/calls/SfuVideoRouter.cpp
        )
        target_include_directories(sfu-media-engine-test PRIVATE src)
        target_compile_definitions(sfu-media-engine-test PRIVATE
            HAVE_LIGHTNING_WEBRTC
            "LIGHTNING_SFU_ENGINE_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/calls/SfuMediaEngine.cpp\""
            # The gst_init ORDERING check reads both backends and the shared
            # bootstrap: which function runs first cannot be observed from
            # inside a process that has already initialised GStreamer.
            "SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
        target_link_libraries(sfu-media-engine-test PRIVATE
            Qt6::Core Qt6::Multimedia Qt6::Test
            PkgConfig::GSTWEBRTC OpenSSL::Crypto)
        # A REAL call against a REAL homeserver, headless. Needs the Rust
        # backend (it logs in and syncs), so it is only built with it, and it
        # SKIPS unless the LIGHTNING_LIVE_* environment names a server, an
        # account and a room. Registered so it is built and kept compiling;
        # inert without credentials.
        if(ENABLE_RUST_SDK_BACKEND)
            qt_add_executable(call-live-diagnostic-test
                tests/CallLiveDiagnostic.cpp
            )
            lightning_configure_qml_test(call-live-diagnostic-test)
            target_include_directories(call-live-diagnostic-test PRIVATE src)
            target_compile_definitions(call-live-diagnostic-test PRIVATE
                HAVE_LIGHTNING_WEBRTC)
            add_test(NAME call-live-diagnostic
                COMMAND call-live-diagnostic-test)
            set_tests_properties(call-live-diagnostic PROPERTIES TIMEOUT 240)
        endif()

        add_test(NAME sfu-media-engine COMMAND sfu-media-engine-test)
        set_tests_properties(sfu-media-engine PROPERTIES TIMEOUT 120)
        # A real ICE+DTLS handshake needs a little longer than unit suites.
        set_tests_properties(call-media-loopback PROPERTIES TIMEOUT 120)
    endif()

    # v0.6.0: SDK-backed thread foundation — lifecycle (open/close/switch),
    # root/reply identity via the composite timeline id, failure states,
    # thread-only send path, encrypted-thread fixtures, participants.
    qt_add_executable(thread-controller-test
        tests/ThreadControllerTest.cpp
        src/threads/ThreadController.h
        src/threads/ThreadController.cpp
        src/models/SlashCommands.h
        src/models/SlashCommands.cpp
        src/threads/ThreadManager.h
        src/threads/ThreadManager.cpp
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
        src/media/StagedImageStore.h
        src/media/StagedImageStore.cpp
        # v0.7: see attachment-queue-test — the poster hook keeps this
        # suite free of any real media backend.
        src/media/VideoPosterExtractor.h
        src/media/VideoPosterExtractor.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        # MockMatrixClient's registry-backed restore needs these:
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        # v0.7.x: ThreadController's draft hooks pull in the store.
        src/app/DraftStore.h
        src/app/DraftStore.cpp
    )
    target_include_directories(thread-controller-test PRIVATE src)
    target_link_libraries(thread-controller-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Qml Qt6::Test)
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
    # logout clearing. Also the show-QR leg: ready -> scanned -> confirm ->
    # done, the QR-to-SAS fallback, terminal retirement of the code, and
    # flow scoping. Slots QSKIP without the Rust backend.
    qt_add_executable(verification-flow-test
        tests/VerificationFlowTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(verification-flow-test)
    target_link_libraries(verification-flow-test PRIVATE Qt6::Test)
    add_test(NAME verification-flow COMMAND verification-flow-test)
    set_tests_properties(verification-flow PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Show-QR rendering: packed grid -> QImage (module placement, quiet
    # zone, whole-module scaling), unknown/stale token renders nothing,
    # malformed geometry refused, and clear() dropping the code so it
    # cannot outlive its flow.
    qt_add_executable(qr-image-provider-test
        tests/QrImageProviderTest.cpp
        src/crypto/QrImageProvider.h
        src/crypto/QrImageProvider.cpp
    )
    target_include_directories(qr-image-provider-test PRIVATE src)
    target_link_libraries(qr-image-provider-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
    add_test(NAME qr-image-provider COMMAND qr-image-provider-test)
    set_tests_properties(qr-image-provider PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The show-QR panel inside the REAL SettingsScreen verification card:
    # present only while the SDK has a live code, opaque provider URL (no
    # flow id), the confirm prompt gated on the SDK's scan report, and the
    # panel retiring on dismissal and on completion. Zero QML warnings.
    qt_add_executable(verification-qr-qml-test
        tests/VerificationQrQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(verification-qr-qml-test)
    target_link_libraries(verification-qr-qml-test PRIVATE Qt6::Test)
    add_test(NAME verification-qr-qml COMMAND verification-qr-qml-test)
    set_tests_properties(verification-qr-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Server-synchronized per-room notification modes: the AppController
    # entry point (cache-first write, capability-gated server write and
    # refresh), account-scoped SettingsManager storage with legacy-key
    # fallback (mode 0 removes the scoped key only when no legacy value
    # shadows it), and cache reconciliation from USER-DEFINED backend
    # reports only — gated by the per-room sync-failure state (a differing
    # report never overrides an unsynced local choice).
    # Rust-backend slots QSKIP without ENABLE_RUST_SDK_BACKEND.
    qt_add_executable(room-notification-mode-test
        tests/RoomNotificationModeTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(room-notification-mode-test)
    target_link_libraries(room-notification-mode-test PRIVATE Qt6::Test)
    add_test(NAME room-notification-mode COMMAND room-notification-mode-test)

    # v0.7 thread parity: ownership of the single shared VoiceRecorder.
    # Pins that a live recording is refused-not-stolen, which is what keeps
    # the microphone from being orphaned with no owner and no UI to stop it.
    qt_add_executable(voice-ownership-test
        tests/VoiceOwnershipTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(voice-ownership-test)
    target_link_libraries(voice-ownership-test PRIVATE Qt6::Test)
    add_test(NAME voice-ownership COMMAND voice-ownership-test)
    set_tests_properties(room-notification-mode PROPERTIES
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
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        src/notifications/FallbackAvatar.h
        src/notifications/FallbackAvatar.cpp
        src/theme/IdentityColors.h
        src/theme/IdentityColors.cpp
        src/matrix/EventPreview.h
        src/matrix/EventPreview.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
    )
    target_include_directories(notification-manager-test PRIVATE src)
    target_link_libraries(notification-manager-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
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
        # Not a link-preview dependency, despite the name of the class that
        # pulls it in: LinkPreviewController::linkifiedBody() is the TIMELINE
        # BODY renderer for a message with no formatted_body (MessageDelegate
        # binds bodyLabel.text to it), and it sizes inline emoji through
        # MessageHtml::markEmoji for the same reason sanitize() does — so the
        # two body paths cannot show one emoji at two sizes. MessageHtml is
        # Qt6::Core only, so this adds no library to a target that links
        # Qt6::Core alone.
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
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
        src/storage/PortableMode.cpp
        src/media/PlayableFileWriter.h
        src/media/PlayableFileWriter.cpp
        src/app/GuiStallTracer.cpp
        src/media/VideoPosterExtractor.h
        src/media/VideoPosterExtractor.cpp
        src/media/MediaImageProvider.h
        src/media/MediaImageProvider.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(media-bridge-test PRIVATE src)
    # Gui/Multimedia only for the poster extractor MediaBridge now owns;
    # tests never construct it (lazy), so no multimedia backend spins up.
    # Quick for MediaImageProvider: the decode-bound cases call requestImage
    # directly. No window or scene graph is created.
    target_link_libraries(media-bridge-test
        PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Quick Qt6::Test)
    add_test(NAME media-bridge COMMAND media-bridge-test)

    # v0.7 voice round: waveform bucket math (max-preserving downsample,
    # loudness normalization, bounds) — pure static helper, no audio
    # backend is ever constructed.
    qt_add_executable(voice-recorder-test
        tests/VoiceRecorderTest.cpp
        src/media/VoiceRecorder.h
        src/media/VoiceRecorder.cpp
        # mediaScratchRoot(): the recorder writes into the portable
        # folder when portable, the OS temp directory otherwise.
        src/storage/PortableMode.cpp
    )
    target_include_directories(voice-recorder-test PRIVATE src)
    target_link_libraries(voice-recorder-test
        PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME voice-recorder COMMAND voice-recorder-test)

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
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        src/matrix/BridgeNetwork.h
        src/matrix/BridgeNetwork.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-state-model-test PRIVATE src)
    # The section-label contract reads RoomsPanel.qml: a category C++ can
    # emit with no label in QML renders under the wrong header.
    target_compile_definitions(room-state-model-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(room-state-model-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-state-model COMMAND room-state-model-test)

    # v0.6.5: matrix.to room permalink formatting (1d Copy room link) —
    # alias preferred, id fallback, messagePermalink's encoding convention.
    qt_add_executable(room-permalink-test
        tests/RoomPermalinkTest.cpp
        src/models/RoomListModel.h
        src/models/RoomListModel.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        src/matrix/BridgeNetwork.h
        src/matrix/BridgeNetwork.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-permalink-test PRIVATE src)
    target_link_libraries(room-permalink-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-permalink COMMAND room-permalink-test)

    # Unified inbox: recognising which bridged network a conversation belongs
    # to from a ghost user id or a portal alias. The negatives carry the
    # weight — mislabelling an ordinary contact with an underscore in their
    # localpart is worse than showing no badge.
    qt_add_executable(bridge-network-test
        tests/BridgeNetworkTest.cpp
        src/matrix/BridgeNetwork.h
        src/matrix/BridgeNetwork.cpp
    )
    target_include_directories(bridge-network-test PRIVATE src)
    target_link_libraries(bridge-network-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME bridge-network COMMAND bridge-network-test)

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
        src/storage/PortableMode.cpp
    )
    target_include_directories(quick-switcher-test PRIVATE src)
    target_link_libraries(quick-switcher-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME quick-switcher COMMAND quick-switcher-test)

    # v0.7.x Matrix presence: PresenceManager policy — watch bookkeeping,
    # batch application, unknown-vs-offline honesty, the disabled-server
    # latch, session clearing, publication gating, batch rotation.
    qt_add_executable(presence-manager-test
        tests/PresenceManagerTest.cpp
        src/presence/PresenceManager.h
        src/presence/PresenceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        # MockMatrixClient's registry-backed restore needs these:
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(presence-manager-test PRIVATE src)
    target_link_libraries(presence-manager-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME presence-manager COMMAND presence-manager-test)

    # Profile banners (MSC4427 over MSC4133): the policy half. "No banner",
    # "not asked yet" and "this server has no extended profiles" are three
    # different facts that all render as nothing.
    qt_add_executable(profile-banner-test
        tests/ProfileBannerTest.cpp
        src/profile/ProfileBannerManager.h
        src/profile/ProfileBannerManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(profile-banner-test PRIVATE src)
    target_link_libraries(profile-banner-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME profile-banner COMMAND profile-banner-test)

    # Profile bios (MSC4440 over MSC4133) + the decorative badge table. Same
    # honesty as banners: "no bio", "not asked yet" and "this server has no
    # extended profiles" are three different facts that all render as nothing,
    # and only the third stops the client asking.
    qt_add_executable(profile-bio-badge-test
        tests/ProfileBioBadgeTest.cpp
        src/profile/ProfileBioManager.h
        src/profile/ProfileBioManager.cpp
        src/profile/ProfileBadges.h
        src/profile/ProfileBadges.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/MediaHelpers.h
        src/matrix/MediaHelpers.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(profile-bio-badge-test PRIVATE src)
    target_link_libraries(profile-bio-badge-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME profile-bio-badge COMMAND profile-bio-badge-test)

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

    # 0.8.x: the crop/adjust pre-step every display-image upload goes through.
    # The maths is pure and lives in `imagecrop::`; this proves the rectangle,
    # the cap and the FORMAT HONESTY (bytes are the format they are named) and
    # that an SVG is refused by magic before anything decodes it.
    qt_add_executable(image-crop-test
        tests/ImageCropTest.cpp
        src/media/ImageCropper.h
        src/media/ImageCropper.cpp
        src/media/StagedImageStore.h
        src/media/StagedImageStore.cpp
    )
    target_include_directories(image-crop-test PRIVATE src)
    target_link_libraries(image-crop-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME image-crop COMMAND image-crop-test)

    # 0.8.x: the wiring contract — ONE crop dialog, every display-image upload
    # site routed through it, and the sites deliberately left alone (chat
    # attachments, Save-As, the local app icon) still left alone.
    qt_add_executable(image-crop-contract-test
        tests/ImageCropDialogContractTest.cpp
    )
    target_compile_definitions(image-crop-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(image-crop-contract-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME image-crop-contract COMMAND image-crop-contract-test)

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
    lightning_configure_qml_test(theme-snapshot-test)
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
    lightning_configure_qml_test(design-acceptance-test)
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
    lightning_configure_qml_test(settings-shell-qml-test)
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
    lightning_configure_qml_test(login-repair-qml-test)
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
    lightning_configure_qml_test(composer-qml-test)
    target_link_libraries(composer-qml-test PRIVATE Qt6::Test)
    add_test(NAME composer-qml COMMAND composer-qml-test)
    set_tests_properties(composer-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-18 tester report (Element chat export): the Home surface's
    # buttons actually fire, reactions wrap, a redacted message cannot be
    # pinned, Alt+V opens skin tones, a clicked room link does not flash a
    # modal, voice recording has pause/done/preview, the timeline has
    # middle-click autoscroll, and the audio card fits and remembers.
    qt_add_executable(tester-report-fixes-test
        tests/TesterReportFixesTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(tester-report-fixes-test)
    target_compile_definitions(tester-report-fixes-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(tester-report-fixes-test PRIVATE Qt6::Test)
    add_test(NAME tester-report-fixes COMMAND tester-report-fixes-test)
    set_tests_properties(tester-report-fixes PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 2026-08-18 tester report: the shell must stay usable at the app's own
    # minimum window size — the composer keeps a readable width and still
    # grows with a multi-line draft in a short window.
    qt_add_executable(narrow-shell-layout-test
        tests/NarrowShellLayoutTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(narrow-shell-layout-test)
    target_link_libraries(narrow-shell-layout-test PRIVATE Qt6::Test)
    add_test(NAME narrow-shell-layout COMMAND narrow-shell-layout-test)
    set_tests_properties(narrow-shell-layout PROPERTIES
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
    lightning_configure_qml_test(creation-dialog-qml-test)
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
    # runs the real lightning-matrix binary and asserts --build-info's
    # screenshot_demo_compiled line and (when the option is OFF, i.e. every
    # release/default build) that --screenshot-demo is rejected in preflight.
    qt_add_executable(screenshot-demo-exclusion-test
        tests/ScreenshotDemoExclusionTest.cpp
    )
    target_link_libraries(screenshot-demo-exclusion-test PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(screenshot-demo-exclusion-test PRIVATE
        "SCREENSHOT_DEMO_BINARY=\"$<TARGET_FILE:lightning-matrix>\"")
    if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
        target_compile_definitions(screenshot-demo-exclusion-test PRIVATE
            LIGHTNING_TEST_DEMO_COMPILED=1)
    endif()
    add_dependencies(screenshot-demo-exclusion-test lightning-matrix)
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
        src/storage/PortableMode.cpp
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
        lightning_configure_qml_test(screenshot-demo-accounts-test)
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
            src/storage/PortableMode.cpp
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
            src/storage/PortableMode.cpp
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
        lightning_configure_qml_test(screenshot-demo-scenario-test)
        target_compile_definitions(screenshot-demo-scenario-test PRIVATE
            LIGHTNING_ENABLE_SCREENSHOT_DEMO=1
            # The Channels case reads the controller's own source: which
            # navigation layout a scenario selects, and in what ORDER, is not
            # observable from the catalogue it exposes.
            "SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
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
        lightning_configure_qml_test(screenshot-demo-panel-qml-test)
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

    # v0.9 slash commands: registry, parse rules (// escape, letters-only
    # command words), and completion matching. Pure, no composer needed.
    qt_add_executable(slash-commands-test
        tests/SlashCommandsTest.cpp
        src/models/SlashCommands.h
        src/models/SlashCommands.cpp
        src/models/MentionTokenizer.h
        src/models/MentionTokenizer.cpp
    )
    target_include_directories(slash-commands-test PRIVATE src)
    target_link_libraries(slash-commands-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME slash-commands COMMAND slash-commands-test)

    # v0.9 rich composer: the QTextDocument -> Matrix serializer. Every
    # formatting type's HTML shape, the plain fallback derived beside it,
    # the whitelist property, and link-target safety.
    qt_add_executable(rich-composition-test
        tests/RichCompositionTest.cpp
        src/models/RichComposition.h
        src/models/RichComposition.cpp
    )
    target_include_directories(rich-composition-test PRIVATE src)
    target_link_libraries(rich-composition-test
        PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME rich-composition COMMAND rich-composition-test)
    set_tests_properties(rich-composition PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

    # The environment a spawned browser inherits. Inside an AppImage that
    # environment points at the bundle, so xdg-open's browser loads the
    # bundle's glib ahead of the host's and never starts -- reported as
    # "clicking links doesn't open them in browser". Asserted on the
    # environment, because what a browser does with it is not observable.
    qt_add_executable(url-launcher-test
        tests/UrlLauncherTest.cpp
        src/app/UrlLauncher.h
        src/app/UrlLauncher.cpp
    )
    target_include_directories(url-launcher-test PRIVATE src)
    target_link_libraries(url-launcher-test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME url-launcher COMMAND url-launcher-test)

    # The composer's emoji face, asserted on the FORMAT the document ends up
    # with. Qt's per-character fallback differs by version, so emoji have to be
    # NAMED; the composer is mixed text, so the face is applied per-range by the
    # highlighter rather than as a font.family. The first attempt set only the
    # Qt 6 list API and the composer kept the default face while the picker was
    # already correct -- a source scan sees the call, only reading the applied
    # format shows which property the layout honours. Needs Qt6::Gui for
    # QTextDocument.
    qt_add_executable(mention-highlighter-emoji-test
        tests/MentionHighlighterEmojiTest.cpp
        src/models/MentionHighlighter.h
        src/models/MentionHighlighter.cpp
    )
    target_include_directories(mention-highlighter-emoji-test PRIVATE src)
    target_link_libraries(mention-highlighter-emoji-test
        PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
    add_test(NAME mention-highlighter-emoji
             COMMAND mention-highlighter-emoji-test)

    # Legacy Matrix SSO (m.login.sso): the loopback callback listener driven
    # over REAL sockets in both flows, plus AuthManager's discovery/login
    # policy. Covers token extraction, missing/empty token, duplicate and
    # stale callbacks, malformed and oversized requests, timeout,
    # cancellation, provider scoping, and that OAuth callback parsing is
    # unaffected. Needs a QGuiApplication for the Qt network event loop.
    qt_add_executable(sso-callback-test
        tests/SsoCallbackTest.cpp
    )
    target_include_directories(sso-callback-test PRIVATE src)
    target_link_libraries(sso-callback-test
        PRIVATE lightning-app-testlib Qt6::Test)
    add_test(NAME sso-callback COMMAND sso-callback-test)

    # v0.9 slash commands, composer half: which backend verb a command runs,
    # with which arguments and body spec, and the draft-preservation contract
    # (a refusal never destroys what the user typed).
    qt_add_executable(slash-command-composer-test
        tests/SlashCommandComposerTest.cpp
    )
    target_include_directories(slash-command-composer-test PRIVATE src)
    target_link_libraries(slash-command-composer-test
        PRIVATE lightning-app-testlib Qt6::Test)
    add_test(NAME slash-command-composer COMMAND slash-command-composer-test)

    # The opt-in sync-latency tracer: inert when disabled, no room ids, an
    # unobserved stage is not reported as instant, bounded journey table, and
    # a sync gap past the threshold is reported.
    qt_add_executable(sync-latency-tracer-test
        tests/SyncLatencyTracerTest.cpp
        src/app/SyncLatencyTracer.h
        src/app/SyncLatencyTracer.cpp
    )
    target_include_directories(sync-latency-tracer-test PRIVATE src)
    target_link_libraries(sync-latency-tracer-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME sync-latency-tracer COMMAND sync-latency-tracer-test)

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
        src/storage/PortableMode.cpp
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

    # Inline emoji sizing (MessageHtml::markEmoji): what a body renders as
    # when it carries an emoji, including the big-emoji suppression and the
    # code-span exclusion. Behavioural, not a source scan.
    qt_add_executable(message-emoji-size-test
        tests/MessageEmojiSizeTest.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
    )
    target_include_directories(message-emoji-size-test PRIVATE src)
    target_link_libraries(message-emoji-size-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME message-emoji-size COMMAND message-emoji-size-test)

    # Shared control system: rendered proof that AppButton kinds,
    # SegmentedControl, AppTextField, and the AppComboBox popup are flat,
    # token-driven, and theme-tracking — no Fusion bevels or gradients.
    qt_add_executable(control-system-test
        tests/ControlSystemTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(control-system-test)
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
    lightning_configure_qml_test(button-system-test)
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
    lightning_configure_qml_test(menu-system-qml-test)
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
    lightning_configure_qml_test(mention-popup-contract-test)
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
    lightning_configure_qml_test(identity-card-account-menu-test)
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
    lightning_configure_qml_test(member-profile-popover-contract-test)
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
    lightning_configure_qml_test(trust-card-test)
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
    lightning_configure_qml_test(invite-people-dialog-qml-test)
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
    lightning_configure_qml_test(quick-switcher-modes-qml-test)
    target_compile_definitions(quick-switcher-modes-qml-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(quick-switcher-modes-qml-test PRIVATE Qt6::Test)
    add_test(NAME quick-switcher-modes-qml
        COMMAND quick-switcher-modes-qml-test)
    set_tests_properties(quick-switcher-modes-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # ------------------------------------------------------------------
    # Update system. Every one of these links only the handful of sources it
    # exercises -- never the application -- so they compile in seconds and an
    # updater change does not drag the whole app graph with it.
    set(_update_crypto_sources
        src/update/Version.cpp
        src/update/InstallType.cpp
        src/update/UpdateTrustStore.cpp
        src/update/SignatureVerifier.cpp
        src/update/UpdateEndpoints.cpp
        src/update/UpdateManifest.cpp)

    qt_add_executable(update-version-test
        tests/UpdateVersionTest.cpp src/update/Version.cpp)
    target_include_directories(update-version-test PRIVATE src)
    target_link_libraries(update-version-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME update-version COMMAND update-version-test)

    qt_add_executable(update-install-type-test
        tests/UpdateInstallTypeTest.cpp src/update/InstallType.cpp)
    target_include_directories(update-install-type-test PRIVATE src)
    target_link_libraries(update-install-type-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME update-install-type COMMAND update-install-type-test)

    # Signature and hash verification: the tests generate a throwaway Ed25519
    # keypair at run time, so no key material is committed anywhere.
    qt_add_executable(update-manifest-test
        tests/UpdateManifestTest.cpp ${_update_crypto_sources})
    target_include_directories(update-manifest-test PRIVATE src)
    target_link_libraries(update-manifest-test
        PRIVATE Qt6::Core Qt6::Test OpenSSL::Crypto)
    add_test(NAME update-manifest COMMAND update-manifest-test)

    qt_add_executable(update-manager-state-test
        tests/UpdateManagerStateTest.cpp
        ${_update_crypto_sources}
        src/update/UpdateDownloader.cpp
        src/update/UpdateManager.cpp
        # UpdateManager stages downloads under the Lightning-owned cache root,
        # which is portable-aware — a portable copy must stage inside its own
        # folder rather than %LOCALAPPDATA%.
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp)
    target_include_directories(update-manager-state-test PRIVATE src)
    # The suite scans the update sources to prove no GitHub API is reachable
    # from them. Give it the path rather than letting it guess from the binary
    # location, so it can hard-fail instead of silently finding nothing.
    target_compile_definitions(update-manager-state-test PRIVATE
        LIGHTNING_UPDATE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/update")

    # Both update suites compile src/update/*.cpp themselves, so without these
    # they would validate the header DEFAULTS while the shipped application
    # validates the configured values -- the suites would stop being evidence
    # about a build that overrides either variable.
    foreach(_update_test IN ITEMS update-manifest-test update-manager-state-test)
        target_compile_definitions(${_update_test} PRIVATE
            LIGHTNING_UPDATE_HOST="${LIGHTNING_UPDATE_HOST}"
            LIGHTNING_UPDATE_MIRROR_HOSTS="${LIGHTNING_UPDATE_MIRROR_HOSTS_MACRO}"
            LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE="${LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE}")
    endforeach()
    target_link_libraries(update-manager-state-test
        PRIVATE Qt6::Core Qt6::Network Qt6::Qml Qt6::Test OpenSSL::Crypto)
    add_test(NAME update-manager-state COMMAND update-manager-state-test)

    qt_add_executable(updater-helper-args-test
        tests/UpdaterHelperArgsTest.cpp
        src/updater/UpdaterArgs.cpp src/updater/InstallStrategies.cpp)
    target_include_directories(updater-helper-args-test PRIVATE src)
    target_link_libraries(updater-helper-args-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME updater-helper-args COMMAND updater-helper-args-test)

    qt_add_executable(updater-zip-safety-test
        tests/UpdaterZipSafetyTest.cpp src/updater/SafeArchive.cpp)
    target_include_directories(updater-zip-safety-test PRIVATE src)
    target_link_libraries(updater-zip-safety-test PRIVATE Qt6::Core Qt6::Test)
    if(TARGET ZLIB::ZLIB)
        target_link_libraries(updater-zip-safety-test PRIVATE ZLIB::ZLIB)
        target_compile_definitions(updater-zip-safety-test
            PRIVATE LIGHTNING_UPDATER_HAVE_ZLIB=1)
    endif()
    add_test(NAME updater-zip-safety COMMAND updater-zip-safety-test)

    qt_add_executable(updater-portable-swap-test
        tests/UpdaterPortableSwapTest.cpp src/updater/AtomicReplace.cpp)
    target_include_directories(updater-portable-swap-test PRIVATE src)
    target_link_libraries(updater-portable-swap-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME updater-portable-swap COMMAND updater-portable-swap-test)

    # QML contract scan for the Updates settings surface. Needs no app graph.
    qt_add_executable(updates-ui-contract-test tests/UpdatesUiContractTest.cpp)
    target_link_libraries(updates-ui-contract-test PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(updates-ui-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    add_test(NAME updates-ui-contract COMMAND updates-ui-contract-test)

    # tests/ShortcutRegistryTest.cpp shipped with the rebindable-shortcuts
    # feature in cca3011 and was NEVER REGISTERED HERE, so its 19 cases have
    # never been built or run once. Found while fixing the thread composer's
    # Ctrl+B: the suite that covers conflict refusal, reserved sequences and
    # global-vs-editor shadowing was dead the whole time that code shipped.
    qt_add_executable(shortcut-registry-test
        tests/ShortcutRegistryTest.cpp
        src/app/ShortcutRegistry.h
        src/app/ShortcutRegistry.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(shortcut-registry-test PRIVATE src)
    target_link_libraries(shortcut-registry-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME shortcut-registry COMMAND shortcut-registry-test)

    # 2026-08-28: when a rail folder exists and when it stops existing. The
    # deletion rule Rokas asked for ("if a folder is empty, delete it") is a
    # fact about the STORE and never about what the rail managed to render,
    # and the same suite pins the drag defect that was manufacturing empty
    # folders: a reorder named every folder row in its atomic write, which
    # emptied every COLLAPSED folder it passed.
    qt_add_executable(rail-folder-lifecycle-test
        tests/RailFolderLifecycleTest.cpp
        src/spaces/RailLayoutStore.h
        src/spaces/RailLayoutStore.cpp
        src/spaces/RailEntryModel.h
        src/spaces/RailEntryModel.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(rail-folder-lifecycle-test PRIVATE src)
    target_link_libraries(rail-folder-lifecycle-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME rail-folder-lifecycle COMMAND rail-folder-lifecycle-test)

    # 2026-08-28: the People list scoped to the selected Space — the Space
    # roster SpaceManager fetches, the Classic list's DM scope, and the
    # Channels Space view's People group. The fail directions are opposite in
    # the two layouts and that is the point of the suite.
    qt_add_executable(space-people-scope-test
        tests/SpacePeopleScopeTest.cpp
        src/models/RoomListModel.h
        src/models/RoomListModel.cpp
        src/matrix/BridgeNetwork.h
        src/matrix/BridgeNetwork.cpp
        src/models/SpaceChannelModel.h
        src/models/SpaceChannelModel.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/spaces/RailLayoutStore.h
        src/spaces/RailLayoutStore.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(space-people-scope-test PRIVATE src)
    target_link_libraries(space-people-scope-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME space-people-scope COMMAND space-people-scope-test)

    # 2026-08-31: unread badge ATTRIBUTION. Two direct messages were missed
    # because the Direct Messages tile is a synthesised pseudo row that never
    # carried an unread total at all, while Home counted DMs and Space rooms
    # it does not list. The invariant is one line — a tile's badge counts what
    # that tile's view lists — and every case asserts BOTH halves, so a badge
    # that is merely non-zero cannot pass.
    qt_add_executable(unread-badge-attribution-test
        tests/UnreadBadgeAttributionTest.cpp
        src/models/SpaceChannelModel.h
        src/models/SpaceChannelModel.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/spaces/RailEntryModel.h
        src/spaces/RailEntryModel.cpp
        src/spaces/RailLayoutStore.h
        src/spaces/RailLayoutStore.cpp
        src/app/SettingsManager.h
        src/app/SettingsManager.cpp
        src/storage/SecretStore.h
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(unread-badge-attribution-test PRIVATE src)
    target_link_libraries(unread-badge-attribution-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME unread-badge-attribution
             COMMAND unread-badge-attribution-test)

endif()

# ----------------------------------------------------------------------
# The update helper. A deliberately tiny, separate binary: it performs the one
# step that cannot happen while Lightning is running, and its link line IS its
# isolation guarantee. Qt6::Core only -- no Network (it never downloads), no
# Qml/Gui, and none of src/matrix, src/app or src/storage, so it cannot reach a
# Matrix account, a token or a store even by mistake. It only ever receives an
# artifact Lightning has already verified.
qt_add_executable(lightning-updater
    src/updater/main.cpp
    src/updater/UpdaterArgs.h        src/updater/UpdaterArgs.cpp
    src/updater/ProcessWaiter.h      src/updater/ProcessWaiter.cpp
    src/updater/SafeArchive.h        src/updater/SafeArchive.cpp
    src/updater/InstallStrategies.h  src/updater/InstallStrategies.cpp
    src/updater/AtomicReplace.h      src/updater/AtomicReplace.cpp
)
target_include_directories(lightning-updater PRIVATE src)
target_link_libraries(lightning-updater PRIVATE Qt6::Core)
if(TARGET ZLIB::ZLIB)
    target_link_libraries(lightning-updater PRIVATE ZLIB::ZLIB)
    target_compile_definitions(lightning-updater PRIVATE LIGHTNING_UPDATER_HAVE_ZLIB=1)
else()
    message(STATUS "zlib not found: the updater will refuse deflated portable archives")
endif()
if(MSVC)
    target_compile_options(lightning-updater PRIVATE /W4 /permissive-)
else()
    target_compile_options(lightning-updater PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

install(TARGETS lightning-matrix lightning-updater
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
# The scalable slot: launchers and docks that support SVG prefer it over
# any raster size, so the mark stays crisp at every zoom/DPI. Same "thick"
# artwork the raster sizes are generated from.
install(FILES data/icons/lightning.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    RENAME lightning.svg)

# Dismissing a link preview: per-(room,event) keying so one click can never
# collapse a card above the reader, the dismissal checked before the auto-load
# dispatch, undo that grants no consent, and the bounded oldest-released set.
# Appended (rather than placed beside link-preview-test) purely to avoid
# colliding with concurrent edits elsewhere in this file.
if(BUILD_TESTING)
    qt_add_executable(link-preview-dismiss-test
        tests/LinkPreviewDismissTest.cpp
        src/models/LinkPreview.h
        src/models/LinkPreview.cpp
        src/models/LinkPreviewController.h
        src/models/LinkPreviewController.cpp
        # linkifiedBody() renders the timeline body, so the controller pulls
        # in MessageHtml; it is Qt6::Core only and adds no library here.
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(link-preview-dismiss-test PRIVATE src)
    target_link_libraries(link-preview-dismiss-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME link-preview-dismiss COMMAND link-preview-dismiss-test)
endif()

# ---------------------------------------------------------------------------
# Composer spell checking (2026-08-28).
#
# Appended rather than added to APP_SOURCES because that list is consumed
# above (APP_LIB_SOURCES is deliberately blanked once the shared test library
# has taken it), and because this file is shared with concurrent work: an
# append cannot disturb a list another round is editing.
#
# Both consumers get the sources: the application, and the shared test
# library every C++/QML test target links. The testlib only exists when tests
# are enabled, hence the TARGET guard.
# ---------------------------------------------------------------------------
set(LIGHTNING_SPELL_SOURCES
    src/text/SpellBackend.h
    src/text/SpellBackend.cpp
    src/text/SpellChecker.h
    src/text/SpellChecker.cpp
)
target_sources(lightning-matrix PRIVATE ${LIGHTNING_SPELL_SOURCES})
if(TARGET lightning-app-testlib)
    target_sources(lightning-app-testlib PRIVATE ${LIGHTNING_SPELL_SOURCES})
endif()

# Composer spell checking + the tray badge (2026-08-28). Deliberately a
# STANDALONE target rather than one linking lightning-app-testlib: everything
# under test here is pure — a tokenizer, a policy, and a static label rule —
# and the link list is part of the assertion. SpellChecker sees Qt and its own
# backend interface and nothing else; nothing that could hand it a network, a
# room, or a message it was not given.
if(BUILD_TESTING)
    qt_add_executable(composer-spell-test
        tests/ComposerSpellTest.cpp
        src/text/SpellChecker.h
        src/text/SpellChecker.cpp
        src/text/SpellBackend.h
        src/text/SpellBackend.cpp
        src/app/TrayIcon.h
        src/app/TrayIcon.cpp
    )
    target_include_directories(composer-spell-test PRIVATE src)
    target_compile_definitions(composer-spell-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(composer-spell-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    add_test(NAME composer-spell COMMAND composer-spell-test)
endif()

# The spell underline against the PRODUCTION composer, not against the policy
# behind it. Offscreen, like every other QML suite here.
if(BUILD_TESTING)
    qt_add_executable(composer-spell-qml-test
        tests/ComposerSpellQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(composer-spell-qml-test)
    target_link_libraries(composer-spell-qml-test PRIVATE Qt6::Test)
    add_test(NAME composer-spell-qml COMMAND composer-spell-qml-test)
    set_tests_properties(composer-spell-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
