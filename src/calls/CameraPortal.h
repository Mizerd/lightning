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

    static bool available();

    static bool cameraPresent();

    void requestAccess();

    void cancel();

    static QString failureCategory(const QString &errorName,
                                   bool openingRemote);
    bool busy() const { return m_busy; }

Q_SIGNALS:
    void ready(int pipewireFd);
    void cancelled();
    void failed(const QString &category);

private:
    void reset();
#if defined(HAVE_QT_DBUS) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    void openRemote();
#endif

    bool m_busy = false;
    QTimer m_requestTimeout;
    quint64 m_generation = 0;
};
