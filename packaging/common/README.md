# Shared desktop metadata

Lightning currently embeds its QML and emoji catalogue in the executable and
does not ship desktop, AppStream, icon, or repository-wide license files.
These external files add desktop integration without changing the source
repository. No icon is duplicated; the package intentionally relies on the
desktop environment's generic application fallback.
