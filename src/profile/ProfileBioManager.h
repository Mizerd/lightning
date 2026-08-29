#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

// Profile biographies (MSC4440 over MSC4133 extended profile fields).
//
// The policy half of the feature; the protocol half is rust/src/bio.rs. This
// is deliberately the same shape as ProfileBannerManager — the two features
// share a transport, a capability question and an honesty problem, and letting
// their policies drift apart would mean two different answers to "does this
// server do extended profiles?" on one card.
//
// Honesty rules, the same ones presence and banners follow:
//   * a user with no bio and a user we have not asked about are both rendered
//     as NOTHING. `bioFor()` returns "" for both, and QML must not turn that
//     into a placeholder, an error, or an empty card;
//   * a homeserver that does not implement extended profile fields is
//     `supported == false`, which is a DIFFERENT fact from "no bio" and is
//     what hides the editing surface — rather than offering a control that
//     cannot work. An absent field answers M_NOT_FOUND and is NOT this case
//     (rust/src/banner.rs::is_unsupported draws that line, and both managers
//     read the same answer from it);
//   * the text that arrives is PLAIN, bounded and control-stripped in Rust,
//     and must be rendered with Text.PlainText. It is free text written by a
//     remote user.
//
// Nothing is applied optimistically. `ownBio` changes only when the server has
// accepted the write, and it takes the value the WRITE PATH reports — which is
// the bounded text actually stored, not the text that was typed.
class ProfileBioManager : public QObject
{
    Q_OBJECT

    // The backend can read extended profile fields at all.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // ...and this homeserver actually answered one. False once the server has
    // told us it does not know the endpoint.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // Bumped whenever any cached bio changes; QML bindings read it to
    // re-evaluate bioFor().
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The local account's own bio, or "" — the Settings editor's model.
    Q_PROPERTY(QString ownBio READ ownBio NOTIFY revisionChanged)
    // A write is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    // The client-side ceiling, so the editor can show a counter and bound the
    // field without hard-coding a number QML would have to keep in step with
    // rust/src/bio.rs.
    Q_PROPERTY(int maxLength READ maxLength CONSTANT)

public:
    explicit ProfileBioManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool supported() const { return m_supported; }
    int revision() const { return m_revision; }
    bool busy() const { return m_pendingWrite != 0; }
    QString lastError() const { return m_lastError; }
    QString ownBio() const;
    // Mirrors MAX_BIO_CHARS in rust/src/bio.rs. MSC4440 specifies no limit and
    // names an unbounded bio as its own security consideration.
    static constexpr int kMaxLength = 2048;
    int maxLength() const { return kMaxLength; }

    // "" when unknown or absent — the two are deliberately indistinguishable
    // here, because they look the same on a card. Pure read, safe in a
    // binding; re-read on revisionChanged.
    Q_INVOKABLE QString bioFor(const QString &userId) const;
    // Asks once per user per session. Idempotent and deduplicated: a profile
    // popover opening twice must not cost two requests.
    Q_INVOKABLE void request(const QString &userId);
    // Ask again even if this user has been asked about already, for the one
    // case where the answer is known to have changed — the account's own bio
    // straight after writing it.
    Q_INVOKABLE void refresh(const QString &userId);
    // EMPTY (or whitespace-only) clears the bio.
    Q_INVOKABLE void setOwnBio(const QString &text);
    Q_INVOKABLE void clearOwnBio();

Q_SIGNALS:
    void availableChanged();
    void supportedChanged();
    void revisionChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void handleReceived(quint64 opId, const QString &userId, const QString &bio,
                        bool supported);
    void handleSet(quint64 opId, bool ok, const QString &bio,
                   const QString &category);
    void clearSession();
    void setLastError(const QString &error);
    void cache(const QString &userId, const QString &bio);

    // Bounded: one entry per profile card anyone has opened this session.
    static constexpr int kMaxCached = 256;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_cache;   // userId -> bio ("" = asked, none)
    QSet<QString> m_asked;
    QHash<quint64, QString> m_inFlight;
    quint64 m_nextOpId = 1;
    quint64 m_pendingWrite = 0;
    int m_revision = 0;
    bool m_supported = true;
    QString m_lastError;
};
