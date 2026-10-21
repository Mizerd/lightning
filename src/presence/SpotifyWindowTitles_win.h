#pragma once

// Windows only, and deliberately free of Qt so the cross compiler can check
// it alone. The captions of every top-level window owned by a process whose
// image is Spotify.exe. GetWindowTextW reads a window of another process from
// the system's copy of its caption without sending it a message, so a hung
// Spotify cannot block the caller. Nothing else about the windows or other
// processes is read or kept.

#include <string>
#include <vector>

namespace spotifywin {

std::vector<std::wstring> spotifyWindowTitles();

}   // namespace spotifywin
