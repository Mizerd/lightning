#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

// A display-name colour the user chooses, carried in their Matrix profile
// (`org.lightning.name_color`, MSC4133) so other Lightning clients see it.
//
// The policy half; the protocol half is rust/src/namecolor.rs.
//
// # What this does NOT do
//
// Paint the colour. It carries the CHOICE, and `AppTheme.userColor` decides
// what that becomes on the viewer's background — because a colour legible on
// the sender's theme can be invisible on the viewer's, and a profile field is
// written by its owner and read by everybody else. Nobody gets to hand every
// other user an unreadable name.
//
// # Fetching
//
// Lazily, once per user, and never again for the life of the session unless
// the caller asks. A sender colour is wanted for every name in a busy
// timeline, so a fetch per message would be a request storm against the
// profile endpoint; `m_asked` is what makes `colorFor()` safe to call from a
// binding that re-evaluates constantly.
//
// Honesty rules, the same ones the banner and presence follow: a user with no
// colour and a user nobody has asked about are both rendered as NOTHING, and
// a homeserver with no extended profile fields is `supported == false` — a
// different fact, and the one that hides the editing surface rather than
// offering a control that cannot work.
class NameColorManager : public QObject
{
    Q_OBJECT

    // The backend can carry name colours at all.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // ...and this homeserver answered one. False once it has said it does not
    // know the endpoint.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // Bumped whenever any cached colour changes; QML bindings read it so
    // colorFor() re-evaluates when an answer lands.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The local account's own colour, or "" — the Settings control's model.
    Q_PROPERTY(QString ownColor READ ownColor NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit NameColorManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool supported() const { return m_supported; }
    int revision() const { return m_revision; }
    QString ownColor() const;
    bool busy() const { return m_pendingSet != 0; }
    QString lastError() const { return m_lastError; }

    /// The colour this user chose, or "" when they chose none, nobody has
    /// asked yet, or the server cannot answer. Asking is a SIDE EFFECT: the
    /// first call for a user schedules the fetch and returns "" for now.
    Q_INVOKABLE QString colorFor(const QString &userId);
    /// Set or clear (empty) the local account's colour.
    Q_INVOKABLE void setOwnColor(const QString &value);
    /// Drop everything — a new account must not inherit the last one's map.
    void clear();

Q_SIGNALS:
    void availableChanged();
    void supportedChanged();
    void revisionChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void setSupported(bool supported);
    void setLastError(const QString &error);

    MatrixClient *m_client = nullptr;
    // userId -> "#rrggbb", or "" for "asked, and they have none".
    QHash<QString, QString> m_colors;
    // Every user a fetch has been dispatched for. Separate from m_colors so
    // "no colour" is remembered as an ANSWER and not re-asked forever.
    QSet<QString> m_asked;
    QHash<quint64, QString> m_pending;
    quint64 m_nextOp = 1;
    quint64 m_pendingSet = 0;
    int m_revision = 0;
    bool m_supported = true;
    QString m_lastError;
};
