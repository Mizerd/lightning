FROM fedora@sha256:6c75d5bf57cb0fa5aa4b92c6a83c86c791644496d9ac230de7711f5b8ec3b898

ARG RUST_TOOLCHAIN=1.95.0
ARG QT_MULTIMEDIA_VERSION=6.11.2
# ENV, not ARG: a --build-arg must not be able to replace the digest of a
# download that is executed.
ENV QT_MULTIMEDIA_SHA256=967b5e02ec6b793cdb360622cd6e703132836af983208d678dae4b50f109cd9f
ARG FFMPEG_VERSION=7.1.1
# ENV, not ARG (see above).
ENV FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

LABEL org.opencontainers.image.title="Lightning Windows cross-package builder" \
      org.opencontainers.image.description="Fedora 44 Linux/MinGW builder for unsigned Lightning Windows artifacts (Qt Multimedia FFmpeg backend)" \
      org.opencontainers.image.source="https://gitlab.smetonis.net/Mizerd/lightning-deploy" \
      net.smetonis.lightning.builder-kind="linux-cross" \
      net.smetonis.lightning.target="x86_64-pc-windows-gnu" \
      net.smetonis.lightning.qt="6.11.2" \
      net.smetonis.lightning.ffmpeg="7.1.1" \
      net.smetonis.lightning.rust="${RUST_TOOLCHAIN}"

# The base image is content-addressed and the toolchain packages are pinned to
# Fedora NEVRAs; transitive RPMs are recorded in
# /usr/local/share/lightning-windows-rpms.txt for every CI artifact.
#
# Fedora withdraws superseded NEVRAs, which makes this file unbuildable without
# any change here (build-host tools and the Qt stack have both needed bumps).
# Check with `dnf repoquery` inside the base image, and validate the probe
# against a package known to exist and one known not to before trusting it.
# Release numbers are not uniform (qtmultimedia is -2, the rest -1), so copy
# them from `dnf repoquery` rather than editing the version string.
RUN dnf install -y --setopt=install_weak_deps=False \
        bash \
        ca-certificates \
        cmake-4.3.0-1.fc44 \
        coreutils \
        curl \
        diffutils \
        file \
        findutils \
        gcc-16.2.1-2.fc44 \
        git \
        grep \
        gzip \
        ImageMagick-7.1.2.27-1.fc44 \
        jq-1.8.1-3.fc44 \
        make \
        mingw64-binutils-2.46.0-1.fc44 \
        mingw64-gcc-c++-16.1.1-1.fc44 \
        mingw32-nsis-3.11-3.fc44 \
        mingw64-nsis-3.11-3.fc44 \
        mingw64-openssl-3.2.4-4.fc44 \
        mingw64-qt6-qtbase-6.11.2-1.fc44 \
        mingw64-qt6-qtdeclarative-6.11.2-1.fc44 \
        mingw64-qt6-qtimageformats-6.11.2-1.fc44 \
        mingw64-qt6-qtmultimedia-6.11.2-2.fc44 \
        mingw64-qt6-qtsvg-6.11.2-1.fc44 \
        mingw64-qt6-qttools-6.11.2-1.fc44 \
        mingw64-qt6-qttranslations-6.11.2-1.fc44 \
        mingw64-sqlite-3.51.3-1.fc44 \
        mingw64-zlib-1.3.2-1.fc44 \
        msitools-0.106.58-1.fc44 \
        nasm-3.02-1.fc44 \
        ninja-build-1.13.2-2.fc44 \
        python3-3.14.7-1.fc44 \
        python3-pyyaml-6.0.3-3.fc44 \
        rustup-1.29.0-7.fc44 \
        sed \
        tar \
        unzip \
        which-2.25-1.fc44 \
        wine-common-11.0-3.fc44 \
        wine-core-11.0-3.fc44.x86_64 \
        wine-desktop-11.0-3.fc44 \
        xorg-x11-server-Xvfb-21.1.24-1.fc44 \
        xz \
        zip-3.0-45.fc44 \
    && dnf clean all \
    && rm -rf /var/cache/dnf

ENV RUSTUP_HOME=/opt/rust/rustup \
    CARGO_HOME=/opt/rust/cargo \
    PATH=/usr/local/libexec/lightning-windows:/opt/rust/cargo/bin:/usr/x86_64-w64-mingw32/bin:/usr/bin:/bin \
    CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
    CC_x86_64_pc_windows_gnu=x86_64-w64-mingw32-gcc \
    CXX_x86_64_pc_windows_gnu=x86_64-w64-mingw32-g++ \
    AR_x86_64_pc_windows_gnu=x86_64-w64-mingw32-ar \
    CARGO_BUILD_TARGET=x86_64-pc-windows-gnu \
    WINEARCH=win64 \
    WINEDEBUG=-all \
    QT_QPA_PLATFORM=offscreen

RUN rustup-init -y --no-modify-path \
        --profile minimal \
        --default-toolchain "${RUST_TOOLCHAIN}" \
        --target x86_64-pc-windows-gnu \
    && rustc --version \
    && cargo --version

COPY packaging/windows/cargo-windows-wrapper \
    /usr/local/libexec/lightning-windows/cargo
COPY packaging/windows/toolchain-mingw64.cmake \
    /usr/local/share/lightning-windows/toolchain-mingw64.cmake

# Cross-build FFmpeg (LGPLv3, shared, native decoders only) for Qt Multimedia's
# FFmpeg backend. Fedora's MinGW Qt ships only the WMF backend, which stalls
# video after the first buffered frames. --disable-iconv drops a subtitle path
# Lightning does not use and which fails under GCC 16 + MinGW. /usr/bin comes
# first on PATH so FFmpeg's host helpers use the native assembler.
#
# Every download has a stall guard: `--speed-limit`/`--speed-time` abort a
# transfer below 10 kB/s for a minute, `--retry` covers flaky mirrors, and the
# sha256 check stays the integrity gate. The GStreamer installer is ~960 MB, so
# a slow download looks like a hang; compare the file size twice, some seconds
# apart, to tell them apart:
#     sudo stat -c %s /proc/<curl-pid>/root/tmp/gstreamer-mingw.exe
RUN set -eu; \
    url="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"; \
    curl --fail --show-error --silent --location --proto '=https' --tlsv1.2 \
        --connect-timeout 30 --speed-limit 10000 --speed-time 60 \
        --retry 3 --retry-delay 5 --retry-connrefused \
        "$url" --output /tmp/ffmpeg.tar.xz; \
    printf '%s  %s\n' "$FFMPEG_SHA256" /tmp/ffmpeg.tar.xz | sha256sum -c -; \
    mkdir -p /tmp/ffmpeg-src; \
    tar -xJf /tmp/ffmpeg.tar.xz --strip-components=1 -C /tmp/ffmpeg-src; \
    cd /tmp/ffmpeg-src; \
    export PATH=/usr/bin:/bin:$PATH; \
    ./configure \
        --prefix=/usr/x86_64-w64-mingw32/sys-root/mingw \
        --arch=x86_64 --target-os=mingw32 \
        --cross-prefix=x86_64-w64-mingw32- \
        --pkg-config=x86_64-w64-mingw32-pkg-config \
        --enable-cross-compile --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-debug \
        --disable-gpl --disable-nonfree --enable-version3 --disable-iconv \
        --extra-cflags="-Wno-incompatible-pointer-types -Wno-implicit-function-declaration -Wno-int-conversion"; \
    make -j4; \
    make install; \
    mkdir -p /usr/share/licenses/lightning-ffmpeg; \
    cp -a COPYING.LGPLv2.1 COPYING.LGPLv3 LICENSE.md \
        /usr/share/licenses/lightning-ffmpeg/ 2>/dev/null || true; \
    rm -rf /tmp/ffmpeg.tar.xz /tmp/ffmpeg-src; \
    file /usr/x86_64-w64-mingw32/sys-root/mingw/bin/avcodec-*.dll \
        | grep -F 'PE32+ executable'

# Fedora's MinGW multimedia RPM omits the QtMultimedia QML module and the
# FFmpeg backend plugin, so both are built from the matching Qt source. The WMF
# plugin is not built (its sources include <D3d11.h>, which is case-sensitive
# here); the RPM's copy is the fallback. A case-only symlink lets the FFmpeg
# plugin's PCH find <D3d11.h>. Host ShaderTools supplies qsb.
RUN set -eu; \
    dnf install -y --setopt=install_weak_deps=False \
        qt6-qtshadertools-devel-6.11.2-1.fc44.x86_64; \
    dnf clean all; \
    rm -rf /var/cache/dnf; \
    ln -sf d3d11.h /usr/x86_64-w64-mingw32/sys-root/mingw/include/D3d11.h; \
    source_url="https://download.qt.io/official_releases/qt/6.11/${QT_MULTIMEDIA_VERSION}/submodules/qtmultimedia-everywhere-src-${QT_MULTIMEDIA_VERSION}.tar.xz"; \
    curl --fail --show-error --silent --location --proto '=https' --tlsv1.2 \
        --connect-timeout 30 --speed-limit 10000 --speed-time 60 \
        --retry 3 --retry-delay 5 --retry-connrefused \
        "$source_url" --output /tmp/qtmultimedia.tar.xz; \
    printf '%s  %s\n' "$QT_MULTIMEDIA_SHA256" /tmp/qtmultimedia.tar.xz \
        | sha256sum -c -; \
    mkdir -p /tmp/qtmultimedia-src /tmp/qtmultimedia-build; \
    tar -xJf /tmp/qtmultimedia.tar.xz --strip-components=1 \
        -C /tmp/qtmultimedia-src; \
    cmake -S /tmp/qtmultimedia-src -B /tmp/qtmultimedia-build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/usr/local/share/lightning-windows/toolchain-mingw64.cmake \
        -DQt6ShaderTools_DIR=/usr/lib64/cmake/Qt6ShaderTools \
        -DCMAKE_BUILD_TYPE=Release \
        -DQT_BUILD_EXAMPLES=OFF \
        -DQT_BUILD_TESTS=OFF; \
    grep -Eq '^ +FFmpeg .+ yes$' /tmp/qtmultimedia-build/config.summary; \
    cmake --build /tmp/qtmultimedia-build --parallel 4 --target quickmultimedia; \
    ninja -C /tmp/qtmultimedia-build \
        lib/qt6/plugins/multimedia/ffmpegmediaplugin.dll; \
    install -m 0755 /tmp/qtmultimedia-build/bin/Qt6MultimediaQuick.dll \
        /usr/x86_64-w64-mingw32/sys-root/mingw/bin/Qt6MultimediaQuick.dll; \
    mkdir -p /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/plugins/multimedia; \
    install -m 0755 \
        /tmp/qtmultimedia-build/lib/qt6/plugins/multimedia/ffmpegmediaplugin.dll \
        /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/plugins/multimedia/ffmpegmediaplugin.dll; \
    mkdir -p /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/qml/QtMultimedia \
        /usr/share/licenses/lightning-qtmultimedia-qml-source; \
    cp -a /tmp/qtmultimedia-build/lib/qt6/qml/QtMultimedia/. \
        /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/qml/QtMultimedia/; \
    cp -a /tmp/qtmultimedia-src/LICENSES/. \
        /usr/share/licenses/lightning-qtmultimedia-qml-source/; \
    rm -rf /tmp/qtmultimedia.tar.xz /tmp/qtmultimedia-src /tmp/qtmultimedia-build

# Declared here rather than at the top: anything added above the FFmpeg and Qt
# Multimedia builds invalidates their layers, and those may not be rebuildable
# once Fedora withdraws pinned NEVRAs (docs/windows-runner-operations.md).
ARG GSTREAMER_VERSION=1.28.5
# ENV, not ARG (see above).
ENV GSTREAMER_SHA256=43feae04c3ab6f7a14de580f79b97daa0c391623e6fbdfc940d61df07d749768

COPY packaging/windows/gst-element-probe.c \
    /usr/local/share/lightning-windows/gst-element-probe.c

# GStreamer for the call media engine. CMake enables the engine only when
# pkg-config finds gstreamer-{1.0,webrtc,sdp,app,video,rtp}-1.0; otherwise the
# Windows binary refuses every call.
#
# The official MinGW SDK, not Fedora's mingw64 packages, which lack webrtcbin,
# nice, srtp, opus, vpx and webrtcdsp. The installer is Inno Setup 6.7 data that
# innoextract 1.10 and 7-Zip cannot read, so it runs in its own silent mode
# under Wine (already present for the smoke test).
#
# Only a subset (~26 MB of 2.4 GB) is installed: the plugins below, the runtime
# DLLs their import closure needs, headers, .pc files and import libraries.
#
# A collision with a Fedora sysroot DLL is a hard error. The SDK is a UCRT
# build and Fedora's MinGW is msvcrt; mingw-w64's wchar.h makes mbstate_t a
# struct under _UCRT and an int otherwise, so libstdc++ symbols mangle
# differently and a process gets one libstdc++-6.dll. The d3d11 and
# mediafoundation plugins import such symbols and are deliberately not shipped
# (Wine loads them anyway, so the element probe cannot catch it; see
# docs/windows-packaging.md). Capture uses ksvideosrc and gdiscreencapsrc.
# Everything the shipped set imports from the colliding DLLs exists in
# Fedora's copies, so no Fedora runtime DLL is replaced.
#
# The plugin list and the `expected=` count in the verify stage are one fact in
# two places; change both in the same commit. The count is a literal on
# purpose: a derived count would pass however many plugins failed to install.
#
# A required plugin must land here, and the image be rebuilt and deployed,
# before stage-windows-runtime.py lists it as required.
#
# The SDK ships no gst-plugins-good licence text (the `ls -1` below lists what
# it does ship), so that text is vendored in this repository and staged by
# stage-windows-runtime.py.
RUN set -eu; \
    url="https://gstreamer.freedesktop.org/data/pkg/windows/${GSTREAMER_VERSION}/mingw/gstreamer-1.0-mingw-x86_64-${GSTREAMER_VERSION}.exe"; \
    curl --fail --show-error --silent --location --proto '=https' --tlsv1.2 \
        --connect-timeout 30 --speed-limit 10000 --speed-time 60 \
        --retry 3 --retry-delay 5 --retry-connrefused \
        "$url" --output /tmp/gstreamer-mingw.exe; \
    printf '%s  %s\n' "$GSTREAMER_SHA256" /tmp/gstreamer-mingw.exe | sha256sum -c -; \
    export HOME=/tmp/gst-wine WINEPREFIX=/tmp/gst-wine/prefix \
        WINEDEBUG=-all WINEARCH=win64; \
    mkdir -p "$WINEPREFIX"; \
    wineboot -u >/dev/null 2>&1 || true; \
    xvfb-run -a wine /tmp/gstreamer-mingw.exe /VERYSILENT /SUPPRESSMSGBOXES \
        /NORESTART /NOCANCEL /SP- '/DIR=Z:\tmp\gstreamer-sdk'; \
    wineserver -k >/dev/null 2>&1 || true; \
    src=/tmp/gstreamer-sdk; \
    test -d "$src/lib/gstreamer-1.0"; \
    test -f "$src/lib/pkgconfig/gstreamer-webrtc-1.0.pc"; \
    sysroot=/usr/x86_64-w64-mingw32/sys-root/mingw; \
    mkdir -p "$sysroot/lib/gstreamer-1.0"; \
    for plugin in \
        libgstapp.dll libgstaudioconvert.dll libgstaudioresample.dll \
        libgstaudiotestsrc.dll libgstautodetect.dll libgstcoreelements.dll \
        libgstdirectsound.dll libgstdirectsoundsrc.dll libgstdtls.dll \
        libgstnice.dll libgstopus.dll libgstrtp.dll libgstrtpmanager.dll \
        libgstsctp.dll \
        libgstjpeg.dll \
        libgstlevel.dll \
        libgstsrtp.dll libgstvideoconvertscale.dll libgstvideorate.dll \
        libgstvideotestsrc.dll libgstvolume.dll libgstvpx.dll \
        libgstopengl.dll \
        libgstwasapi.dll libgstwasapi2.dll libgstwebrtc.dll \
        libgstwebrtcdsp.dll libgstwinks.dll libgstwinscreencap.dll; do \
        install -m 0755 "$src/lib/gstreamer-1.0/$plugin" \
            "$sysroot/lib/gstreamer-1.0/$plugin"; \
    done; \
    for runtime in \
        libgstapp-1.0-0.dll libgstaudio-1.0-0.dll libgstbadaudio-1.0-0.dll \
        libgstbase-1.0-0.dll libgstnet-1.0-0.dll libgstpbutils-1.0-0.dll \
        libgstreamer-1.0-0.dll libgstrtp-1.0-0.dll libgstsctp-1.0-0.dll \
        libgstsdp-1.0-0.dll libgsttag-1.0-0.dll libgstvideo-1.0-0.dll \
        libgstwebrtc-1.0-0.dll libgstwebrtcnice-1.0-0.dll \
        libgstgl-1.0-0.dll libnice-10.dll \
        libopus-0.dll liborc-0.4-0.dll libsrtp2-1.dll libz-1.dll; do \
        if [ -e "$sysroot/bin/$runtime" ]; then \
            echo "GStreamer runtime DLL collides with the Fedora sysroot: $runtime" >&2; \
            exit 1; \
        fi; \
        install -m 0755 "$src/bin/$runtime" "$sysroot/bin/$runtime"; \
    done; \
    # The opengl plugin's own dependencies (libgstcontroller, libgraphene,
    # libjpeg-8, libpng16, and libgmodule for libgstgl), found with objdump.
    # Skip-if-present rather than fail-if-present: these are ordinary
    # third-party libraries Fedora's stack may already ship (Qt pulls libpng
    # and libjpeg), and using Fedora's copy keeps the rule that no Fedora
    # runtime DLL is ever replaced.
    for gldep in \
        libgstcontroller-1.0-0.dll libgraphene-1.0-0.dll \
        libgmodule-2.0-0.dll libjpeg-8.dll libpng16.dll; do \
        if [ -e "$sysroot/bin/$gldep" ]; then \
            echo "gl dep already provided by the sysroot, not staging: $gldep"; \
            continue; \
        fi; \
        if [ ! -e "$src/bin/$gldep" ]; then \
            echo "gl dep missing from the GStreamer SDK: $gldep" >&2; \
            exit 1; \
        fi; \
        install -m 0755 "$src/bin/$gldep" "$sysroot/bin/$gldep"; \
    done; \
    cp -a "$src/include/gstreamer-1.0" "$sysroot/include/"; \
    cp -a "$src/include/orc-0.4" "$sysroot/include/"; \
    for implib in \
        libgstreamer-1.0.dll.a libgstbase-1.0.dll.a libgstallocators-1.0.dll.a \
        libgstapp-1.0.dll.a libgstaudio-1.0.dll.a libgstpbutils-1.0.dll.a \
        libgstrtp-1.0.dll.a libgstsdp-1.0.dll.a libgsttag-1.0.dll.a \
        libgstvideo-1.0.dll.a libgstwebrtc-1.0.dll.a liborc-0.4.dll.a; do \
        install -m 0644 "$src/lib/$implib" "$sysroot/lib/$implib"; \
    done; \
    for pc in \
        gstreamer-1.0.pc gstreamer-base-1.0.pc gstreamer-allocators-1.0.pc \
        gstreamer-app-1.0.pc gstreamer-audio-1.0.pc gstreamer-pbutils-1.0.pc \
        gstreamer-rtp-1.0.pc gstreamer-sdp-1.0.pc gstreamer-tag-1.0.pc \
        gstreamer-video-1.0.pc gstreamer-webrtc-1.0.pc orc-0.4.pc; do \
        install -m 0644 "$src/lib/pkgconfig/$pc" "$sysroot/lib/pkgconfig/$pc"; \
    done; \
    mkdir -p /usr/share/licenses/lightning-gstreamer; \
    echo "--- licence directories the GStreamer SDK actually ships ---"; \
    ls -1 "$src/share/licenses"; \
    echo "--- end licence directories ---"; \
    for licensed in \
        gstreamer-1.0 gst-plugins-base-1.0 gst-plugins-bad-1.0 gst-plugins-rs \
        libnice opus libvpx libsrtp orc zlib webrtc-audio-processing \
        mingw-runtime; do \
        cp -a "$src/share/licenses/$licensed" /usr/share/licenses/lightning-gstreamer/; \
    done; \
    x86_64-w64-mingw32-gcc \
        /usr/local/share/lightning-windows/gst-element-probe.c \
        -o /usr/local/share/lightning-windows/gst-element-probe.exe -mconsole \
        $(x86_64-w64-mingw32-pkg-config --cflags --libs gstreamer-1.0); \
    rm -rf /tmp/gstreamer-mingw.exe /tmp/gstreamer-sdk /tmp/gst-wine

# The element-symbol probe below checks that each element name appears in its
# plugin, which only that plugin's gst_element_register would contain. For
# level:libgstlevel this degrades to file existence, since element and plugin
# share a name; the Wine registry probe (GSTREAMER_ELEMENTS) covers it.
RUN chmod 0755 /usr/local/libexec/lightning-windows/cargo \
    && rpm -qa --qf '%{NAME} %{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}\n' \
       | LC_ALL=C sort > /usr/local/share/lightning-windows-rpms.txt \
    && x86_64-w64-mingw32-g++ --version | head -1 \
    && x86_64-w64-mingw32-qmake-qt6 -query QT_VERSION \
    && file /usr/x86_64-w64-mingw32/sys-root/mingw/bin/Qt6MultimediaQuick.dll \
       | grep -F 'PE32+ executable' \
    && test -f /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/qml/QtMultimedia/qmldir \
    && file /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/plugins/multimedia/ffmpegmediaplugin.dll \
       | grep -F 'PE32+ executable' \
    && for lib in avcodec avformat avutil swresample swscale; do \
         ls /usr/x86_64-w64-mingw32/sys-root/mingw/bin/${lib}-*.dll >/dev/null; \
       done \
    && x86_64-w64-mingw32-pkg-config --print-errors --exists \
         gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
         gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtp-1.0 \
    && test "$(x86_64-w64-mingw32-pkg-config --modversion gstreamer-webrtc-1.0)" \
         = "${GSTREAMER_VERSION}" \
    && { expected=29; \
         staged="$(ls /usr/x86_64-w64-mingw32/sys-root/mingw/lib/gstreamer-1.0/*.dll | wc -l)"; \
         test "$staged" = "$expected" \
           || { echo "staged plugin count is $staged, expected $expected -- the install loop above and this literal are ONE fact in two places" >&2; \
                exit 1; }; } \
    && for plugin in /usr/x86_64-w64-mingw32/sys-root/mingw/lib/gstreamer-1.0/*.dll; do \
         file "$plugin" | grep -F 'PE32+ executable' >/dev/null \
           || { echo "not a PE32+ plugin: $plugin" >&2; exit 1; }; \
       done \
    && for pair in \
         webrtcbin:libgstwebrtc nicesrc:libgstnice dtlssrtpenc:libgstdtls \
         srtpenc:libgstsrtp opusenc:libgstopus vp8enc:libgstvpx \
         rtpvp8pay:libgstrtp rtpbin:libgstrtpmanager appsink:libgstapp \
         queue:libgstcoreelements autoaudiosrc:libgstautodetect \
         webrtcdsp:libgstwebrtcdsp ksvideosrc:libgstwinks \
         gdiscreencapsrc:libgstwinscreencap \
         sctpenc:libgstsctp \
         jpegdec:libgstjpeg \
         level:libgstlevel \
         glupload:libgstopengl glcolorconvert:libgstopengl \
         glcolorscale:libgstopengl gldownload:libgstopengl; do \
         element="${pair%%:*}"; plugin="${pair##*:}"; \
         strings -a -n 3 \
           "/usr/x86_64-w64-mingw32/sys-root/mingw/lib/gstreamer-1.0/${plugin}.dll" \
           | grep -qFx "$element" \
           || { echo "element $element is not in ${plugin}.dll" >&2; exit 1; }; \
       done \
    && file /usr/local/share/lightning-windows/gst-element-probe.exe \
       | grep -F 'PE32+ executable' \
    && wixl --version \
    && makensis '-XTarget amd64-unicode' -VERSION \
    && wine64 --version

LABEL org.opencontainers.image.description="Fedora 44 Linux/MinGW builder for unsigned Lightning Windows artifacts (Qt Multimedia FFmpeg backend, GStreamer WebRTC call media engine)" \
      net.smetonis.lightning.gstreamer="${GSTREAMER_VERSION}"

WORKDIR /builds/lightning-deploy
