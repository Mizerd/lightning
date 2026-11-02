#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QString>

class ScreenCastPortal : public QObject
{
    Q_OBJECT

public:
    enum SourceType {
        Monitor = 1,
        Window = 2,
        Virtual = 4,
    };

    explicit ScreenCastPortal(QObject *parent = nullptr);
    ~ScreenCastPortal() override;

    static bool available();

    void requestShare(int types = Monitor | Window, int preparationBoundMs = 0,
                      int pickerBoundMs = 0);
    void cancel();
    bool busy() const { return m_busy; }

Q_SIGNALS:
    /// The user chose a source and its PipeWire remote is open. Both values
    /// are needed: the node lives in the remote the portal opened for us, and
    /// `pipewiresrc path=<node>` alone produces no frames. `pipewireFd` is a
    /// duplicated descriptor owned by the receiver.
    void ready(unsigned pipewireNodeId, int pipewireFd);
    void cancelled();
    void failed(const QString &category);

private:
    void reset();
    void armAnswerWatchdog(int ms);
#ifdef HAVE_QT_DBUS
    void selectSources(int types);
    void startSession();
    void handleStreams(const QVariantMap &results);
    void openRemote(unsigned nodeId);
#endif

    bool m_busy = false;
    QTimer m_requestTimeout;
    QTimer m_answerWatchdog;
    int m_pickerBoundMs = 0;
    QString m_sessionHandle;
    quint64 m_generation = 0;
};
