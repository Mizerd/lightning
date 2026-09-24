#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

// Profile biographies (MSC4440 over MSC4133 extended profile fields). The
// protocol half is rust/src/bio.rs. Same shape as ProfileBannerManager, so the
// two agree on whether the server supports extended profiles.
//
//   * No bio and not-yet-asked both render as nothing: bioFor() returns ""
//     and QML must not show a placeholder or error.
//   * A server without extended profile fields is `supported == false`,
//     which hides the editor. An absent field (M_NOT_FOUND) is not this case
//     (rust/src/banner.rs::is_unsupported).
//   * The text is plain, bounded and control-stripped in Rust, and must be
//     rendered with Text.PlainText: it is remote free text.
//
// Nothing is optimistic: `ownBio` changes only after the server accepted the
// write, to the bounded text actually stored.
class ProfileBioManager : public QObject
{
    Q_OBJECT

    // The backend can read extended profile fields at all.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // ...and this homeserver answered one; false once it reported the endpoint
    // unknown.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // Bumped when any cached bio changes, so bioFor() bindings re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The account's own bio, or "".
    Q_PROPERTY(QString ownBio READ ownBio NOTIFY revisionChanged)
    // A write is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    // Client-side ceiling for the editor's counter, from rust/src/bio.rs.
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
    // Mirrors MAX_BIO_CHARS in rust/src/bio.rs. MSC4440 sets no limit and lists
    // unbounded bios as a security consideration.
    static constexpr int kMaxLength = 2048;
    int maxLength() const { return kMaxLength; }

    // "" when unknown or absent (they look the same on a card). Pure read.
    Q_INVOKABLE QString bioFor(const QString &userId) const;
    // Asks once per user per session; deduplicated.
    Q_INVOKABLE void request(const QString &userId);
    // Asks again regardless, e.g. for the account's own bio after writing it.
    Q_INVOKABLE void refresh(const QString &userId);
    // Empty or whitespace-only clears the bio.
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

    // Bounded: one entry per profile card opened this session.
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
