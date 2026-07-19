# Package layout

Lightning's CMake install target places `matrix-client` in `bin`. QML files
and the emoji catalogue are Qt resources compiled into that executable, and
the Rust SDK bridge is linked statically. The deployment project adds:

```text
/usr/bin/matrix-client
/usr/share/applications/lightning.desktop
/usr/share/metainfo/lightning.metainfo.xml
/usr/share/licenses/lightning/copyright
/usr/share/doc/lightning/LICENSE
/usr/share/doc/lightning/README.md
```

Qt, libsecret, SQLite and other system libraries remain dynamically linked and
are declared as native DEB/RPM dependencies.
Since Lightning 0.7 the source supplies the application icon set
(`/usr/share/icons/hicolor/<size>/apps/lightning.png`) and its own desktop
entry through `cmake --install`; packages ship them as staged.
