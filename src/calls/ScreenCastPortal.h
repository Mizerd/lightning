// Screen capture through xdg-desktop-portal (org.freedesktop.portal.ScreenCast).
//
// The preferred route everywhere on Linux and the only one on Wayland: the
// portal shows its own picker and hands back a PipeWire node for exactly
// what the user chose, so Lightning never enumerates windows or touches a
// framebuffer. Only on an X11 session without a portal does
// SfuCallController fall back to its own display picker; on Wayland there is
// no fallback (see linuxShareRoute()).
//
// The handshake is asynchronous; each step returns a Request whose
// `Response` signal carries the result:
//
//   CreateSession  -> session_handle
//   SelectSources  -> (user picks in the portal's dialog)
//   Start          -> streams: a(ua{sv}), first element = PipeWire node id
//
// Every failure, including Cancel, ends as `cancelled` or `failed`, never as a
// half-open session. Without Qt DBus (HAVE_QT_DBUS), available() is false.
#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QString>

class ScreenCastPortal : public QObject
{
    Q_OBJECT

public:
    /// What the user may pick; the portal's own bitmask, passed through.
    enum SourceType {
        Monitor = 1,
        Window = 2,
        Virtual = 4,
    };

    explicit ScreenCastPortal(QObject *parent = nullptr);
    ~ScreenCastPortal() override;

    /// True when a portal is reachable on the session bus. False without Qt
    /// DBus or without a portal, so sharing is refused rather than offered
    /// broken.
    static bool available();

    /// Begin the handshake. `types` is a SourceType bitmask; the portal decides
    /// what it can offer. Answers exactly once with `ready`, `cancelled` or
    /// `failed`.
    ///
    /// No-answer bounds, for a caller that has somewhere else to go (an X11
    /// session, where SfuCallController has its own display picker): when
    /// `preparationBoundMs` > 0, a portal that has not created the session
    /// and accepted the source selection within that long (no human is
    /// involved in either step) is abandoned with `failed("no_answer")`;
    /// when `pickerBoundMs` > 0, so is one whose picker (Start) has not
    /// answered within that long. xdg-desktop-portal-kde on X11 advertises
    /// ScreenCast and then never produces a session (live 2026-10-07). 0
    /// keeps only the generous overall timeout.
    void requestShare(int types = Monitor | Window, int preparationBoundMs = 0,
                      int pickerBoundMs = 0);
    /// Abandon an in-flight request and close any open session. Idempotent.
    void cancel();
    bool busy() const { return m_busy; }

Q_SIGNALS:
    /// The user chose a source and its PipeWire remote is open. Both values
    /// are needed: the node lives in the remote the portal opened for us, and
    /// `pipewiresrc path=<node>` alone produces no frames. `pipewireFd` is a
    /// duplicated descriptor owned by the receiver.
    void ready(unsigned pipewireNodeId, int pipewireFd);
    /// The user declined. Not an error; show no message.
    void cancelled();
    /// A coarse, safe-to-log category, never a raw D-Bus error string (which
    /// can carry paths and window titles).
    void failed(const QString &category);

private:
    void reset();
    /// (Re)arms the no-answer bound; 0 stops it.
    void armAnswerWatchdog(int ms);
#ifdef HAVE_QT_DBUS
    void selectSources(int types);
    void startSession();
    void handleStreams(const QVariantMap &results);
    /// Step 4: get a descriptor to the PipeWire remote holding the granted
    /// node.
    void openRemote(unsigned nodeId);
#endif

    bool m_busy = false;
    /// Frees a wedged picker: only the portal's `Response` ends a request, and
    /// one that never arrives would leave `m_busy` set and sharing refused
    /// until restart. Generous, since a human is choosing.
    QTimer m_requestTimeout;
    /// The caller's tighter no-answer bound (requestShare()), per step.
    QTimer m_answerWatchdog;
    int m_pickerBoundMs = 0;
    QString m_sessionHandle;
    /// Discards replies to a superseded request, so a stale Response cannot
    /// start a capture.
    quint64 m_generation = 0;
};
