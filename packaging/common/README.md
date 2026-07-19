# Shared desktop metadata

Since Lightning 0.7 the source repository ships its own desktop entry and
hicolor application icons and installs them via `cmake --install`; packages
inherit them from the staged tree. The desktop file kept here is only a
fallback used when an older pinned source SHA (pre-icon) is rebuilt. The
AppStream metainfo, copyright, and licence files remain packaging-side
additions.
