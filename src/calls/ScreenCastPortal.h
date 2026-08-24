// Screen capture through xdg-desktop-portal (org.freedesktop.portal.ScreenCast).
//
// This is the ONLY way Lightning obtains a screen or window stream. It never
// touches a framebuffer, never enumerates windows itself, and never asks the
// compositor directly: the portal shows ITS OWN picker, the user chooses
// there, and Lightning receives a PipeWire node id for exactly what was
// chosen. That is what makes screen sharing safe on Wayland, and it is also
// why there is no Lightning-drawn source picker — drawing one would mean
// enumerating windows we are not entitled to see.
//
// The exchange is a three-step async handshake, each step returning a
// Request object path whose `Response` signal carries the result:
//
//   CreateSession  -> session_handle
//   SelectSources  -> (user picks in the portal's dialog)
//   Start          -> streams: a(ua{sv}), first element = PipeWire node id
//
// Failure at any step, including the user pressing Cancel, ends as
// `cancelled` or `failed` — never as a half-open session, and never as a
// silent no-op that leaves a "sharing" button lit.
//
// Compiled only where Qt DBus exists (HAVE_QT_DBUS); elsewhere
// `available()` is false and callers keep their honest refusal.
#pragma once

#include <QObject>
#include <QVariantMap>
#include <QString>

class ScreenCastPortal : public QObject
{
    Q_OBJECT

public:
    /// What the user may pick. Mirrors the portal's own bitmask so the
    /// values can be passed straight through.
    enum SourceType {
        Monitor = 1,
        Window = 2,
        Virtual = 4,
    };

    explicit ScreenCastPortal(QObject *parent = nullptr);
    ~ScreenCastPortal() override;

    /// True when a portal is actually reachable on this session bus. False on
    /// a build without Qt DBus, or a desktop with no portal — in which case
    /// screen sharing is refused honestly rather than offered and broken.
    static bool available();

    /// Begin the handshake. `types` is a SourceType bitmask; the portal
    /// decides what it can actually offer. Answers exactly once with
    /// `ready`, `cancelled` or `failed`.
    void requestShare(int types = Monitor | Window);
    /// Abandon an in-flight request and close any open session. Idempotent.
    void cancel();
    bool busy() const { return m_busy; }

Q_SIGNALS:
    /// The user chose a source, and the PipeWire remote for it is open.
    ///
    /// BOTH values are needed. A portal ScreenCast node lives in a PipeWire
    /// remote the portal opens for US: `pipewiresrc path=<node>` alone asks
    /// the caller's own default remote for a node that need not be visible
    /// there, which is how a share ends up producing no frames at all while
    /// every step of the handshake reports success. `fd` is the duplicated
    /// descriptor from OpenPipeWireRemote and the OWNER is the receiver —
    /// GStreamer's pipewiresrc dups it again, so it must be closed once the
    /// element has been created.
    void ready(unsigned pipewireNodeId, int pipewireFd);
    /// The user declined. NOT an error: no message should be shown.
    void cancelled();
    /// A coarse, safe-to-log category. Never a raw D-Bus error string, which
    /// can carry paths and window titles.
    void failed(const QString &category);

private:
    void reset();
#ifdef HAVE_QT_DBUS
    void selectSources(int types);
    void startSession();
    void handleStreams(const QVariantMap &results);
    /// Step 4: ask the portal for a file descriptor to the PipeWire remote
    /// holding the node it just granted.
    void openRemote(unsigned nodeId);
#endif

    bool m_busy = false;
    QString m_sessionHandle;
    /// Distinguishes replies from a superseded request: a stale Response for
    /// a share the user already cancelled must not start a capture.
    quint64 m_generation = 0;
};
