# Package layout

Lightning's CMake install target places `matrix-client` in `bin`. QML files
and the emoji catalogue are Qt resources compiled into that executable, and
the Rust SDK bridge is linked statically. The deployment project adds:

```text
/usr/bin/matrix-client
/usr/share/applications/lightning.desktop
/usr/share/metainfo/lightning.metainfo.xml
/usr/share/licenses/lightning/copyright
```

Qt, libsecret, SQLite and other system libraries remain dynamically linked and
are declared as native DEB/RPM dependencies or captured in the Nix closure.
The source currently supplies no application icon, so packages do not invent
or duplicate one.
