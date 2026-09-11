// Camera access through xdg-desktop-portal (org.freedesktop.portal.Camera).
//
// WHY THIS EXISTS. Lightning's camera capture opens `v4l2src` on Linux, which
// needs the raw `/dev/video*` node. Inside a sandbox that node is not there:
// Flatpak has no camera-only device permission, `--device=all` is the only
// static route to one and Flathub rejects it, so a packaged Flatpak has
// working audio calls, working screen sharing, and a camera button that
// cannot ever produce a frame. The portal is the sanctioned answer and it
// needs NO static permission at all — that is the whole point of a portal:
// the user grants the camera at the moment it is asked for, to this app, and
// the sandbox manifest says nothing about cameras.
//
// THE HANDSHAKE IS THE SCREENCAST ONE WITH THE MIDDLE TAKEN OUT. There is no
// session, no source list and no picker:
//
//   AccessCamera        -> Request; Response 0 = the user granted the camera
//   OpenPipeWireRemote  -> a file descriptor for a PipeWire remote in which
//                          ONLY camera nodes are visible
//
// Introspected on a live xdg-desktop-portal rather than assumed:
//   .AccessCamera       method   a{sv} -> o
//   .OpenPipeWireRemote method   a{sv} -> h
//   .IsCameraPresent    property b
//   .version            property u   (1)
//
// WHAT THE CALLER GETS, and why it is a bare fd with no node id. The
// ScreenCast portal grants ONE node — the monitor or window the user picked —
// so its `ready` carries both the node and the remote. The Camera portal
// grants the camera SET: the remote it opens exposes the machine's cameras
// and nothing else, and the node inside it is chosen by connecting. So
// `pipewiresrc fd=<fd>` with its default `autoconnect` is the whole
// selection, and a node id would have to be enumerated out of that remote
// with libpipewire, which this client does not link. CHOOSING BETWEEN TWO
// CAMERAS IN A SANDBOX IS THEREFORE NOT IMPLEMENTED — stated here rather than
// left to be discovered, since the settings picker enumerates through Qt and
// Qt sees no `/dev/video*` inside the sandbox either, so it already offers
// nothing to choose.
//
// Compiled everywhere; without Qt DBus (HAVE_QT_DBUS) `available()` is false
// and `requestAccess()` fails with a category, exactly as ScreenCastPortal
// does, so callers keep their honest refusal and their existing fallback.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class CameraPortal : public QObject
{
    Q_OBJECT

public:
    explicit CameraPortal(QObject *parent = nullptr);
    ~CameraPortal() override;

    /// True when the Camera portal interface answers on this session bus.
    /// False on a build without Qt DBus, on a desktop with no portal, and on
    /// every non-Linux platform (where the camera is opened directly and
    /// there is no broker to ask).
    ///
    /// Says nothing about whether a camera EXISTS — that is
    /// `cameraPresent()`, deliberately separate: "no portal" and "a portal
    /// that reports no camera" send a reader to completely different places.
    static bool available();

    /// The portal's own `IsCameraPresent`. False when the property cannot be
    /// read, which is the same answer a desktop with no camera gives and is
    /// the safe one: it keeps the direct route in charge on a machine where
    /// the portal has nothing to offer.
    static bool cameraPresent();

    /// Ask the user for the camera. Answers exactly once with `ready`,
    /// `cancelled` or `failed`.
    ///
    /// The portal remembers the grant, so this raises a dialog on the first
    /// call and returns immediately afterwards. That is why there is no
    /// "ask once at startup" here: asking before the user presses the camera
    /// button would be a permission prompt for a feature they have not used.
    void requestAccess();

    /// Abandon an in-flight request. Idempotent. There is no session to
    /// close — the Camera portal has none — so this only stops a pending
    /// request from being acted on.
    void cancel();
    bool busy() const { return m_busy; }

Q_SIGNALS:
    /// Access was granted and the PipeWire remote holding the cameras is
    /// open. `pipewireFd` is a DUP of the portal's descriptor and THE
    /// RECEIVER OWNS IT: GStreamer's pipewiresrc dups it again, so it must be
    /// closed when the element that was given it goes away — or on any path
    /// that declines to build one, or a declined camera leaks a descriptor
    /// per attempt (the ScreenCast lane learned that one the expensive way).
    void ready(int pipewireFd);
    /// The user declined, or the portal denied. NOT an error: no message.
    void cancelled();
    /// A coarse, safe-to-log category. Never a raw D-Bus error string, which
    /// can carry paths and device names.
    void failed(const QString &category);

private:
    void reset();
#if defined(HAVE_QT_DBUS) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    /// Step 2: ask for a descriptor to the PipeWire remote the grant opened.
    void openRemote();
#endif

    bool m_busy = false;
    /// Frees a wedged request. The portal's `Response` signal is the only
    /// thing that ends one, and a dialog the user never answers — or a portal
    /// that never replies — would otherwise leave `m_busy` set forever and
    /// the camera dead until the app restarts. Exactly the failure
    /// ScreenCastPortal's timer was added for.
    QTimer m_requestTimeout;
    /// Distinguishes replies from a superseded request: a stale Response for
    /// a camera the user already turned back off must not open a remote.
    quint64 m_generation = 0;
};
