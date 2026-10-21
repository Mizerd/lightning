#include "presence/SpotifyWindowTitles_win.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <unordered_map>

namespace spotifywin {

namespace {

struct EnumState {
    std::vector<std::wstring> titles;
    // pid -> is Spotify.exe, so each process is opened at most once a pass.
    std::unordered_map<DWORD, bool> isSpotify;
    int windowsSeen = 0;
};

// Bounds one pass: a desktop has hundreds of top-level windows, never this
// many, and a pathological one cannot make a 5 s poll expensive.
constexpr int kMaxWindows = 4096;
constexpr int kMaxTitles = 16;

bool processIsSpotify(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok || size == 0)
        return false;
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    }
    return _wcsicmp(base, L"Spotify.exe") == 0;
}

BOOL CALLBACK collect(HWND window, LPARAM param)
{
    auto *state = reinterpret_cast<EnumState *>(param);
    if (++state->windowsSeen > kMaxWindows
        || static_cast<int>(state->titles.size()) >= kMaxTitles)
        return FALSE;
    // The owner first: only Spotify's windows are read, and never one of this
    // process (GetWindowText MESSAGES a window of the calling process).
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0 || pid == GetCurrentProcessId())
        return TRUE;
    auto it = state->isSpotify.find(pid);
    if (it == state->isSpotify.end())
        it = state->isSpotify.emplace(pid, processIsSpotify(pid)).first;
    if (!it->second)
        return TRUE;
    // Only Spotify's own UI windows (Chromium Embedded: Chrome_WidgetWin_*),
    // not its helper and IME windows. GetClassNameW reads the class without
    // messaging the window. Visibility is NOT required on purpose: Spotify
    // closed to the tray hides its main window and keeps naming it.
    wchar_t cls[64];
    const int clsLen = GetClassNameW(window, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0])));
    if (clsLen <= 0 || std::wcsncmp(cls, L"Chrome_WidgetWin_", 17) != 0)
        return TRUE;
    // Another process's window: GetWindowTextW copies the system's caption
    // and sends nothing.
    wchar_t buffer[512];
    const int copied = GetWindowTextW(window, buffer,
                                      static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    if (copied <= 0)
        return TRUE;
    std::wstring caption(buffer, static_cast<size_t>(copied));
    // Only "Artist - Title" captions count; "Spotify Premium" and empty
    // helper captions are dropped here already.
    if (caption.find(L" - ") == std::wstring::npos)
        return TRUE;
    state->titles.push_back(std::move(caption));
    return TRUE;
}

}   // namespace

std::vector<std::wstring> spotifyWindowTitles()
{
    EnumState state;
    EnumWindows(collect, reinterpret_cast<LPARAM>(&state));
    return std::move(state.titles);
}

}   // namespace spotifywin

#else

namespace spotifywin {

std::vector<std::wstring> spotifyWindowTitles() { return {}; }

}   // namespace spotifywin

#endif
