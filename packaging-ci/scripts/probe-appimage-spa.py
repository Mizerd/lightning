#!/usr/bin/env python3
"""Load the AppImage's SPA audio converter without connecting to any server.

Run with LD_LIBRARY_PATH pointing at the extracted AppDir's usr/lib (plus
the host libraries excluded by linuxdeploy). No devices, portals or audio
servers are opened. libpipewire's stable C API loads the modules itself, so
this catches missing dlopen dependencies that ldd and gst-inspect cannot.
"""

import argparse
import ctypes
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appdir", type=Path)
    appdir = parser.parse_args().appdir.resolve()
    os.environ["SPA_PLUGIN_DIR"] = str(appdir / "usr/lib/spa-0.2")
    pw = ctypes.CDLL(str(appdir / "usr/lib/libpipewire-0.3.so.0"),
                     use_errno=True)

    class Support(ctypes.Structure):
        _fields_ = [("type", ctypes.c_char_p), ("data", ctypes.c_void_p)]

    pw.pw_init.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    pw.pw_init.restype = None
    pw.pw_deinit.argtypes = []
    pw.pw_deinit.restype = None
    pw.pw_get_support.argtypes = [ctypes.POINTER(Support), ctypes.c_uint]
    pw.pw_get_support.restype = ctypes.c_uint
    pw.pw_load_spa_handle.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                    ctypes.c_void_p, ctypes.c_uint,
                                    ctypes.POINTER(Support)]
    pw.pw_load_spa_handle.restype = ctypes.c_void_p
    pw.pw_unload_spa_handle.argtypes = [ctypes.c_void_p]
    pw.pw_unload_spa_handle.restype = ctypes.c_int

    pw.pw_init(None, None)
    failed = False
    try:
        support = (Support * 32)()
        count = pw.pw_get_support(support, len(support))
        for library, factory in (
            (b"support/libspa-support", b"support.system"),
            (b"audioconvert/libspa-audioconvert", b"audio.convert"),
        ):
            ctypes.set_errno(0)
            handle = pw.pw_load_spa_handle(library, factory, None, count, support)
            if handle:
                print(f"SPA factory {factory.decode()}: loadable")
                pw.pw_unload_spa_handle(handle)
            else:
                print(f"SPA factory {factory.decode()}: FAILED "
                      f"({os.strerror(ctypes.get_errno())}; {library.decode()})")
                failed = True
    finally:
        pw.pw_deinit()
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
