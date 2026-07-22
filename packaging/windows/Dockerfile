FROM fedora@sha256:6c75d5bf57cb0fa5aa4b92c6a83c86c791644496d9ac230de7711f5b8ec3b898

ARG RUST_TOOLCHAIN=1.95.0
ARG QT_MULTIMEDIA_VERSION=6.11.1
ARG QT_MULTIMEDIA_SHA256=390f8e52ddee3aca5c4de7eead900c84c4fa61ff6d1f0ebea9c7543365c09b0a
ARG FFMPEG_VERSION=7.1.1
ARG FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

LABEL org.opencontainers.image.title="Lightning Windows cross-package builder" \
      org.opencontainers.image.description="Fedora 44 Linux/MinGW builder for unsigned Lightning Windows artifacts (Qt Multimedia FFmpeg backend)" \
      org.opencontainers.image.source="https://gitlab.smetonis.net/Mizerd/lightning-deploy" \
      net.smetonis.lightning.builder-kind="linux-cross" \
      net.smetonis.lightning.target="x86_64-pc-windows-gnu" \
      net.smetonis.lightning.qt="6.11.1" \
      net.smetonis.lightning.ffmpeg="7.1.1" \
      net.smetonis.lightning.rust="${RUST_TOOLCHAIN}"

# The base image is content-addressed. The compiler, Qt, Rust installer,
# installer generators, and Wine are also pinned to concrete Fedora NEVRAs.
# Transitive RPMs are captured in /usr/local/share/lightning-windows-rpms.txt
# so every CI artifact records the exact resolved image contents.
RUN dnf install -y --setopt=install_weak_deps=False \
        bash \
        ca-certificates \
        cmake-4.3.0-1.fc44 \
        coreutils \
        curl \
        diffutils \
        file \
        findutils \
        gcc-16.1.1-2.fc44 \
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
        mingw64-qt6-qtbase-6.11.1-1.fc44 \
        mingw64-qt6-qtdeclarative-6.11.1-2.fc44 \
        mingw64-qt6-qtimageformats-6.11.1-1.fc44 \
        mingw64-qt6-qtmultimedia-6.11.1-1.fc44 \
        mingw64-qt6-qtsvg-6.11.1-1.fc44 \
        mingw64-qt6-qttools-6.11.1-1.fc44 \
        mingw64-qt6-qttranslations-6.11.1-1.fc44 \
        mingw64-sqlite-3.51.3-1.fc44 \
        mingw64-zlib-1.3.2-1.fc44 \
        msitools-0.106.58-1.fc44 \
        nasm-3.01-3.fc44 \
        ninja-build-1.13.2-2.fc44 \
        python3-3.14.6-1.fc44 \
        python3-pyyaml-6.0.3-3.fc44 \
        rustup-1.29.0-4.fc44 \
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

# Cross-build FFmpeg (LGPLv3, shared) for the Windows target so Qt Multimedia's
# FFmpeg backend can be built. The Fedora MinGW Qt ships ONLY the Windows Media
# Foundation backend, which delivers a video's first buffered frames and then
# stalls the surface; the FFmpeg backend decodes it correctly. LGPL build (no
# --enable-gpl / --enable-nonfree; native decoders only). --disable-iconv drops
# the subtitle charset path Lightning does not use (and which fails under
# GCC 16 + MinGW). The native host compiler must use the NATIVE assembler, so
# /usr/bin precedes the target bin dir on PATH here; the cross tools are still
# resolved by their x86_64-w64-mingw32- prefix.
RUN set -eu; \
    url="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"; \
    curl --fail --show-error --silent --location --proto '=https' --tlsv1.2 \
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

# Fedora's 6.11.1 MinGW multimedia RPM contains Qt6Multimedia.dll and the
# Windows Media Foundation backend but omits the QtMultimedia QML module and the
# FFmpeg backend plugin. Build both from the matching official Qt source: the
# quickmultimedia QML module and ffmpegmediaplugin (the FFmpeg decode backend,
# which links the av*/sw* DLLs cross-built above). The WMF plugin is NOT built
# here (its sources include <D3d11.h>, which is case-sensitive on this Linux
# cross-build) — the RPM's windowsmediaplugin.dll is kept as the fallback. The
# FFmpeg plugin's PCH also includes <D3d11.h>, so a case-only symlink to the
# lowercase MinGW header lets it compile. Host ShaderTools supplies qsb.
RUN set -eu; \
    dnf install -y --setopt=install_weak_deps=False \
        qt6-qtshadertools-devel-6.11.1-1.fc44.x86_64; \
    dnf clean all; \
    rm -rf /var/cache/dnf; \
    ln -sf d3d11.h /usr/x86_64-w64-mingw32/sys-root/mingw/include/D3d11.h; \
    source_url="https://download.qt.io/official_releases/qt/6.11/${QT_MULTIMEDIA_VERSION}/submodules/qtmultimedia-everywhere-src-${QT_MULTIMEDIA_VERSION}.tar.xz"; \
    curl --fail --show-error --silent --location --proto '=https' --tlsv1.2 \
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
    && wixl --version \
    && makensis '-XTarget amd64-unicode' -VERSION \
    && wine64 --version

WORKDIR /builds/lightning-deploy
