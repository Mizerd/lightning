cmake_minimum_required(VERSION 3.21)
project(lightning
    VERSION 0.9.9
    DESCRIPTION "Lightning — native C++/Qt Matrix desktop client"
    LANGUAGES CXX
)

include(CTest)

# Human-facing release label. Keep in sync with project() and rust/Cargo.toml.
set(APP_VERSION_LABEL "0.9.9")

# Build metadata surfaced by `lightning-matrix --build-info` (never secrets).
# The source SHA comes from git at configure time; packaging may override it.
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

# Auto-enable ccache for source builds only; an explicit compiler launcher
# wins, and packaged builds (LIGHTNING_ARTIFACT_KIND != "source") never pick
# up a developer's cache.
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

# Update-manifest signatures are verified with Ed25519 via OpenSSL 3's EVP API.
# REQUIRED: an updater must never silently lose signature verification. Only
# libcrypto is used; TLS stays with Qt.
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# zlib for the updater's ZIP reader (portable archives are deflated). Must be
# found before the test targets so updater-zip-safety-test covers inflate.
# Optional: without it, deflated entries are refused with a typed error.
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
    "Canonical host: the FIRST source of update metadata (the mirror's fixed slot is the fallback), and the fallback for artifacts")
set(LIGHTNING_UPDATE_PROJECT_ID "6" CACHE STRING
    "GitLab project id hosting the update package registry")

# Hosts besides the canonical one that may serve artifact bytes, and the
# fallback manifest pair (isAllowedFallbackManifestUrl uses the same list).
# Safe because the pair is verified against the compiled-in Ed25519 key before
# use; see docs/updates.md and src/update/UpdateEndpoints.h. github.com
# redirects to the *.githubusercontent.com hosts.
set(LIGHTNING_UPDATE_MIRROR_HOSTS
    "github.com;objects.githubusercontent.com;release-assets.githubusercontent.com"
    CACHE STRING
    "Semicolon-separated hosts allowed to serve update ARTIFACTS (never metadata)")

# A list would expand to separate -D arguments in target_compile_definitions
# ($<SEMICOLON> does not help), so pass a comma-separated string;
# UpdateEndpoints.cpp accepts either separator.
string(REPLACE ";" "," LIGHTNING_UPDATE_MIRROR_HOSTS_MACRO
    "${LIGHTNING_UPDATE_MIRROR_HOSTS}")

# Mirrored copy of the signed manifest pair, used only when the canonical host
# is unreachable; empty disables it. The signature and the strictly-newer rule
# apply identically, so a stale or hostile mirror can at worst offer no update.
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

# Release guard: drop the HTTP and mock backends from lightning-matrix so a
# package can only run the Rust SDK backend. Packaging sets it; dev builds keep
# mock/http. Affects only the main target; tests build their own mock client.
option(LIGHTNING_RUST_ONLY
    "Ship ONLY the Rust backend: drop HTTP/mock from the lightning-matrix binary" OFF)
if(LIGHTNING_RUST_ONLY AND NOT ENABLE_RUST_SDK_BACKEND)
    message(FATAL_ERROR
        "LIGHTNING_RUST_ONLY=ON requires ENABLE_RUST_SDK_BACKEND=ON "
        "(a Rust-only build must have the Rust backend compiled in).")
endif()

# ---------------------------------------------------------------------------
# Development-only --screenshot-demo mode: the real UI on the mock backend with
# deterministic fake data. Off by default and incompatible with
# LIGHTNING_RUST_ONLY, so a release binary never contains it.
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
# Build-only GIF provider keys. A -P script reads
# LIGHTNING_BUILD_GIPHY_API_KEY / LIGHTNING_BUILD_KLIPY_API_KEY from the
# environment and writes them to a build-tree header only; they never reach a
# compiler command line, CMakeCache.txt, install rules or logs. Source builds
# stay keyless. LIGHTNING_REQUIRE_GIF_KEYS fails the build if either is absent.
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
    # Inline video/audio playback.
    Multimedia
)
# Qt SVG library for thumbnails of SVGs the user sends. Linking it makes the
# deploy tools add the qsvg image PLUGIN, which the AppImage, macOS and Windows
# builds leave out: nothing received is decoded as SVG. Global so every target
# compiling SvgThumbnail.h sees one definition.
option(LIGHTNING_REQUIRE_QT_SVG "Fail configure without Qt SVG" OFF)
find_package(Qt6 6.5 QUIET COMPONENTS Svg)
if(TARGET Qt6::Svg)
    add_compile_definitions(LIGHTNING_HAVE_QT_SVG=1)
    link_libraries(Qt6::Svg)
    message(STATUS "SVG send thumbnails: yes (Qt SVG ${Qt6Svg_VERSION})")
elseif(LIGHTNING_REQUIRE_QT_SVG)
    message(FATAL_ERROR
        "LIGHTNING_REQUIRE_QT_SVG=ON but Qt6::Svg was not found "
        "(Debian/Ubuntu: qt6-svg-dev; Fedora: qt6-qtsvg-devel).")
else()
    message(STATUS "SVG send thumbnails: no (Qt SVG not found)")
endif()
# QPlatformScreen, for the X11 screen-share fallback's native screen rectangle.
# Optional: the call site is guarded on LIGHTNING_HAVE_QPA_SCREEN and the
# fallback refuses without it.
find_package(Qt6 6.5 QUIET COMPONENTS GuiPrivate)
# Packaging (LIGHTNING_REQUIRE_WEBRTC) must not silently ship without it.
# Debian/Ubuntu: qt6-base-private-dev. Fedora: qt6-qtbase-private-devel.
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
# Freedesktop notifications. Optional so non-DBus platforms still build.
find_package(Qt6 6.5 COMPONENTS DBus)

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# Localization (Qt Linguist).
#
# The catalogs are hand-maintained .ts files under i18n/. English is the
# source language, but it still has a catalog for Qt plural forms.
#
# LinguistTools is optional (lrelease is a separate distro package); without
# it the app is English-only and LocalizationManager::translationsAvailable()
# reports false.
# ---------------------------------------------------------------------------
# `en` needs a catalog for its plural forms: without one Qt renders the "(s)"
# of `%n room(s)` literally.
set(LIGHTNING_LANGUAGE_CODES en zh_CN hi es ar fr bn pt ru id lt)
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
        # -nounfinished: an unfinished translation falls back to English.
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

    # Manual only: lupdate rewrites the tracked .ts files. See
    # docs/localization.md.
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

# Optional GStreamer webrtcbin call media engine, built when the GStreamer
# WebRTC dev files are present; without it CallController refuses calls. The
# engine also re-probes its element factories at runtime.
option(LIGHTNING_ENABLE_WEBRTC
    "Build the GStreamer webrtcbin voice-call media engine when available" ON)
# Packaging sets this so a missing GStreamer fails configuration instead of
# silently shipping a build without calls.
option(LIGHTNING_REQUIRE_WEBRTC
    "Fail configuration if the GStreamer WebRTC dev files are missing" OFF)
set(HAVE_LIGHTNING_WEBRTC OFF)
if(LIGHTNING_ENABLE_WEBRTC AND PkgConfig_FOUND)
    # gstreamer-app-1.0: received video goes through an appsink into a QML
    # VideoOutput's QVideoSink, since qml6glsink may be absent.
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
    # Contradictory switches.
    message(FATAL_ERROR
        "LIGHTNING_REQUIRE_WEBRTC=ON contradicts LIGHTNING_ENABLE_WEBRTC=OFF "
        "(or PkgConfig is unavailable). Pick one.")
endif()

set(APP_SOURCES
    src/main.cpp

    src/app/AccountAvatarStore.h
    src/app/AccountAvatarStore.cpp
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
    src/calls/CallSoundPolicy.h
    src/calls/CallSoundPolicy.cpp
    src/calls/CallSoundController.h
    src/calls/CallSoundController.cpp
    src/calls/CallSoundPlayer.h
    src/calls/CallSoundPlayer.cpp
    src/calls/CallDeviceController.h
    src/calls/CallDeviceController.cpp
    src/calls/CameraPortal.h
    src/calls/CameraPortal.cpp
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
    # Loopback redirect endpoint for OAuth browser sign-in (plain Qt Network,
    # driven by the Rust backend).
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
    src/storage/BridgeLabelStore.h
    src/storage/BridgeLabelStore.cpp
    src/matrix/RoomActionError.h
    src/matrix/RoomActionError.cpp
    src/app/DesktopEntryQuoting.h
    src/app/DesktopEntryQuoting.cpp
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
    src/models/RoomExport.cpp
    src/models/WidgetController.cpp
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
    src/gif/GifPreviewCache.h
    src/gif/GifPreviewCache.cpp
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
    src/profile/NameColorManager.h
    src/profile/NameColorManager.cpp
    src/profile/ProfileBioManager.h
    src/profile/ProfileBioManager.cpp
    src/profile/UserProfileResolver.h
    src/profile/UserProfileResolver.cpp
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
    src/models/MediaHistoryModel.cpp
    src/crypto/CryptoBootstrapModel.h
    src/crypto/CryptoBootstrapModel.cpp
    src/crypto/E2eeDiagnostics.h
    src/crypto/E2eeDiagnostics.cpp
    src/crypto/OwnDeviceKeyWatch.h
    src/crypto/OwnDeviceKeyWatch.cpp
    src/crypto/QrImageProvider.h
    src/crypto/QrImageProvider.cpp
    src/crypto/QrLoginController.h
    src/crypto/QrLoginController.cpp
    src/app/PolicyListController.h
    src/app/PolicyListController.cpp
    src/spaces/SpaceManager.h
    src/spaces/SpaceManager.cpp
    src/spaces/SpaceModerationController.h
    src/spaces/SpaceModerationController.cpp
    src/spaces/RoomClosureController.h
    src/spaces/RoomClosureController.cpp
    src/threads/ThreadManager.h
    src/threads/ThreadManager.cpp
    src/threads/ThreadController.h
    src/threads/ThreadController.cpp
    src/presence/PresenceManager.h
    src/presence/PresenceManager.cpp

    # Application updates. Free of Matrix dependencies: the updater is a
    # separate trust boundary with no access to accounts, tokens or stores.
    # See docs/updates.md.
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
    # Shared with the updater helper: proves the staged file is still the
    # verified one.
    src/updater/ArtifactDigest.h
    src/updater/ArtifactDigest.cpp
)

if(ENABLE_RUST_SDK_BACKEND)
    list(APPEND APP_SOURCES
        src/matrix/RustSdkMatrixClient.h
        src/matrix/RustSdkMatrixClient.cpp
        src/matrix/RustRoomRegistry.h
        src/matrix/RustRoomRegistry.cpp
        src/matrix/RustTimelineMirror.h
        src/smoke/RustSdkSmokeTest.h
        src/smoke/RustSdkSmokeTest.cpp
    )
endif()

# Development-only screenshot-demo controller (`app.demo`); compiled only when
# the demo option is on.
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
    list(APPEND APP_SOURCES
        src/app/ScreenshotDemoController.h
        src/app/ScreenshotDemoController.cpp
    )
endif()

# Everything except main.cpp, shared with the QML test targets. Keeps mock/http
# so mock-based tests build even with LIGHTNING_RUST_ONLY.
if(HAVE_LIGHTNING_WEBRTC)
    list(APPEND APP_SOURCES
        # The single GStreamer init, which applies the bundled plugin path
        # first.
        src/calls/GstBootstrap.h
        src/calls/GstBootstrap.cpp
        src/calls/GstCallMediaBackend.h
        src/calls/GstCallMediaBackend.cpp
        src/calls/SfuMediaEngine.h
        src/calls/SfuMediaEngine.cpp
        src/calls/CaptureDeviceSelection.h
        src/calls/CaptureDeviceSelection.cpp
        src/calls/RtpVp8Payloader.h
        src/calls/RtpVp8Payloader.cpp
        # Windows window capture: the shipped GStreamer has no usable element
        # that takes a window handle. Compiles to nothing off Windows.
        src/calls/WindowCaptureSrc.h
        src/calls/WindowCaptureSrc.cpp
        # Serves those captures as preview tiles to the picker.
        src/calls/ShareSourceImageProvider.h
        src/calls/ShareSourceImageProvider.cpp
        # Needs OpenSSL directly; kept out of the shared app sources so test
        # targets do not link libcrypto.
        src/calls/CallFrameCryptor.h
        src/calls/CallFrameCryptor.cpp
        # Delivers decoded received video to the UI.
        src/calls/SfuVideoRouter.h
        src/calls/SfuVideoRouter.cpp
        # Selects the application audio a screen share carries, excluding our
        # own so the call is not echoed back.
        src/calls/ShareAudioSources.h
        src/calls/ShareAudioSources.cpp)
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
    # Themed replacements for the unstyled Qt Basic controls.
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
    qml/SpaceLobby.qml
    qml/SpacesRail.qml
    qml/FolderTile.qml
    qml/RoomsPanel.qml
    qml/TimelinePane.qml
    qml/RoomDelegate.qml
    # The room row context menu, shared by both navigation layouts.
    qml/RoomActionsMenu.qml
    # The room-list column hosts two interchangeable navigation layouts.
    qml/RoomListClassicPresenter.qml
    qml/RoomChannelsPresenter.qml
    qml/ChannelDelegate.qml
    qml/ChannelCategoryHeader.qml
    qml/ChannelNavRow.qml
    # A Space IS a Matrix room: this is the room-settings backend behind a
    # Space-shaped surface, reached from the rail's menu.
    qml/SpaceMemberActionDialog.qml
    qml/SpaceSettingsDialog.qml
    qml/RoomCloseDialog.qml
    qml/UnreadBadge.qml
    # Room-row call indicator, shared by both layouts. It must never ask
    # RtcController to refresh (that falls back to a full /state per idle room).
    qml/RoomCallGlyph.qml
    # Display picker for platforms without an xdg portal.
    qml/ScreenSharePicker.qml
    qml/NavigationLayoutCard.qml
    qml/MessageDelegate.qml
    # Element-style local image hiding: keeps the media box's exact geometry.
    qml/MediaHiddenPlaceholder.qml
    # One-line summary for a collapsed media or link embed.
    qml/CollapsedEmbedRow.qml
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
    qml/ExportRoomDialog.qml
    qml/WidgetOpenSheet.qml
    qml/MediaBrowser.qml
    qml/EmojiCompletionPopup.qml
    qml/ForwardSelectionDialog.qml
    qml/MediaBrowserTile.qml
    qml/MediaBrowserRow.qml
    qml/JumpToDateDialog.qml
    qml/StickerPackEditor.qml
    qml/StickerPicker.qml
    qml/PolicyListDialog.qml
    qml/AddWidgetDialog.qml
    qml/QrLoginDialog.qml
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
    qml/CallPipWindow.qml
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
    qml/EncryptionBrokenPrompt.qml
    qml/QuickReactionStrip.qml
    qml/IdentityCard.qml
    qml/TrustCard.qml
    qml/StormBand.qml
    qml/StormNode.qml
    qml/TrustMeter.qml
    qml/CodeBlock.qml
    qml/AppTheme.qml
)

# Development-only demo control panel, added only when the demo option is on.
# Main.qml loads it by a runtime string, never a compiled type.
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

# Development-only demo media fixtures (scripts/generate-demo-media.sh),
# bundled only when the demo option is on. Resolved at runtime as
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
        # Animated fixtures for the GIF picker (scripts/generate-demo-gifs.sh);
        # demo mode has no network or provider key.
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

# Everything a Qt executable needs to run the real UI (app sources and the QML
# module). Each executable calls qt_add_qml_module on itself: a static backing
# library did not get Qt's static QML plugin auto-import to work reliably
# across target boundaries.
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
            # Selectable UI families (Settings → Appearance → Font).
            "data/fonts/Inter[wght].ttf"
            "data/fonts/IBMPlexSans[wght].ttf"
            "data/fonts/SourceSans3[wght].ttf"
            "data/fonts/PlusJakartaSans[wght].ttf"
            # Brand face for the trust surface; never a selectable body face.
            "data/fonts/SpaceGrotesk[wght].ttf"
            # Icon glyphs for interface chrome (Apache-2.0 subset; see
            # scripts/generate-icon-font.sh and qml/Icon.qml).
            data/fonts/MaterialSymbolsRounded-subset.ttf
            # Window icon for source runs and Windows, which has no icon theme.
            # 256px stays crisp on high-DPI.
            data/icons/hicolor/256x256/apps/lightning.png
            # macOS menu-bar template icon (alpha only, recoloured by AppKit).
            # Used only on macOS but listed everywhere so every target's
            # resource set is identical. Regenerate with
            # scripts/generate-macos-menubar-icon.sh.
            data/icons/menubar/lightning-template-16.png
            data/icons/menubar/lightning-template-18.png
            data/icons/menubar/lightning-template-20.png
            data/icons/menubar/lightning-template-22.png
            data/icons/menubar/lightning-template-32.png
            data/icons/menubar/lightning-template-36.png
            data/icons/menubar/lightning-template-40.png
            data/icons/menubar/lightning-template-44.png
            # Development-only demo media fixtures (empty list in a release
            # build; see DEMO_MEDIA_RESOURCES above).
            ${DEMO_MEDIA_RESOURCES}
        RESOURCE_PREFIX /qt/qml
        # Each target registers its own copy of the "MatrixClient" module and
        # needs a distinct output directory.
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml-modules/${target}"
    )

    target_include_directories(${target} ${_scope} src)

    # Embedded translation catalogs, loaded by LocalizationManager from
    # ":/i18n/lightning_<code>.qm". The resource name is per-target.
    if(LIGHTNING_TRANSLATIONS_ENABLED)
        qt_add_resources(${target} "lightning_i18n_${target}"
            PREFIX "/i18n"
            BASE "${LIGHTNING_QM_DIR}"
            FILES ${LIGHTNING_QM_FILES})
        add_dependencies(${target} lightning-translations)
        target_compile_definitions(${target} ${_scope} LIGHTNING_HAS_TRANSLATIONS=1)
    endif()
    # qmltyperegistrar probes each QML_ELEMENT header as <HeaderName.h>, so
    # every directory holding a registered type must be on the include path.
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
    # The X11 share fallback needs a screen's native rectangle, which no public
    # Qt API gives: QScreen::geometry() scales only the size and
    # devicePixelRatio() is rounded. QPlatformScreen has it. Qt6::GuiPrivate
    # adds include paths only (no new shared library). Linux only; Windows and
    # macOS capture by display index.
    if(UNIX AND NOT APPLE AND TARGET Qt6::GuiPrivate)
        target_link_libraries(${target} ${_scope} Qt6::GuiPrivate)
        target_compile_definitions(${target} ${_scope}
            LIGHTNING_HAVE_QPA_SCREEN=1)
    endif()

    # Update trust configuration: the only values the updater takes from the
    # build.
    target_compile_definitions(${target} ${_scope}
        LIGHTNING_INSTALL_TYPE="${LIGHTNING_INSTALL_TYPE}"
        LIGHTNING_UPDATE_PUBKEY_2026A="${LIGHTNING_UPDATE_PUBKEY_2026A}"
        LIGHTNING_UPDATE_HOST="${LIGHTNING_UPDATE_HOST}"
        LIGHTNING_UPDATE_MIRROR_HOSTS="${LIGHTNING_UPDATE_MIRROR_HOSTS_MACRO}"
        LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE="${LIGHTNING_UPDATE_MIRROR_MANIFEST_BASE}"
        LIGHTNING_UPDATE_PROJECT_ID="${LIGHTNING_UPDATE_PROJECT_ID}"
    )
    # Gate DBus on platform too: a Windows Qt that ships Qt6 DBus would
    # otherwise try to autolaunch a session bus at startup.
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
        # gdi32/user32 for WindowCaptureSrc.cpp; linked explicitly rather than
        # relying on Qt to pull them in. dwmapi is resolved at runtime with
        # GetProcAddress to avoid a new import.
        target_link_libraries(${target} ${_scope} advapi32 gdi32 user32)
        target_compile_definitions(${target} ${_scope} HAVE_WINCRED=1)
    endif()

    if(ENABLE_RUST_SDK_BACKEND)
        add_dependencies(${target} matrix-client-rust-build)
        target_link_libraries(${target} ${_scope} matrix-client-rust)
        if(UNIX AND NOT APPLE)
            # Rust static libraries commonly need libdl/libpthread/libm.
            target_link_libraries(${target} ${_scope} dl pthread m)
        endif()

        target_compile_definitions(${target} ${_scope} ENABLE_RUST_SDK_BACKEND=1)

        # CryptoManager::supportsE2ee() is true for the Rust backend only.
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
# library (below) needs the same configuration to propagate to the test
# executables that link it, so the visibility is a parameter.
function(lightning_configure_qml_app target)
    _lightning_configure_qml_module(${target} PRIVATE)
endfunction()

# Test executables link the one prebuilt copy of the application instead of
# recompiling all of APP_LIB_SOURCES per target. Linking the generated
# ...plugin target is what runs the static QML module's type registration.
function(lightning_configure_qml_test target)
    # qmlimportscanner runs serially at configure time for every QML-linking
    # executable and dominated configure time. It only matters for static Qt
    # plugins; with a shared Qt, tests resolve plugins at runtime, and the
    # MatrixClient module is linked explicitly via lightning-app-testlibplugin.
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
    # No find_package(SQLite3): the Rust staticlib bundles SQLite (matrix-sdk
    # `bundled-sqlite`), and linking the system library too would put two
    # SQLite implementations in one process.

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

    # Glob the crate's sources so editing any of them re-runs cargo;
    # CONFIGURE_DEPENDS picks up added or removed files.
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

# The shipped executable's sources. LIGHTNING_RUST_ONLY drops the HTTP and mock
# backends (both .h and .cpp, so AUTOMOC skips them too); the shared lists keep
# them for tests.
set(APP_MAIN_SOURCES ${APP_SOURCES})
if(LIGHTNING_RUST_ONLY)
    list(REMOVE_ITEM APP_MAIN_SOURCES
        src/matrix/MockMatrixClient.h
        src/matrix/MockMatrixClient.cpp
        src/matrix/CppHttpMatrixClient.h
        src/matrix/CppHttpMatrixClient.cpp
    )
endif()

# The binary is `lightning-matrix`; packaging renames it per platform
# (Lightning.exe, Lightning.app). The persistent settings/store identity is a
# separate literal in src/main.cpp and must not follow this name.
qt_add_executable(lightning-matrix ${APP_MAIN_SOURCES})
lightning_configure_qml_app(lightning-matrix)

# Call sounds, rendered by scripts/generate-call-sounds.py (GPL-3.0-or-later).
# Played by CallSoundPlayer from qrc:/sounds/<name>.wav. App target only, not
# the QML module: every test target re-registers that, and the WAVs are ~2.4 MB.
qt_add_resources(lightning-matrix "lightning_call_sounds"
    PREFIX "/sounds"
    BASE "data/sounds"
    FILES
        data/sounds/join.wav data/sounds/leave.wav
        data/sounds/connected.wav data/sounds/ended.wav
        data/sounds/disconnected.wav data/sounds/mute.wav
        data/sounds/unmute.wav data/sounds/deafen.wav
        data/sounds/undeafen.wav data/sounds/share-start.wav
        data/sounds/share-stop.wav data/sounds/hand-raised.wav
        data/sounds/ring.wav data/sounds/call-waiting.wav
        data/sounds/ringback.wav)
# Per-target (never on the shared mock-based test targets): drive the C++
# #ifdef guards in AppController and the --build-info self-report.
if(LIGHTNING_RUST_ONLY)
    target_compile_definitions(lightning-matrix PRIVATE LIGHTNING_RUST_ONLY=1)
endif()
# Development-only screenshot demo: drives the #ifdef guards in main.cpp /
# AppController and the --build-info self-report.
if(LIGHTNING_ENABLE_SCREENSHOT_DEMO)
    target_compile_definitions(lightning-matrix PRIVATE LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
    # Defined tree-wide: demo sources compile into every Qt-executable target,
    # so demo-only APIs must exist in all of them.
    add_compile_definitions(LIGHTNING_ENABLE_SCREENSHOT_DEMO=1)
endif()

# Windows: a GUI-subsystem binary, so launching never opens a console. main.cpp
# attaches to a parent console for --version/--help. Test binaries stay console
# so CTest captures their output.
if(WIN32)
    set_target_properties(lightning-matrix PROPERTIES WIN32_EXECUTABLE ON)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    # ------------------------------------------------------------------
    # One prebuilt copy of the application for the Qt-executable tests, so
    # the ~150 app sources compile once rather than once per test target.
    # Packaging builds use -DBUILD_TESTING=OFF.
    qt_add_library(lightning-app-testlib STATIC ${APP_LIB_SOURCES})
    _lightning_configure_qml_module(lightning-app-testlib PUBLIC)
    target_link_libraries(lightning-app-testlib PUBLIC Qt6::Test)

    # Blanked so every `${APP_LIB_SOURCES}` in the test targets below expands
    # to nothing; they link lightning-app-testlib instead.
    set(APP_LIB_SOURCES "")

    # SignPath Foundation code-signing prerequisites kept in this repository
    # (policy documents, version consistency, no committed secrets).
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

    # Exec quoting for the AppImage launcher entry; its own unit because
    # main.cpp cannot be linked into a test.
    qt_add_executable(desktop-entry-quoting-test
        tests/DesktopEntryQuotingTest.cpp
        src/app/DesktopEntryQuoting.cpp
    )
    target_include_directories(desktop-entry-quoting-test PRIVATE src)
    target_link_libraries(desktop-entry-quoting-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME desktop-entry-quoting COMMAND desktop-entry-quoting-test)

    # Messages for a refused room action. Pure string mapping; links Core only.
    qt_add_executable(room-action-error-test
        tests/RoomActionErrorTest.cpp
        src/matrix/RoomActionError.cpp
    )
    target_include_directories(room-action-error-test PRIVATE src)
    target_compile_definitions(room-action-error-test PRIVATE
        SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(room-action-error-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-action-error COMMAND room-action-error-test)

    # The remembered MSC2346 network badge per room. Pure storage; Core only.
    qt_add_executable(bridge-label-store-test
        tests/BridgeLabelStoreTest.cpp
        src/storage/BridgeLabelStore.cpp
    )
    target_include_directories(bridge-label-store-test PRIVATE src)
    target_link_libraries(bridge-label-store-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME bridge-label-store COMMAND bridge-label-store-test)

    # Windows portable mode. It is decided before the first QSettings exists,
    # without a QCoreApplication; the policy has no platform guard so it is
    # testable here, including relocating state between roots.
    qt_add_executable(portable-mode-test
        tests/PortableModeTest.cpp
        src/storage/PortableMode.cpp
        src/storage/AppDataPaths.cpp
    )
    target_include_directories(portable-mode-test PRIVATE src)
    target_link_libraries(portable-mode-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME portable-mode COMMAND portable-mode-test)

    # The portable session store, hermetic: it takes its directory as an
    # argument. SecretStore.h only; the .cpp would pull in the WinCred and
    # libsecret backends.
    qt_add_executable(portable-secret-store-test
        tests/PortableSecretStoreTest.cpp
        src/storage/PortableSecretStore.h
        src/storage/PortableSecretStore.cpp
        src/storage/SecretStore.h
        # The insecure fallback also answers lastReadFailed(), which every
        # destructive decision keys on.
        src/storage/InsecureFallbackSecretStore.h
        src/storage/InsecureFallbackSecretStore.cpp
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
        # The drag logic lives in the model, so it is testable without a view.
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
    # The catalogs are read from the source tree (the .ts files, for the
    # placeholder contract) and from the build tree (the compiled .qm, to
    # prove one actually loads). Neither is a resource here: this target
    # deliberately does not build the QML module.
    target_compile_definitions(localization-test PRIVATE
        LIGHTNING_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(LIGHTNING_TRANSLATIONS_ENABLED)
        target_compile_definitions(localization-test PRIVATE
            LIGHTNING_HAS_TRANSLATIONS=1
            LIGHTNING_QM_DIR="${LIGHTNING_QM_DIR}"
            LIGHTNING_LUPDATE_EXECUTABLE="$<TARGET_FILE:Qt6::lupdate>")
        add_dependencies(localization-test lightning-translations)
        # The catalogs are embedded here too, at the same :/i18n prefix the
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
    # published promise must move together.
    target_compile_definitions(settings-session-test PRIVATE
        REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME settings-session COMMAND settings-session-test)

    # "Send later": server (MSC4140) vs local queue policy, no persisted
    # encrypted-room plaintext, crash and sign-out handling.
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

    # Activity Center model: classification, exclusions, seen state, bounds,
    # account switch.
    qt_add_executable(activity-model-test
        tests/ActivityModelTest.cpp
        src/models/ActivityModel.h
        src/models/ActivityModel.cpp
    src/models/MediaHistoryModel.cpp
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

    # User-selectable and importable fonts. The link list is part of the
    # contract: FontManager must not depend on the Matrix client or network.
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

    # The recentEmoji property feeding the quick-react strip.
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
        src/gif/GifPreviewCache.h
        src/gif/GifPreviewCache.cpp
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

    # One activation, one send: the controller drops an identical in-flight
    # destination+GIF, but not a deliberate repeat after the first resolves.
    qt_add_executable(gif-send-controller-dedup-test
        tests/GifSendControllerDedupTest.cpp
        src/gif/GifSendController.h
        src/gif/GifSendController.cpp
        src/gif/GifTransport.h
        src/gif/MatrixGifTransport.h
        src/gif/MatrixGifTransport.cpp
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

    # NOT COVERED: history landing during an active wheel animation. The
    # offscreen harness cannot keep the anchor delegate in range across a
    # pagination round trip. restoreCapturedAnchor() applies a relative shift
    # to the live contentY either way, but that is reasoning, not evidence.

    # Threads prepend on requestOlder like the main timeline and need the same
    # anchoring so older replies do not move the view.
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

    # The account-identity <-> SDK-store binding: create and restore use the
    # same store path; adoption of a divergent-case store is unambiguous,
    # idempotent and non-destructive; a no-op reset says so; each failure has
    # its own reason code.
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

    # Local "starred chat GIF" store: content-hash dedup, caps that refuse
    # rather than evict, account isolation, unstar deletes the file, and a
    # stale index entry never surfaces an unplayable tile.
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

    # Everything that quotes the release version (README included) quotes the
    # same one.
    qt_add_executable(version-consistency-test tests/VersionConsistencyTest.cpp)
    target_compile_definitions(version-consistency-test PRIVATE
        REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(version-consistency-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME version-consistency COMMAND version-consistency-test)

    qt_add_executable(emoji-ui-contract-test tests/EmojiUiContractTest.cpp)
    target_compile_definitions(emoji-ui-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(emoji-ui-contract-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME emoji-ui-contract COMMAND emoji-ui-contract-test)

    # The MemberProfilePopover single-instance convention: one shared popover
    # per view; a per-delegate popover is a silent perf regression.
    qt_add_executable(profile-popover-contract-test
        tests/ProfilePopoverContractTest.cpp)
    target_compile_definitions(profile-popover-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(profile-popover-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME profile-popover-contract
        COMMAND profile-popover-contract-test)

    # The Settings verification card's visibility predicates.
    qt_add_executable(verification-card-contract-test
        tests/VerificationCardContractTest.cpp)
    target_compile_definitions(verification-card-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(verification-card-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME verification-card-contract
        COMMAND verification-card-contract-test)

    # Context-menu contracts: grouping, accelerators, quick-react liveness,
    # radio flyouts, signal-only mutation, permalink/leave wiring.
    qt_add_executable(context-menu-contract-test
        tests/ContextMenuContractTest.cpp)
    target_compile_definitions(context-menu-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(context-menu-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME context-menu-contract COMMAND context-menu-contract-test)

    # The GIF hover star: gating parity, HoverHandler-only hover, one
    # activation path for tap/keyboard/accessibility, mediaKey-keyed
    # resolution, live refresh from the store.
    qt_add_executable(gif-hover-star-contract-test
        tests/GifHoverStarContractTest.cpp)
    target_compile_definitions(gif-hover-star-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(gif-hover-star-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME gif-hover-star-contract COMMAND gif-hover-star-contract-test)

    # Emoji picker contract: category rail, grid metrics, footer preview,
    # choose()/recents ordering.
    qt_add_executable(emoji-picker-contract-test
        tests/EmojiPickerContractTest.cpp)
    target_compile_definitions(emoji-picker-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(emoji-picker-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME emoji-picker-contract COMMAND emoji-picker-contract-test)

    # GIF picker contract: chrome plus the exact-selection invariants.
    qt_add_executable(gif-picker-redesign-contract-test
        tests/GifPickerRedesignContractTest.cpp)
    target_compile_definitions(gif-picker-redesign-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(gif-picker-redesign-contract-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME gif-picker-redesign-contract
        COMMAND gif-picker-redesign-contract-test)

    # Non-circular sizing/visibility of the two confirmation dialogs and the
    # timeline pagination header.
    qt_add_executable(qml-binding-contract-test tests/QmlBindingContractTest.cpp)
    target_compile_definitions(qml-binding-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        # Trust labels composed in C++ (`app.sessionTrustState`) are invisible
        # to a QML-only scan.
        LIGHTNING_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(qml-binding-contract-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME qml-binding-contract COMMAND qml-binding-contract-test)

    # A text scan cannot catch a QML type that was never registered. This boots
    # a real AppController on the mock backend and loads the real
    # TimelinePane.qml through the "MatrixClient" module.
    qt_add_executable(timeline-pane-qml-test
        tests/TimelinePaneQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(timeline-pane-qml-test)
    target_link_libraries(timeline-pane-qml-test PRIVATE Qt6::Test)
    add_test(NAME timeline-pane-qml COMMAND timeline-pane-qml-test)
    set_tests_properties(timeline-pane-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The rail's drop-to-make-a-folder gesture, driven by a real pointer on the
    # real SpacesRail and asserted on the store. RailLayoutTest covers only the
    # model and cannot see an unreachable gesture.
    qt_add_executable(rail-drag-qml-test
        tests/RailDragQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(rail-drag-qml-test)
    target_link_libraries(rail-drag-qml-test PRIVATE Qt6::Test)
    add_test(NAME rail-drag-qml COMMAND rail-drag-qml-test)
    set_tests_properties(rail-drag-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # A Space's name, topic and avatar as the real Space settings dialog renders
    # them; catches bindings that never re-evaluate, which text scans cannot.
    qt_add_executable(space-identity-qml-test
        tests/SpaceIdentityQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(space-identity-qml-test)
    target_link_libraries(space-identity-qml-test PRIVATE Qt6::Test)
    add_test(NAME space-identity-qml COMMAND space-identity-qml-test)
    set_tests_properties(space-identity-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Timeline stability: hydration gate, bottom pinning under async row
    # growth, scrolled-up anchor preservation, row-scoped model updates.
    qt_add_executable(timeline-hydration-qml-test
        tests/TimelineHydrationQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(timeline-hydration-qml-test)
    target_link_libraries(timeline-hydration-qml-test PRIVATE Qt6::Test)
    add_test(NAME timeline-hydration-qml COMMAND timeline-hydration-qml-test)
    set_tests_properties(timeline-hydration-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # QML-pipeline cost of a near-top page of contiguous state-change rows
    # versus ordinary messages. See the test file header.
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

    # Media placeholders: reserved geometry for image/video/sticker, compact
    # audio rows, the decrypting skeleton, Skeleton animation eligibility.
    qt_add_executable(media-placeholder-qml-test
        tests/MediaPlaceholderQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(media-placeholder-qml-test)
    target_link_libraries(media-placeholder-qml-test PRIVATE Qt6::Test)
    add_test(NAME media-placeholder-qml COMMAND media-placeholder-qml-test)
    set_tests_properties(media-placeholder-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Which picture the image viewer opens: the Media tab walks history the
    # timeline never loaded, so the viewer uses the given list and index and
    # never substitutes a neighbour.
    qt_add_executable(media-viewer-navigation-qml-test
        tests/MediaViewerNavigationQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(media-viewer-navigation-qml-test)
    target_link_libraries(media-viewer-navigation-qml-test PRIVATE Qt6::Test)
    add_test(NAME media-viewer-navigation-qml
             COMMAND media-viewer-navigation-qml-test)
    set_tests_properties(media-viewer-navigation-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The link-preview consent gate: one compact band, smaller than what it
    # leads to, with the privacy fact stated and the button the only thing
    # that consents.
    # "Collapse media and link embeds": the covered surfaces, the deliberate
    # exclusions, the reversible disclosure, and that a collapsed attachment
    # is not instantiated (so it cannot fetch). `visible: false` would pass a
    # weaker test.
    qt_add_executable(collapsed-embeds-qml-test
        tests/CollapsedEmbedsQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(collapsed-embeds-qml-test)
    target_link_libraries(collapsed-embeds-qml-test PRIVATE Qt6::Test)
    add_test(NAME collapsed-embeds-qml COMMAND collapsed-embeds-qml-test)
    set_tests_properties(collapsed-embeds-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(link-preview-qml-test
        tests/LinkPreviewQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(link-preview-qml-test)
    target_link_libraries(link-preview-qml-test PRIVATE Qt6::Test)
    add_test(NAME link-preview-qml COMMAND link-preview-qml-test)
    set_tests_properties(link-preview-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The audio card's cover art keeps its aspect ratio at every size.
    qt_add_executable(audio-cover-art-aspect-test
        tests/AudioCoverArtAspectTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(audio-cover-art-aspect-test)
    target_link_libraries(audio-cover-art-aspect-test PRIVATE Qt6::Test)
    add_test(NAME audio-cover-art-aspect COMMAND audio-cover-art-aspect-test)
    set_tests_properties(audio-cover-art-aspect PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Avatar pipeline: explicit loading/ready/missing/failed states through the
    # real MediaBridge + MediaImageProvider, no fill beneath a decoded avatar,
    # stable fallback colours, clean URL-change resets.
    qt_add_executable(avatar-pipeline-qml-test
        tests/AvatarPipelineQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(avatar-pipeline-qml-test)
    target_link_libraries(avatar-pipeline-qml-test PRIVATE Qt6::Test)
    add_test(NAME avatar-pipeline-qml COMMAND avatar-pipeline-qml-test)
    set_tests_properties(avatar-pipeline-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Clicking a favourite GIF chooses exactly that item, never an index-zero
    # substitute.
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

    # The Saved tab's provider half and empty state, a real drag on the resize
    # grip, and the nav row's list tabs as geometry at two widths.
    qt_add_executable(gif-saved-tab-qml-test
        tests/GifSavedTabQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(gif-saved-tab-qml-test)
    target_link_libraries(gif-saved-tab-qml-test PRIVATE Qt6::Test)
    add_test(NAME gif-saved-tab-qml COMMAND gif-saved-tab-qml-test)
    set_tests_properties(gif-saved-tab-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Startup lifecycle: Boot/RestoringSession, no login UI for valid sessions,
    # the no-account and failed-restore login states.
    qt_add_executable(startup-session-test
        tests/StartupSessionTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(startup-session-test)
    target_link_libraries(startup-session-test PRIVATE Qt6::Test)
    add_test(NAME startup-session COMMAND startup-session-test)
    set_tests_properties(startup-session PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Account-switch lifecycle on a real AppController (mock backend).
    qt_add_executable(account-switch-test
        tests/AccountSwitchTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(account-switch-test)
    target_link_libraries(account-switch-test PRIVATE Qt6::Test)
    add_test(NAME account-switch COMMAND account-switch-test)
    set_tests_properties(account-switch PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Setting the account's own display name on a real AppController: stale
    # generations and foreign op ids never update the cached identity; the
    # code-point limit, clear vs empty refusal, and the registry re-fetch a
    # confirmed write triggers (sync does not feed that cache).
    qt_add_executable(own-profile-test
        tests/OwnProfileTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(own-profile-test)
    target_link_libraries(own-profile-test PRIVATE Qt6::Test)
    add_test(NAME own-profile COMMAND own-profile-test)
    set_tests_properties(own-profile PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # AppController::isChatGifStarred/unstarChatGif's two tiers (session fast
    # path + durable content-hash fallback) against a controllable FakeClient.
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

    # Where a window opens: centred on one screen, not the whole virtual
    # desktop.
    qt_add_executable(window-placement-test
        tests/WindowPlacementTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(window-placement-test)
    target_link_libraries(window-placement-test PRIVATE Qt6::Test)
    add_test(NAME window-placement COMMAND window-placement-test)
    set_tests_properties(window-placement PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The media browser's model: what it shows and what it claims about how
    # much history it has seen.
    qt_add_executable(media-history-model-test
        tests/MediaHistoryModelTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(media-history-model-test)
    target_link_libraries(media-history-model-test PRIVATE Qt6::Test)
    add_test(NAME media-history-model COMMAND media-history-model-test)
    set_tests_properties(media-history-model PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Retiring a Rust client must not block the GUI thread. Needs the real Rust
    # backend (dropping a tokio runtime is the expensive part).
    if(ENABLE_RUST_SDK_BACKEND)
        qt_add_executable(rust-retirement-test
            tests/RustRetirementTest.cpp
            ${APP_LIB_SOURCES}
        )
        lightning_configure_qml_test(rust-retirement-test)
        target_link_libraries(rust-retirement-test PRIVATE Qt6::Test)
        add_test(NAME rust-retirement COMMAND rust-retirement-test)
        set_tests_properties(rust-retirement PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    # Live-timeline diff translation and generation tracking. Pure Qt, so it
    # runs in both build trees.
    qt_add_executable(rust-timeline-ingest-test
        tests/RustTimelineIngestTest.cpp
        src/matrix/RustTimelineIngest.cpp
        # Reply previews go through the shared preview normalisation.
        src/matrix/EventPreview.cpp
    )
    target_include_directories(rust-timeline-ingest-test PRIVATE src)
    target_link_libraries(rust-timeline-ingest-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME rust-timeline-ingest COMMAND rust-timeline-ingest-test)

    # TimelineModel diff application (insert/replace/remove/truncate/reset,
    # local-echo reconciliation, invalid-index self-heal).
    qt_add_executable(timeline-model-diff-test
        tests/TimelineModelDiffTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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

    # State-activity grouping passes through virtual rows (date dividers, read
    # markers, timeline start) and breaks on any visible message.
    qt_add_executable(state-activity-grouping-test
        tests/StateActivityGroupingTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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

    # Member-profile change sentences, built translatably from typed old/new
    # values and the actor's resolved name. Names are untrusted plain text:
    # markup is never interpreted and a missing value never renders as "".
    qt_add_executable(profile-activity-test
        tests/ProfileActivityTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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

    # One hover toolbar per row, anchored in place (rows do not clip); only one
    # row shows it at a time.
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

    # Runtime counterpart to scroll-consistency-contract, which cannot see a
    # null scrollTarget (that makes a pane unscrollable).
    qt_add_executable(smooth-wheel-area-runtime-test
        tests/SmoothWheelAreaRuntimeTest.cpp)
    lightning_configure_qml_test(smooth-wheel-area-runtime-test)
    target_link_libraries(smooth-wheel-area-runtime-test PRIVATE Qt6::Test)
    add_test(NAME smooth-wheel-area-runtime
             COMMAND smooth-wheel-area-runtime-test)
    set_tests_properties(smooth-wheel-area-runtime PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # CodeBlock.qml: a long line becomes horizontal scroll range, never
    # geometry; the gutter is never copied; a vertical wheel notch still
    # scrolls the timeline.
    # The Channels layout's rows, measured in real geometry (source scans
    # cannot catch a 0px row).
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

    # Measurement, not a fix: the cost of state-activity grouping over a long
    # run of state-change rows versus ordinary messages. See the test header.
    qt_add_executable(timeline-state-flood-perf-test
        tests/TimelineStateFloodPerfTest.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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

    # CacheStore refuses encrypted-room rows on every write path; decrypted
    # plaintext stays in memory.
    qt_add_executable(cache-store-security-test
        tests/CacheStoreSecurityTest.cpp
        src/storage/CacheStore.cpp
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp
    )
    target_include_directories(cache-store-security-test PRIVATE src)
    target_link_libraries(cache-store-security-test PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME cache-store-security COMMAND cache-store-security-test)

    # Semantic theme tokens, WCAG contrast for critical pairs, no stray hex
    # colours in core view QML.
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

    # Attachment pipeline: validation, MIME from content, queue state machine,
    # retry, stale-completion rejection, lifecycle clears.
    qt_add_executable(attachment-queue-test
        tests/AttachmentQueueTest.cpp
        src/models/MessageComposer.h
        src/models/MessageComposer.cpp
        # The composer's draft hooks pull in the store.
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
        # The queue owns send-side video poster extraction; the suite installs
        # the poster hook so no QMediaPlayer is constructed.
        src/media/VideoPosterExtractor.h
        src/media/VideoPosterExtractor.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(attachment-queue-test PRIVATE src)
    target_link_libraries(attachment-queue-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME attachment-queue COMMAND attachment-queue-test)

    # Poster extraction runs on its own thread: the first QVideoSink in a
    # process costs ~1 s of Qt Multimedia initialisation. This suite does
    # construct a real QMediaPlayer.
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

    # Playable-file materialisation runs off the GUI thread: the call returns
    # before the bytes are on disk and the caller keeps running meanwhile.
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

    # A notification's initials disc matches the room list's (palette, hash,
    # initials rule).
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

    # User search and DM/room creation/invite flows: debounce, stale results,
    # existing-DM reuse, duplicate submission, partial invites, sign-out.
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

    # Kick/ban through RoomInfoController: permission snapshot, SDK-flag
    # gating, one in flight, sanitized results, stale results on room switch.
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

    # Room administration: power levels, join rule and canonical alias.
    # Matrix rules enforced before dispatch, custom levels preserved, no
    # optimistic writes, stale answers rejected.
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

    # PinnedMessagesController: snapshot ingestion, the complete-id-list behind
    # isPinned(), the applicable action only, no optimistic apply, remote
    # re-read, clearing on room switch and sign-out.
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

    # MSC2545 stickers and custom emoji: StickerPackManager snapshot ingestion,
    # selection rules, canSave (plain mxc only), send destination and thread
    # rule, bounded emoticon lookup, sign-out isolation.
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

    # Animated sticker tiles: a `required property` the model does not serve
    # builds zero delegates silently, and the picker matches MediaBridge cache
    # keys as strings. Derived from source, so new properties are covered.
    qt_add_executable(sticker-picker-tile-contract-test
        tests/StickerPickerTileContractTest.cpp
        src/stickers/StickerPack.h
        src/stickers/StickerPack.cpp
        src/stickers/StickerPackModel.h
        src/stickers/StickerPackModel.cpp
        src/stickers/StickerImageModel.h
        src/stickers/StickerImageModel.cpp
    )
    target_include_directories(sticker-picker-tile-contract-test PRIVATE src)
    target_compile_definitions(sticker-picker-tile-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        LIGHTNING_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(sticker-picker-tile-contract-test PRIVATE
        Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME sticker-picker-tile-contract
        COMMAND sticker-picker-tile-contract-test)

    # Room upgrades: unknown successors stay Unknown, joined ones navigate
    # without joining, invited ones join first, a failed join leaves the user
    # in place with the reason, the successor must point back, and superseded
    # rooms are demoted. Observing a tombstone never moves or joins anything.
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

    # Space settings: name, topic and avatar go through the permission-gated
    # room-edit backend, each gated on its own state event level. Also drives
    # RoomInfoController against a fake client to prove what is dispatched.
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

    # "Mark as read" for a room that is not open: the capable-backend route,
    # the timeline-walk fallback, local-echo skipping, unknown targets.
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

    # Export a room. The renderers are pure (events in, string out). The key
    # case is the encrypted one: a decrypted body reaches a file only when the
    # caller passes the flag the UI sets after explicit confirmation (§6).
    qt_add_executable(room-export-test
        tests/RoomExportTest.cpp
        src/models/RoomExport.h
        src/models/RoomExport.cpp
    )
    target_include_directories(room-export-test PRIVATE src)
    target_link_libraries(room-export-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-export COMMAND room-export-test)

    # Widgets open in the user's browser, so the boundary under test is that
    # no QML path hands the desktop a URL outside the model's validated list.
    # The URL rules themselves are tested in Rust.
    qt_add_executable(widget-controller-test
        tests/WidgetControllerTest.cpp
        src/models/WidgetController.h
        src/models/WidgetController.cpp
        src/app/UrlLauncher.h
        src/app/UrlLauncher.cpp
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
    target_include_directories(widget-controller-test PRIVATE src)
    target_link_libraries(widget-controller-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME widget-controller COMMAND widget-controller-test)

    # User-chosen name colours: caching and dedup (so colorFor() is safe in a
    # per-name binding) and validation of a profile value used as a colour.
    qt_add_executable(name-color-manager-test
        tests/NameColorManagerTest.cpp
        src/profile/NameColorManager.h
        src/profile/NameColorManager.cpp
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
    target_include_directories(name-color-manager-test PRIVATE src)
    target_link_libraries(name-color-manager-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME name-color-manager COMMAND name-color-manager-test)

    # Global-profile lookup for mention pills and the profile popover: one
    # request per user per session, cached answers, remembered refusals.
    qt_add_executable(user-profile-resolver-test
        tests/UserProfileResolverTest.cpp
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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
    target_include_directories(user-profile-resolver-test PRIVATE src)
    target_link_libraries(user-profile-resolver-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    add_test(NAME user-profile-resolver COMMAND user-profile-resolver-test)

    # ForwardController: refuses redacted/local-echo/undecryptable sources,
    # forwards text with no relation (a thread reply lands as an ordinary
    # message), re-fetches and re-uploads media rather than copying the mxc,
    # keeps the dialog open on failure, handles the stale-fetch race.
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

    # Thread-participant fetch bound: concurrency cap, FIFO queue, slot release
    # on success and failure, dedup, room-switch invalidation.
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

    # Discover / Join: directory search model and the discovery controller.
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

    # Server-side message search: debounce, room names, paging, stale
    # superseding, scope changes, sign-out.
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

    # UIA + device sign-out: challenge round trip, wrong-password retry,
    # cancel, current-device guard, OAuth console URL. Credentials never
    # persist.
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

    # Personal moderation: ignore/unignore, remote list pushes, reporting, and
    # the notification ignore guard.
    qt_add_executable(moderation-test
        tests/ModerationTest.cpp
        src/app/ModerationController.h
        src/app/ModerationController.cpp
        src/notifications/NotificationManager.h
        src/notifications/NotificationManager.cpp
        src/app/TrayIcon.h
        src/app/TrayIcon.cpp
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
    target_compile_definitions(moderation-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(moderation-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Widgets Qt6::Test)
    if(TARGET Qt6::DBus)
        target_link_libraries(moderation-test PRIVATE Qt6::DBus)
        target_compile_definitions(moderation-test PRIVATE HAVE_QT_DBUS=1)
    endif()
    add_test(NAME moderation COMMAND moderation-test)

    # Drafts: scoping, encrypted-room memory-only policy, debounce staleness,
    # retirement on send/clear, account isolation.
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
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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
        # Header only: AUTOMOC supplies the SecretStore metaobject; the .cpp
        # would pull in the real backends.
        src/storage/SecretStore.h
    )
    target_include_directories(draft-store-test PRIVATE src)
    target_link_libraries(draft-store-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Qml Qt6::Test)
    add_test(NAME draft-store COMMAND draft-store-test)

    # Opt-in GUI stall heartbeat (LIGHTNING_GUI_STALL_TRACE).
    qt_add_executable(gui-stall-trace-test
        tests/GuiStallTraceTest.cpp
        src/app/GuiStallTracer.h
        src/app/GuiStallTracer.cpp
    )
    target_include_directories(gui-stall-trace-test PRIVATE src)
    target_link_libraries(gui-stall-trace-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME gui-stall-trace COMMAND gui-stall-trace-test)

    # QML contracts: copy image, reply thumbnails, receipt popover, space
    # invites and nested subspaces, rail indent, initial avatars.
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

    # The Space Home "Rooms and spaces" list, the rail's inline expansion, the
    # reader popover's timestamps, and the non-exclusive TapHandler guards.
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

    # SpaceManager::setSpaceChildSuggested: pending ops, foreign-op rejection,
    # refused sends, logout isolation.
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

    # The sliding window in ReverseListProxyModel: per-frame cost scales with
    # instantiated rows, so the proxy must release rows. Each transition is one
    # insert/remove at one end, the mapping stays invertible, and a live
    # message does not move a reader deep in history.
    qt_add_executable(reverse-list-proxy-window-test
        tests/ReverseListProxyWindowTest.cpp
        src/models/ReverseListProxyModel.h
        src/models/ReverseListProxyModel.cpp
        # The proxy attributes row construction for stall tracing.
        src/app/GuiStallTracer.h
        src/app/GuiStallTracer.cpp
    )
    target_include_directories(reverse-list-proxy-window-test PRIVATE src)
    target_link_libraries(reverse-list-proxy-window-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME reverse-list-proxy-window
             COMMAND reverse-list-proxy-window-test)

    # MSC2746 call signalling: ringing, glare, busy auto-reject, cross-device
    # settlement, invite lifetime, LRU absorption, placeCall refusal, logout.
    qt_add_executable(call-controller-test
        tests/CallControllerTest.cpp
        src/calls/CallController.h
        src/calls/CallController.cpp
        src/matrix/RtcSession.h
        src/calls/RtcController.h
        src/calls/RtcController.cpp
        src/calls/SfuCallController.h
        src/calls/SfuCallController.cpp
        # SfuCallController.h calls wincap::available() inline, so every target
        # including it needs this file; off Windows it is a few stubs.
        src/calls/WindowCaptureSrc.h
        src/calls/WindowCaptureSrc.cpp
        src/calls/CallParticipantModel.h
        src/calls/CallParticipantModel.cpp
        src/calls/CallShareModel.h
        src/calls/CallShareModel.cpp
        src/calls/CallStageState.h
        src/calls/CallStageState.cpp
        src/calls/CameraPortal.h
        src/calls/CameraPortal.cpp
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
        # Header only: AUTOMOC supplies the SecretStore metaobject; the .cpp
        # would pull in the real backends.
        src/storage/SecretStore.h
    )
    target_include_directories(call-controller-test PRIVATE src)
    target_link_libraries(call-controller-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
    # The X11 share fallback reads QPlatformScreen; see
    # _lightning_configure_qml_module.
    if(UNIX AND NOT APPLE AND TARGET Qt6::GuiPrivate)
        target_link_libraries(call-controller-test PRIVATE Qt6::GuiPrivate)
        target_compile_definitions(call-controller-test PRIVATE
            LIGHTNING_HAVE_QPA_SCREEN=1)
    endif()
    # Reads the controller's source: deriving a capture rectangle from
    # devicePixelRatio() is the defect and is not observable through the API.
    target_compile_definitions(call-controller-test PRIVATE
        "SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    add_test(NAME call-controller COMMAND call-controller-test)

    # Ring policy on a full AppController: ignore/mute/backlog gates,
    # ringForCalls, missed classification, notification-decline routing.
    qt_add_executable(call-ring-policy-test
        tests/CallRingPolicyTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(call-ring-policy-test)
    target_link_libraries(call-ring-policy-test PRIVATE Qt6::Test)
    add_test(NAME call-ring-policy COMMAND call-ring-policy-test)

    # Call sounds, policy only: which cue plays when (no audio).
    qt_add_executable(call-sound-policy-test
        tests/CallSoundPolicyTest.cpp
        src/calls/CallSoundPolicy.h
        src/calls/CallSoundPolicy.cpp
    )
    target_include_directories(call-sound-policy-test PRIVATE src)
    target_link_libraries(call-sound-policy-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME call-sound-policy COMMAND call-sound-policy-test)

    # Space moderation: the per-room plan, the cascade and its result reporting.
    qt_add_executable(space-moderation-controller-test
        tests/SpaceModerationControllerTest.cpp
        src/spaces/SpaceModerationController.h
        src/spaces/SpaceModerationController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(space-moderation-controller-test PRIVATE src)
    target_link_libraries(space-moderation-controller-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME space-moderation-controller COMMAND space-moderation-controller-test)

    # Closing a room or Space, and the server-admin delete: the plan gate, the
    # cascade order, the typed confirmation and per-room outcomes.
    qt_add_executable(room-closure-controller-test
        tests/RoomClosureControllerTest.cpp
        src/spaces/RoomClosureController.h
        src/spaces/RoomClosureController.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(room-closure-controller-test PRIVATE src)
    target_link_libraries(room-closure-controller-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME room-closure-controller COMMAND room-closure-controller-test)

    # Call sounds wired to AppController / SfuCallController / CallController
    # through a recording sink; never opens an audio device.
    qt_add_executable(call-sounds-wiring-test
        tests/CallSoundsWiringTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(call-sounds-wiring-test)
    target_link_libraries(call-sounds-wiring-test PRIVATE Qt6::Test)
    add_test(NAME call-sounds-wiring COMMAND call-sounds-wiring-test)
    set_tests_properties(call-sounds-wiring PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(call-ring-policy PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Not a test: renders the in-call header bar in a real window and saves a
    # PNG for visual checks.
    qt_add_executable(call-bar-shot
        tests/CallBarShot.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(call-bar-shot)
    target_link_libraries(call-bar-shot PRIVATE Qt6::Test)

    # LiveKit-compatible frame encryption, checked against livekit-client's
    # wire format (header, trailer, IV construction), plus: no key means no
    # output, tampered frames fail, wrong key indices are dropped.
    qt_add_executable(call-frame-cryptor-test
        tests/CallFrameCryptorTest.cpp
        src/calls/CallFrameCryptor.cpp
    )
    target_include_directories(call-frame-cryptor-test PRIVATE src)
    target_link_libraries(call-frame-cryptor-test
        PRIVATE Qt6::Core Qt6::Test OpenSSL::Crypto)
    add_test(NAME call-frame-cryptor COMMAND call-frame-cryptor-test)

    # Routing received video to QML VideoOutputs: a dead tile must stop
    # receiving frames and no participant's video may land in another's tile.
    qt_add_executable(sfu-video-router-test
        tests/SfuVideoRouterTest.cpp
        src/calls/SfuVideoRouter.cpp
    )
    target_include_directories(sfu-video-router-test PRIVATE src)
    target_link_libraries(sfu-video-router-test
        PRIVATE Qt6::Core Qt6::Multimedia Qt6::Test)
    add_test(NAME sfu-video-router COMMAND sfu-video-router-test)

    # SpaceChannelModel's flat, global list of Space folders: the layout must
    # not change at Home, and a subspace's rooms must not be listed twice (the
    # SDK child accessor is transitive).
    qt_add_executable(space-channels-test
        tests/SpaceChannelsTest.cpp
        src/models/SpaceChannelModel.h
        src/models/SpaceChannelModel.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        # Rooms come from the client, not RoomListModel, which is scoped to the
        # active Space and filtered.
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

    # Host/presenter separation, as a source scan because it is about which
    # file contains what: presenters never reach the host by id, one presenter
    # is instantiated, Channels needs a Space, and new theme tokens are derived.
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

    # MatrixRTC session store: op-id/room matching, generation isolation, poke
    # coalescing (including during an in-flight read), slot-closed
    # suppression, one face per person, distinct join-block reasons.
    qt_add_executable(rtc-session-test
        tests/RtcSessionTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(rtc-session-test)
    target_link_libraries(rtc-session-test PRIVATE Qt6::Test)
    add_test(NAME rtc-session COMMAND rtc-session-test)
    set_tests_properties(rtc-session PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # m.location (MSC3488) and live beacons (MSC3672).
    qt_add_executable(location-message-test
        tests/LocationMessageTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(location-message-test)
    target_link_libraries(location-message-test PRIVATE Qt6::Test)
    add_test(NAME location-message COMMAND location-message-test)
    set_tests_properties(location-message PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Policy lists: op discipline, and following a list does not enforce it.
    qt_add_executable(policy-list-controller-test
        tests/PolicyListControllerTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(policy-list-controller-test)
    target_link_libraries(policy-list-controller-test PRIVATE Qt6::Test)
    add_test(NAME policy-list-controller COMMAND policy-list-controller-test)
    set_tests_properties(policy-list-controller PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # MSC4108: the sign-in-another-device state machine.
    qt_add_executable(qr-login-controller-test
        tests/QrLoginControllerTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(qr-login-controller-test)
    target_link_libraries(qr-login-controller-test PRIVATE Qt6::Test)
    add_test(NAME qr-login-controller COMMAND qr-login-controller-test)
    set_tests_properties(qr-login-controller PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Load gate: load-time QML errors are invisible to qmlformat, qmlcachegen
    # and source-scanning suites. See QmlComponentLoadTest.cpp.
    qt_add_executable(qml-component-load-test
        tests/QmlComponentLoadTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(qml-component-load-test)
    target_link_libraries(qml-component-load-test PRIVATE Qt6::Test)
    target_compile_definitions(qml-component-load-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    add_test(NAME qml-component-load COMMAND qml-component-load-test)
    set_tests_properties(qml-component-load PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Incoming-call card contracts plus a real offscreen instantiation against
    # a ringing CallController.
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

    # A real WebRTC handshake: two GstCallMediaBackends in one process
    # (test-tone mode) complete ICE + DTLS-SRTP over loopback.
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

        # The bundled-plugin rule, needing no runtime. Kept out of
        # sfu-media-engine-test, whose whole-suite QSKIP reports as a pass.
        qt_add_executable(gst-bootstrap-test
            tests/GstBootstrapTest.cpp
            src/calls/GstBootstrap.h
            src/calls/GstBootstrap.cpp
        )
        target_include_directories(gst-bootstrap-test PRIVATE src)
        target_link_libraries(gst-bootstrap-test PRIVATE
            Qt6::Core Qt6::Test PkgConfig::GSTWEBRTC)
        add_test(NAME gst-bootstrap COMMAND gst-bootstrap-test)

        # SfuMediaEngine in test-source mode: pipeline construction, the crypto
        # pad probes and teardown, not the network.
        qt_add_executable(sfu-media-engine-test
            tests/SfuMediaEngineTest.cpp
            src/calls/GstBootstrap.h
            src/calls/GstBootstrap.cpp
            src/calls/SfuMediaEngine.h
            src/calls/SfuMediaEngine.cpp
            src/calls/CaptureDeviceSelection.h
            src/calls/CaptureDeviceSelection.cpp
            src/calls/RtpVp8Payloader.h
            src/calls/RtpVp8Payloader.cpp
            src/calls/WindowCaptureSrc.h
            src/calls/WindowCaptureSrc.cpp
            src/calls/CallFrameCryptor.h
            src/calls/CallFrameCryptor.cpp
            src/calls/SfuVideoRouter.h
            src/calls/SfuVideoRouter.cpp
            src/calls/ShareAudioSources.h
            src/calls/ShareAudioSources.cpp
        )
        target_include_directories(sfu-media-engine-test PRIVATE src)
        target_compile_definitions(sfu-media-engine-test PRIVATE
            HAVE_LIGHTNING_WEBRTC
            "LIGHTNING_SFU_ENGINE_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/calls/SfuMediaEngine.cpp\""
            # The gst_init ordering check reads the sources: ordering cannot be
            # observed after GStreamer is initialised.
            "SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
        target_link_libraries(sfu-media-engine-test PRIVATE
            Qt6::Core Qt6::Multimedia Qt6::Test
            PkgConfig::GSTWEBRTC OpenSSL::Crypto)
        # A real call against a real homeserver, headless. Needs the Rust
        # backend and skips unless LIGHTNING_LIVE_* names a server, account and
        # room; registered so it keeps compiling.
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

    # SDK thread foundation: lifecycle, root/reply identity via the composite
    # timeline id, failure states, thread-only send path, encrypted threads.
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
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
        src/models/UserLookup.h
        src/models/UserLookup.cpp
        src/models/AttachmentQueueModel.h
        src/models/AttachmentQueueModel.cpp
        src/media/StagedImageStore.h
        src/media/StagedImageStore.cpp
        # The poster hook keeps this suite free of a real media backend.
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
        # ThreadController's draft hooks pull in the store.
        src/app/DraftStore.h
        src/app/DraftStore.cpp
    )
    target_include_directories(thread-controller-test PRIVATE src)
    target_link_libraries(thread-controller-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Qml Qt6::Test)
    add_test(NAME thread-controller COMMAND thread-controller-test)

    # Read-only E2EE health model: readiness, tri-state trust, backup/recovery
    # distinctions, generation isolation, no secrets.
    qt_add_executable(crypto-health-model-test
        tests/CryptoHealthModelTest.cpp
        src/crypto/CryptoHealthModel.h
        src/crypto/CryptoHealthModel.cpp
    )
    target_include_directories(crypto-health-model-test PRIVATE src)
    target_link_libraries(crypto-health-model-test PRIVATE
        Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME crypto-health-model COMMAND crypto-health-model-test)

    # "This device can never decrypt" policy: the tri-state latch (Unknown is
    # neither fault nor clearance) and the /keys/query re-check rate limit.
    qt_add_executable(own-device-key-watch-test
        tests/OwnDeviceKeyWatchTest.cpp
        src/crypto/OwnDeviceKeyWatch.h
        src/crypto/OwnDeviceKeyWatch.cpp
    )
    target_include_directories(own-device-key-watch-test PRIVATE src)
    target_link_libraries(own-device-key-watch-test PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME own-device-key-watch COMMAND own-device-key-watch-test)

    # Session bootstrap status: phase derivation from sanitized SDK state, key
    # counts, idempotency, per-session reset.
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

    # The row's right rail is shared: geometric cases over the real
    # MessageDelegate check that the hover bar ends before the receipt
    # facepile on a short row and keeps the corner on a tall one.
    qt_add_executable(message-rail-collision-test
        tests/MessageRailCollisionTest.cpp
    )
    lightning_configure_qml_test(message-rail-collision-test)
    target_include_directories(message-rail-collision-test PRIVATE src)
    add_test(NAME message-rail-collision COMMAND message-rail-collision-test)
    set_tests_properties(message-rail-collision PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # A restore that cannot reach the homeserver must read as offline, even
    # though startSync() and the sync lane report Syncing on the way. Drives
    # the real event dispatcher; no FFI handle, store or network.
    if(ENABLE_RUST_SDK_BACKEND)
        qt_add_executable(offline-restore-state-test
            tests/OfflineRestoreStateTest.cpp
        )
        lightning_configure_qml_test(offline-restore-state-test)
        target_include_directories(offline-restore-state-test PRIVATE src)
        add_test(NAME offline-restore-state COMMAND offline-restore-state-test)
        set_tests_properties(offline-restore-state PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Fields the Rust lane computes must survive the bridge; a fake client
        # emits signals itself and cannot see a dropped field. Drives the real
        # dispatcher with the real payload.
        qt_add_executable(rtc-bridge-payload-test
            tests/RtcBridgePayloadTest.cpp
        )
        lightning_configure_qml_test(rtc-bridge-payload-test)
        target_include_directories(rtc-bridge-payload-test PRIVATE src)
        add_test(NAME rtc-bridge-payload COMMAND rtc-bridge-payload-test)
        set_tests_properties(rtc-bridge-payload PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Media messages from live sync must produce notifications and Activity
        # rows with the right msgtype. Drives the real dispatcher.
        qt_add_executable(sync-message-row-test
            tests/SyncMessageRowTest.cpp
        )
        lightning_configure_qml_test(sync-message-row-test)
        target_include_directories(sync-message-row-test PRIVATE src)
        add_test(NAME sync-message-row COMMAND sync-message-row-test)
        set_tests_properties(sync-message-row PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # A call must never downgrade an encrypted room to cleartext on a stale
        # room-list read. Pure unit test.
        qt_add_executable(call-encryption-state-test
            tests/CallEncryptionStateTest.cpp
        )
        lightning_configure_qml_test(call-encryption-state-test)
        target_include_directories(call-encryption-state-test PRIVATE src)
        add_test(NAME call-encryption-state COMMAND call-encryption-state-test)
        set_tests_properties(call-encryption-state PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    # A device whose published identity key is not its own can never decrypt.
    # Covers the durable state, the tri-state that keeps "unknown" from reading
    # as "broken", the corner card and its sign-out repair, and the timeline
    # row that stops promising an endless wait.
    qt_add_executable(encryption-identity-fault-test
        tests/EncryptionIdentityFaultTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(encryption-identity-fault-test)
    target_link_libraries(encryption-identity-fault-test PRIVATE Qt6::Test)
    add_test(NAME encryption-identity-fault
             COMMAND encryption-identity-fault-test)
    set_tests_properties(encryption-identity-fault PROPERTIES
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

    # The show-QR panel in the real SettingsScreen card: shown only while the
    # SDK has a live code, opaque provider URL, confirm gated on the SDK's scan
    # report, retired on dismissal and completion. Zero QML warnings.
    qt_add_executable(verification-qr-qml-test
        tests/VerificationQrQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(verification-qr-qml-test)
    target_link_libraries(verification-qr-qml-test PRIVATE Qt6::Test)
    add_test(NAME verification-qr-qml COMMAND verification-qr-qml-test)
    set_tests_properties(verification-qr-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Server-synchronised per-room notification modes: the AppController entry
    # point, account-scoped storage with legacy-key fallback, and
    # reconciliation from user-defined backend reports that never overrides an
    # unsynced local choice. Rust-backend slots QSKIP without the Rust backend.
    qt_add_executable(room-notification-mode-test
        tests/RoomNotificationModeTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(room-notification-mode-test)
    target_link_libraries(room-notification-mode-test PRIVATE Qt6::Test)
    add_test(NAME room-notification-mode COMMAND room-notification-mode-test)

    # Ownership of the shared VoiceRecorder: a live recording is refused, not
    # stolen, so the microphone is never orphaned.
    qt_add_executable(voice-ownership-test
        tests/VoiceOwnershipTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(voice-ownership-test)
    target_link_libraries(voice-ownership-test PRIVATE Qt6::Test)
    add_test(NAME voice-ownership COMMAND voice-ownership-test)
    set_tests_properties(room-notification-mode PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # One-line event summaries for the room list and notifications: poll
    # fallbacks, mention markdown, newline collapse, bounded length.
    qt_add_executable(event-preview-test
        tests/EventPreviewTest.cpp
        src/matrix/EventPreview.h
        src/matrix/EventPreview.cpp
    )
    target_include_directories(event-preview-test PRIVATE src)
    target_link_libraries(event-preview-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME event-preview COMMAND event-preview-test)

    # Notification decision policy: privacy modes, mention metadata, room
    # modes, active-room suppression, never exposing ciphertext.
    qt_add_executable(notification-manager-test
        tests/NotificationManagerTest.cpp
        src/notifications/NotificationManager.h
        src/notifications/NotificationManager.cpp
        src/app/TrayIcon.h
        src/app/TrayIcon.cpp
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
    # TrayIcon (the balloon delivery for builds without QtDBus) is a
    # QSystemTrayIcon, which lives in QtWidgets.
    target_link_libraries(notification-manager-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    if(TARGET Qt6::DBus)
        target_link_libraries(notification-manager-test PRIVATE Qt6::DBus)
        target_compile_definitions(notification-manager-test
            PRIVATE HAVE_QT_DBUS=1)
    endif()
    add_test(NAME notification-manager COMMAND notification-manager-test)
    # Point the session bus at a dead address so the tray-fallback cases behave
    # the same on a desktop with a notification daemon as in CI. QDBusConnection
    # caches per thread, so this must be the environment.
    set_tests_properties(notification-manager PROPERTIES
        ENVIRONMENT "DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/lightning-no-daemon")

    # Link previews: URL extraction with a scheme allow-list, GIF MIME, encrypted
    # room gating, bounded cache, stale rejection, settings defaults.
    qt_add_executable(link-preview-test
        tests/LinkPreviewTest.cpp
        src/models/LinkPreview.h
        src/models/LinkPreview.cpp
        src/models/LinkPreviewController.h
        src/models/LinkPreviewController.cpp
        # LinkPreviewController::linkifiedBody() is also the timeline body
        # renderer for plain messages and sizes emoji through
        # MessageHtml::markEmoji like sanitize() does. Qt6::Core only.
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

    # Avatar/media requests: dedup, per-size keys, bounded LRU, sign-out stale
    # rejection, failure marking with explicit retry.
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
    # Gui/Multimedia for MediaBridge's lazily constructed poster extractor;
    # Quick for MediaImageProvider (requestImage is called directly, no window).
    target_link_libraries(media-bridge-test
        PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Quick Qt6::Test)
    add_test(NAME media-bridge COMMAND media-bridge-test)

    # Waveform bucket math: max-preserving downsample, normalisation, bounds.
    qt_add_executable(voice-recorder-test
        tests/VoiceRecorderTest.cpp
        src/media/VoiceRecorder.h
        src/media/VoiceRecorder.cpp
        # mediaScratchRoot(): the portable folder when portable, else the OS
        # temp directory.
        src/storage/PortableMode.cpp
    )
    target_include_directories(voice-recorder-test PRIVATE src)
    target_link_libraries(voice-recorder-test
        PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Test)
    add_test(NAME voice-recorder COMMAND voice-recorder-test)

    # Inline playback coordinator: one audible owner, owner-checked release,
    # stopAll generation for room/account switches.
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

    # Bare-localpart user lookup: candidates from the account's server, profile
    # confirmation before exposure, dedup with provenance, stale rejection.
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

    # Backward pagination: single flight, viewport-fill budget, no-progress
    # stop, retry, stale room/reset/sign-out isolation.
    qt_add_executable(pagination-controller-test
        tests/PaginationControllerTest.cpp
        src/models/PaginationController.h
        src/models/PaginationController.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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

    # Wheel-scroll math: per-notch distance, coalescing, partial deltas,
    # reversal, pixel vs angle, clamping, cancellation.
    qt_add_executable(timeline-scroll-controller-test
        tests/TimelineScrollControllerTest.cpp
        src/models/TimelineScrollController.h
        src/models/TimelineScrollController.cpp
    )
    target_include_directories(timeline-scroll-controller-test PRIVATE src)
    target_link_libraries(timeline-scroll-controller-test PRIVATE
        Qt6::Core Qt6::Qml Qt6::Test)
    add_test(NAME timeline-scroll-controller COMMAND timeline-scroll-controller-test)

    # Automatic read receipts: visibility, debounce, eligibility, duplicate
    # suppression, ordering.
    qt_add_executable(read-receipt-coordinator-test
        tests/ReadReceiptCoordinatorTest.cpp
        src/models/ReadReceiptCoordinator.h
        src/models/ReadReceiptCoordinator.cpp
        src/models/TimelineModel.h
        src/models/TimelineModel.cpp
        src/profile/UserProfileResolver.h
        src/profile/UserProfileResolver.cpp
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

    # matrix.to room permalinks: alias preferred, id fallback, same encoding as
    # messagePermalink.
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

    # Which applications a share's audio carries, never ourselves. A pure
    # function over PipeWire properties, testable without PipeWire or a call.
    qt_add_executable(share-audio-sources-test
        tests/ShareAudioSourcesTest.cpp
        src/calls/ShareAudioSources.h
        src/calls/ShareAudioSources.cpp
    )
    target_include_directories(share-audio-sources-test PRIVATE src)
    target_link_libraries(share-audio-sources-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME share-audio-sources COMMAND share-audio-sources-test)

    # Bridged-network recognition from ghost user ids and portal aliases; the
    # negatives matter most (a false badge is worse than none). Also covers
    # the MSC2346 answer, which needs the room list because only it can badge
    # a bridged group.
    qt_add_executable(bridge-network-test
        tests/BridgeNetworkTest.cpp
        src/matrix/BridgeNetwork.h
        src/matrix/BridgeNetwork.cpp
        src/models/RoomListModel.h
        src/models/RoomListModel.cpp
        src/models/DirectAvatarResolver.h
        src/models/DirectAvatarResolver.cpp
        src/spaces/SpaceManager.h
        src/spaces/SpaceManager.cpp
        src/matrix/MatrixClient.h
        src/matrix/MatrixClient.cpp
    )
    target_include_directories(bridge-network-test PRIVATE src)
    target_link_libraries(bridge-network-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME bridge-network COMMAND bridge-network-test)

    # Quick switcher model: matching, categories, bounded results, routing
    # payload, logout clearing.
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

    # PresenceManager policy: watch bookkeeping, batches, unknown vs offline,
    # the disabled-server latch, session clearing, publication gating.
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

    # Profile banners (MSC4427 over MSC4133), policy half: "no banner", "not
    # asked yet" and "server has no extended profiles" are distinct states.
    qt_add_executable(profile-banner-test
        tests/ProfileBannerTest.cpp
        src/profile/ProfileBannerManager.h
        src/profile/ProfileBannerManager.cpp
        src/profile/NameColorManager.h
        src/profile/NameColorManager.cpp
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

    # Profile bios (MSC4440 over MSC4133) and the badge table. As with banners,
    # three empty states stay distinct; only "no extended profiles" stops the
    # client asking.
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

    # E2EE diagnostics redaction: never full ids, stable for correlation.
    qt_add_executable(e2ee-diagnostics-test
        tests/E2eeDiagnosticsTest.cpp
        src/crypto/E2eeDiagnostics.h
        src/crypto/E2eeDiagnostics.cpp
    )
    target_include_directories(e2ee-diagnostics-test PRIVATE src)
    target_link_libraries(e2ee-diagnostics-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME e2ee-diagnostics COMMAND e2ee-diagnostics-test)

    # Chrome icon hygiene: no emoji as chrome, every Icon name mapped, subset
    # font shipped.
    qt_add_executable(icon-chrome-test
        tests/IconChromeTest.cpp
    )
    target_compile_definitions(icon-chrome-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(icon-chrome-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME icon-chrome COMMAND icon-chrome-test)

    # The crop/adjust step for display-image uploads (`imagecrop::`): the
    # rectangle, the size cap, bytes matching their named format, and SVG
    # refused by magic before decoding.
    qt_add_executable(image-crop-test
        tests/ImageCropTest.cpp
        src/media/ImageCropper.h
        src/media/ImageCropper.cpp
        src/storage/PortableMode.h
        src/storage/PortableMode.cpp
        src/media/StagedImageStore.h
        src/media/StagedImageStore.cpp
    )
    target_include_directories(image-crop-test PRIVATE src)
    target_link_libraries(image-crop-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME image-crop COMMAND image-crop-test)

    # Crop wiring: one crop dialog, every display-image upload routed through
    # it, and chat attachments, Save-As and the local app icon left alone.
    qt_add_executable(image-crop-contract-test
        tests/ImageCropDialogContractTest.cpp
    )
    target_compile_definitions(image-crop-contract-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(image-crop-contract-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME image-crop-contract COMMAND image-crop-contract-test)

    # Desktop integration: launcher entry, hicolor icons, install rules, Qt
    # identity wiring.
    qt_add_executable(desktop-integration-test
        tests/DesktopIntegrationTest.cpp
    )
    target_compile_definitions(desktop-integration-test PRIVATE
        SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(desktop-integration-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME desktop-integration COMMAND desktop-integration-test)

    # Offscreen visual verification: renders each theme on representative
    # controls (including an open ComboBox popup), saves PNG snapshots and
    # checks sampled pixels against the theme's tokens.
    qt_add_executable(theme-snapshot-test
        tests/ThemeSnapshotTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(theme-snapshot-test)
    target_link_libraries(theme-snapshot-test PRIVATE Qt6::Test)
    add_test(NAME theme-snapshot COMMAND theme-snapshot-test)
    set_tests_properties(theme-snapshot PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIGHTNING_SNAPSHOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/snapshots")

    # Whole-shell acceptance: renders the real MainScreen per theme and size,
    # saves PNGs, and asserts geometry and token-sampled pixels.
    qt_add_executable(design-acceptance-test
        tests/DesignAcceptanceTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(design-acceptance-test)
    target_link_libraries(design-acceptance-test PRIVATE Qt6::Test)
    add_test(NAME design-acceptance COMMAND design-acceptance-test)
    set_tests_properties(design-acceptance PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIGHTNING_SNAPSHOT_DIR=${CMAKE_CURRENT_BINARY_DIR}/snapshots")

    # In-shell Settings: rail and room list stay, the timeline region swaps,
    # theme cards, instant theme switching, layout/text-size controls, close
    # restores chat.
    qt_add_executable(settings-shell-qml-test
        tests/SettingsShellQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(settings-shell-qml-test)
    target_link_libraries(settings-shell-qml-test PRIVATE Qt6::Test)
    # The source qml directory, for cases that compare two files which must
    # agree; the engine only sees the compiled module.
    target_compile_definitions(settings-shell-qml-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    add_test(NAME settings-shell-qml COMMAND settings-shell-qml-test)
    set_tests_properties(settings-shell-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Login-screen session repair: the failure card offers a repair that needs
    # no typing, targets the account that failed, varies actions by reason,
    # confirms destructive steps by account name, and never resets from raw
    # form text.
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

    # Main composer: one card, toolbar over input, control order and geometry,
    # markdown round trip, theme-tracked pixels, zero QML warnings.
    qt_add_executable(composer-qml-test
        tests/ComposerQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(composer-qml-test)
    target_link_libraries(composer-qml-test PRIVATE Qt6::Test)
    add_test(NAME composer-qml COMMAND composer-qml-test)
    set_tests_properties(composer-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Home buttons fire, reactions wrap, redacted messages cannot be pinned,
    # Alt+V opens skin tones, room links do not flash a modal, voice recording
    # pause/done/preview, middle-click autoscroll, the audio card.
    qt_add_executable(tester-report-fixes-test
        tests/TesterReportFixesTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(tester-report-fixes-test)
    target_compile_definitions(tester-report-fixes-test PRIVATE
        QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        # Some cases cover C++ lifecycle rather than QML.
        LIGHTNING_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(tester-report-fixes-test PRIVATE Qt6::Test)
    add_test(NAME tester-report-fixes COMMAND tester-report-fixes-test)
    set_tests_properties(tester-report-fixes PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # At the minimum window size the composer keeps a readable width and still
    # grows with a multi-line draft.
    qt_add_executable(narrow-shell-layout-test
        tests/NarrowShellLayoutTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(narrow-shell-layout-test)
    target_link_libraries(narrow-shell-layout-test PRIVATE Qt6::Test)
    add_test(NAME narrow-shell-layout COMMAND narrow-shell-layout-test)
    set_tests_properties(narrow-shell-layout PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # New Conversation / Create Room / Create Space dialog: per-tab
    # instantiation (no encryption control on the Space tab), search clearing
    # and focus on tab switch, name validation, busy/error states, existing-DM
    # reuse, keyboard selection.
    qt_add_executable(creation-dialog-qml-test
        tests/CreationDialogQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(creation-dialog-qml-test)
    target_link_libraries(creation-dialog-qml-test PRIVATE Qt6::Test)
    add_test(NAME creation-dialog-qml COMMAND creation-dialog-qml-test)
    set_tests_properties(creation-dialog-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The DISPLAY / WAYLAND_DISPLAY preflight fires only on Unix-like
    # non-macOS platforms. Header-only predicate.
    qt_add_executable(startup-checks-test
        tests/StartupChecksTest.cpp
        src/app/StartupChecks.h
    )
    target_include_directories(startup-checks-test PRIVATE src)
    target_link_libraries(startup-checks-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME startup-checks COMMAND startup-checks-test)

    # Compiled default backend and --backend=NAME parsing. Uses only
    # AppController.h's inline helpers; no app sources are linked.
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

    # Runs the real binary: --build-info reports screenshot_demo_compiled, and
    # with the option off --screenshot-demo is rejected in preflight.
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

    # --log-file / --console, asserted against the real binary: main.cpp cannot
    # be linked into a test and a source scan cannot see argument handling.
    qt_add_executable(diagnostic-output-test
        tests/DiagnosticOutputTest.cpp
    )
    target_link_libraries(diagnostic-output-test PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(diagnostic-output-test PRIVATE
        "LIGHTNING_BINARY=\"$<TARGET_FILE:lightning-matrix>\"")
    add_dependencies(diagnostic-output-test lightning-matrix)
    add_test(NAME diagnostic-output COMMAND diagnostic-output-test)

    # Development-only demo dataset: structure, determinism (stable ids, fixed
    # clock), and only fictional *.example identities.
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

    # Development-only demo multi-account contract: three fictional accounts,
    # per-account dataset isolation, deterministic reset, an in-memory
    # SecretStore. Drives a real AppController through the real account
    # switch. Built only with the demo option.
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

        # Development-only demo media: every media row and avatar resolves to
        # bundled fixture bytes with the right MIME and no network.
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

        # Development-only demo interactions mutate the in-memory scene through
        # the real backend interface; reset restores it; no-ops outside demo.
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

        # Development-only demo scenarios: each activates the right
        # account/room/page/thread/theme/window size; presets resolve; reset
        # restores defaults.
        qt_add_executable(screenshot-demo-scenario-test
            tests/ScreenshotDemoScenarioTest.cpp
            ${APP_LIB_SOURCES}
        )
        lightning_configure_qml_test(screenshot-demo-scenario-test)
        target_compile_definitions(screenshot-demo-scenario-test PRIVATE
            LIGHTNING_ENABLE_SCREENSHOT_DEMO=1
            # Reads the controller's source: the layout a scenario selects, and
            # the order, is not observable from its catalogue.
            "SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
        target_link_libraries(screenshot-demo-scenario-test PRIVATE Qt6::Test)
        add_test(NAME screenshot-demo-scenario
            COMMAND screenshot-demo-scenario-test)
        set_tests_properties(screenshot-demo-scenario PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Development-only demo control panel in an offscreen window: loads
        # without warnings or layout gaps; presets resize the window.
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

    # Slash commands: registry, parse rules (// escape, letters-only command
    # words), completion. Pure.
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

    # Rich composer QTextDocument -> Matrix serializer: HTML per formatting
    # type, the plain fallback, the whitelist, link-target safety.
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

    # @-mention tokenizer: trigger boundaries, code rejection, query capture
    # and cap, ref shifting, matrix.to expansion.
    qt_add_executable(mention-tokenizer-test
        tests/MentionTokenizerTest.cpp
        src/models/MentionTokenizer.h
        src/models/MentionTokenizer.cpp
    )
    target_include_directories(mention-tokenizer-test PRIVATE src)
    target_link_libraries(mention-tokenizer-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME mention-tokenizer COMMAND mention-tokenizer-test)

    # The environment a spawned browser inherits: inside an AppImage it must
    # not point at the bundle, or the browser loads our glib and never starts.
    qt_add_executable(url-launcher-test
        tests/UrlLauncherTest.cpp
        src/app/UrlLauncher.h
        src/app/UrlLauncher.cpp
    )
    target_include_directories(url-launcher-test PRIVATE src)
    target_link_libraries(url-launcher-test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME url-launcher COMMAND url-launcher-test)

    # The composer's emoji face, asserted on the document's applied format:
    # Qt's fallback differs by version, so emoji faces are applied per range by
    # the highlighter, and only the applied format shows which property the
    # layout honours. Needs Qt6::Gui for QTextDocument.
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

    # Legacy Matrix SSO (m.login.sso): the loopback callback listener over real
    # sockets plus AuthManager's discovery/login policy: tokens, duplicate and
    # stale callbacks, malformed/oversized requests, timeout, cancellation,
    # provider scoping, OAuth parsing unaffected.
    qt_add_executable(sso-callback-test
        tests/SsoCallbackTest.cpp
    )
    target_include_directories(sso-callback-test PRIVATE src)
    target_link_libraries(sso-callback-test
        PRIVATE lightning-app-testlib Qt6::Test)
    add_test(NAME sso-callback COMMAND sso-callback-test)

    # Slash commands, composer half: which backend verb runs with which
    # arguments, and a refusal never destroys the draft.
    qt_add_executable(slash-command-composer-test
        tests/SlashCommandComposerTest.cpp
    )
    target_include_directories(slash-command-composer-test PRIVATE src)
    target_link_libraries(slash-command-composer-test
        PRIVATE lightning-app-testlib Qt6::Test)
    add_test(NAME slash-command-composer COMMAND slash-command-composer-test)

    # Opt-in sync-latency tracer: inert when off, no room ids, unobserved
    # stages not reported as instant, bounded table, gap reporting.
    qt_add_executable(sync-latency-tracer-test
        tests/SyncLatencyTracerTest.cpp
        src/app/SyncLatencyTracer.h
        src/app/SyncLatencyTracer.cpp
    )
    target_include_directories(sync-latency-tracer-test PRIVATE src)
    target_link_libraries(sync-latency-tracer-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME sync-latency-tracer COMMAND sync-latency-tracer-test)

    # @-mention member suggestions: request on activation, filter/rank/dedup,
    # own user excluded, stale op ids, room-switch/logout clearing.
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

    # Inline emoji sizing (MessageHtml::markEmoji): big-emoji suppression and
    # code-span exclusion. Behavioural.
    qt_add_executable(message-emoji-size-test
        tests/MessageEmojiSizeTest.cpp
        src/models/MessageHtml.h
        src/models/MessageHtml.cpp
    )
    target_include_directories(message-emoji-size-test PRIVATE src)
    target_link_libraries(message-emoji-size-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME message-emoji-size COMMAND message-emoji-size-test)

    # Rendered proof that the shared controls are flat, token-driven and
    # theme-tracking.
    qt_add_executable(control-system-test
        tests/ControlSystemTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(control-system-test)
    target_link_libraries(control-system-test PRIVATE Qt6::Test)
    add_test(NAME control-system COMMAND control-system-test)
    set_tests_properties(control-system PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Button styles, offscreen pixel checks: A bare with hover tint, B the
    # accent-soft chip, C the accent fill, keyboard focus ring.
    qt_add_executable(button-system-test
        tests/ButtonSystemTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(button-system-test)
    target_link_libraries(button-system-test PRIVATE Qt6::Test)
    add_test(NAME button-system COMMAND button-system-test)
    set_tests_properties(button-system PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # AppMenu/AppMenuItem metrics and ink, keycaps, section labels, status
    # chips, danger rows, cascading flyouts closing innermost first on Escape.
    qt_add_executable(menu-system-qml-test
        tests/MenuSystemQmlTest.cpp
        ${APP_LIB_SOURCES}
    )
    lightning_configure_qml_test(menu-system-qml-test)
    target_link_libraries(menu-system-qml-test PRIVATE Qt6::Test)
    add_test(NAME menu-system-qml COMMAND menu-system-qml-test)
    set_tests_properties(menu-system-qml PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Mention popup: role chips, query header, selected-row keycap,
    # accessibility, and source scans (needs QML_DIR).
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

    # Account switcher: identity cards, real data only, live-id switch guard,
    # confirmations.
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

    # Member profile: omitted affordances, DM reuse, role chips, clipboard.
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

    # Trust card: brand-fixed rendering, step states, no QR affordance, no
    # app.* coupling.
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

    # Invite dialog: chips, already-member pre-check, room header, per-user
    # result states.
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

    # Quick switcher: mode transitions, command actions, live theme execution,
    # Escape.
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
    # Update system. Each suite links only the sources it exercises, never the
    # application.
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
        src/updater/ArtifactDigest.cpp
        # The helper's own argument parser, fed exactly what UpdateManager
        # builds.
        src/updater/UpdaterArgs.cpp
        # Downloads stage under the portable-aware cache root.
        src/storage/AppDataPaths.cpp
        src/storage/PortableMode.cpp)
    target_include_directories(update-manager-state-test PRIVATE src)
    # Give the scan an explicit path so it hard-fails rather than silently
    # finding nothing.
    target_compile_definitions(update-manager-state-test PRIVATE
        LIGHTNING_UPDATE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/update")

    # The suites compile src/update/*.cpp themselves; pass the configured
    # values so they test the build's settings, not the header defaults.
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
        src/updater/UpdaterArgs.cpp src/updater/InstallStrategies.cpp
        src/updater/ArtifactDigest.cpp)
    target_include_directories(updater-helper-args-test PRIVATE src)
    target_link_libraries(updater-helper-args-test PRIVATE Qt6::Core Qt6::Test)
    # Also runs the real helper binary end to end (a digest mismatch refuses
    # before touching anything; a match installs).
    target_compile_definitions(updater-helper-args-test PRIVATE
        LIGHTNING_UPDATER_HELPER_PATH="$<TARGET_FILE:lightning-updater>")
    add_dependencies(updater-helper-args-test lightning-updater)
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

    # The room-list registry: the index space the SDK's diffs address. Pure
    # QJson in, QHash/QStringList out; no FFI.
    qt_add_executable(rust-room-registry-test
        tests/RustRoomRegistryTest.cpp
        src/matrix/RustRoomRegistry.cpp
        src/matrix/EventPreview.cpp)
    target_include_directories(rust-room-registry-test PRIVATE src)
    target_link_libraries(rust-room-registry-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME rust-room-registry COMMAND rust-room-registry-test)

    qt_add_executable(capture-device-selection-test
        tests/CaptureDeviceSelectionTest.cpp src/calls/CaptureDeviceSelection.cpp)
    target_include_directories(capture-device-selection-test PRIVATE src)
    target_link_libraries(capture-device-selection-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME capture-device-selection COMMAND capture-device-selection-test)

    qt_add_executable(updater-relaunch-environment-test
        tests/RelaunchEnvironmentTest.cpp src/updater/RelaunchEnvironment.cpp)
    target_include_directories(updater-relaunch-environment-test PRIVATE src)
    target_link_libraries(updater-relaunch-environment-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME updater-relaunch-environment COMMAND updater-relaunch-environment-test)

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

    # Conflict refusal, reserved sequences and global-vs-editor shadowing for
    # rebindable shortcuts.
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

    # When a rail folder exists: an empty folder is deleted, decided from the
    # store rather than the rendered rail. Also pins that a reorder does not
    # empty the collapsed folders it passes.
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

    # The People list scoped to the selected Space: the Space roster, the
    # Classic DM scope and the Channels People group, whose failure directions
    # are opposite.
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

    # Unread badge attribution: a tile's badge counts exactly what that tile's
    # view lists (the synthesised DM tile included). Every case asserts both
    # halves, so a merely non-zero badge cannot pass.
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
# The update helper: a tiny separate binary for the one step that cannot run
# while Lightning is running. Its link line is its isolation guarantee:
# Qt6::Core only, nothing from src/matrix, src/app or src/storage, so it cannot
# reach an account, token or store. It only receives already-verified
# artifacts.
qt_add_executable(lightning-updater
    src/updater/main.cpp
    src/updater/UpdaterArgs.h        src/updater/UpdaterArgs.cpp
    src/updater/ArtifactDigest.h     src/updater/ArtifactDigest.cpp
    src/updater/ProcessWaiter.h      src/updater/ProcessWaiter.cpp
    src/updater/RelaunchEnvironment.h src/updater/RelaunchEnvironment.cpp
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
if(LIGHTNING_INSTALL_TYPE STREQUAL "linux-flatpak")
    set(_lightning_appid "org.lightning_matrix.Lightning")

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/lightning.desktop" _lightning_desktop)
    string(REPLACE "Icon=lightning" "Icon=${_lightning_appid}"
        _lightning_desktop "${_lightning_desktop}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/flatpak/${_lightning_appid}.desktop"
        "${_lightning_desktop}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/flatpak/${_lightning_appid}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

    foreach(_icon_size 16 32 48 64 128 192 256 512)
        install(FILES
            "data/icons/hicolor/${_icon_size}x${_icon_size}/apps/lightning.png"
            DESTINATION
            "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_icon_size}x${_icon_size}/apps"
            RENAME "${_lightning_appid}.png")
    endforeach()
    install(FILES data/icons/lightning.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME "${_lightning_appid}.svg")

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/packaging-ci/packaging/common/lightning.metainfo.xml"
        _lightning_metainfo)
    string(REPLACE ">lightning.desktop<" ">${_lightning_appid}.desktop<"
        _lightning_metainfo "${_lightning_metainfo}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/flatpak/${_lightning_appid}.metainfo.xml"
        "${_lightning_metainfo}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/flatpak/${_lightning_appid}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/${_lightning_appid}")
else()
    install(FILES data/lightning.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(_icon_size 16 32 48 64 128 192 256 512)
        install(FILES
            "data/icons/hicolor/${_icon_size}x${_icon_size}/apps/lightning.png"
            DESTINATION
            "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_icon_size}x${_icon_size}/apps")
    endforeach()
    # The scalable icon, preferred by launchers that support SVG.
    install(FILES data/icons/lightning.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME lightning.svg)
endif()

# Dismissing a link preview: per-(room,event) keying, dismissal checked before
# the auto-load dispatch, undo that grants no consent, bounded released set.
if(BUILD_TESTING)
    qt_add_executable(link-preview-dismiss-test
        tests/LinkPreviewDismissTest.cpp
        src/models/LinkPreview.h
        src/models/LinkPreview.cpp
        src/models/LinkPreviewController.h
        src/models/LinkPreviewController.cpp
        # linkifiedBody() pulls in MessageHtml (Qt6::Core only).
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
# Composer spell checking.
#
# Appended rather than added to APP_SOURCES, which is consumed and blanked
# above. Both the application and the shared test library (when tests are
# enabled) get the sources.
# ---------------------------------------------------------------------------
set(LIGHTNING_SPELL_SOURCES
    src/text/SpellBackend.h
    src/text/SpellBackend.cpp
    src/text/SpellChecker.h
    src/text/SpellChecker.cpp
)
# macOS: NSSpellChecker lives in an Objective-C++ unit. OBJCXX is enabled
# here, and every consumer of the spell sources links AppKit.
if(APPLE)
    enable_language(OBJCXX)
    list(APPEND LIGHTNING_SPELL_SOURCES src/text/SpellBackendMac.mm)
endif()
target_sources(lightning-matrix PRIVATE ${LIGHTNING_SPELL_SOURCES})
if(APPLE)
    target_link_libraries(lightning-matrix PRIVATE "-framework AppKit")
endif()
if(TARGET lightning-app-testlib)
    target_sources(lightning-app-testlib PRIVATE ${LIGHTNING_SPELL_SOURCES})
    if(APPLE)
        target_link_libraries(lightning-app-testlib PUBLIC "-framework AppKit")
    endif()
endif()

# Spell checking and the tray badge. Standalone rather than linking
# lightning-app-testlib: everything under test is pure, and the link list is
# part of the contract (SpellChecker sees only Qt and its backend interface).
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
    if(APPLE)
        target_sources(composer-spell-test PRIVATE src/text/SpellBackendMac.mm)
        target_link_libraries(composer-spell-test PRIVATE "-framework AppKit")
    endif()
    add_test(NAME composer-spell COMMAND composer-spell-test)
endif()

# The spell underline against the production composer, offscreen.
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

# CameraPortal against a fake xdg-desktop-portal on a private session bus: a
# grant sent together with the portal's reply must not be lost. Needs QtDBus
# and `dbus-daemon`; skips without the daemon.
if(BUILD_TESTING AND TARGET Qt6::DBus AND NOT WIN32 AND NOT APPLE)
    qt_add_executable(camera-portal-test
        tests/CameraPortalTest.cpp
        src/calls/CameraPortal.h
        src/calls/CameraPortal.cpp
        src/calls/PortalRequest.h
    )
    target_include_directories(camera-portal-test PRIVATE src)
    target_link_libraries(camera-portal-test PRIVATE
        Qt6::Core Qt6::DBus Qt6::Test)
    target_compile_definitions(camera-portal-test PRIVATE HAVE_QT_DBUS=1)
    add_test(NAME camera-portal COMMAND camera-portal-test)
endif()
