// Camera access through xdg-desktop-portal (org.freedesktop.portal.Camera).
//
// Direct capture uses `v4l2src` on `/dev/video*`, which a Flatpak does not
// have: there is no camera-only device permission, and Flathub rejects
// `--device=all`. The portal needs no static permission; the user grants the
// camera when it is first used.
//
// The handshake is ScreenCast's without session, source list or picker:
//
//   AccessCamera        -> Request; Response 0 = the user granted the camera
//   OpenPipeWireRemote  -> fd of a PipeWire remote exposing only cameras
//
// Introspected on a live xdg-desktop-portal:
//   .AccessCamera       method   a{sv} -> o
//   .OpenPipeWireRemote method   a{sv} -> h
//   .IsCameraPresent    property b
//   .version            property u   (1)
//
// The caller gets a bare fd, no node id: the remote exposes the camera set,
// and `pipewiresrc fd=<fd>` with its default `autoconnect` picks one.
// Choosing between cameras in a sandbox is therefore not implemented (Qt sees
// no `/dev/video*` there either, so the settings picker offers nothing).
//
// Without Qt DBus (HAVE_QT_DBUS), available() is false and requestAccess()
// fails with a category, so callers keep their fallback.
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

    /// True when the Camera portal interface answers on the session bus.
    /// False without Qt DBus, without a portal, and on non-Linux platforms.
    /// Says nothing about whether a camera exists; see cameraPresent().
    static bool available();

    /// The portal's `IsCameraPresent`. False when unreadable, which keeps the
    /// direct route in charge.
    static bool cameraPresent();

    /// Ask the user for the camera. Answers exactly once with `ready`,
    /// `cancelled` or `failed`. The portal remembers the grant, so only the
    /// first call shows a dialog; never ask before the user presses the
    /// camera button.
    void requestAccess();

    /// Abandon an in-flight request. Idempotent; there is no session to close.
    void cancel();
    bool busy() const { return m_busy; }

Q_SIGNALS:
    /// Access was granted and the PipeWire remote holding the cameras is open.
    /// `pipewireFd` is a dup owned by the receiver, which must close it on
    /// every path, including declining to build a pipeline.
    void ready(int pipewireFd);
    /// The user declined or the portal denied. Not an error; show no message.
    void cancelled();
    /// A coarse, safe-to-log category, never a raw D-Bus error string (which
    /// can carry paths and device names).
    void failed(const QString &category);

private:
    void reset();
#if defined(HAVE_QT_DBUS) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    /// Step 2: get a descriptor to the PipeWire remote the grant opened.
    void openRemote();
#endif

    bool m_busy = false;
    /// Frees a wedged request: only the portal's `Response` ends one, and a
    /// missing one would leave `m_busy` set and the camera dead until restart.
    QTimer m_requestTimeout;
    /// Discards replies to a superseded request, so a stale Response cannot
    /// open a remote for a camera the user already turned off.
    quint64 m_generation = 0;
};
