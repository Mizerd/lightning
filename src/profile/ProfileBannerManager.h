#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

// Profile banners (MSC4427 over MSC4133 extended profile fields).
//
// The policy half of the feature; the protocol half is rust/src/banner.rs.
// Interoperability is the whole point: the stable field is `m.banner_url` and
// the deployed one is `chat.commet.profile_banner`, and Lightning reads both
// and writes both, so a banner set in Commet, Sable or Haven shows up here and
// one set here shows up there.
//
// Honesty rules, the same ones presence follows:
//   * a user with no banner and a user we have not asked about are both
//     rendered as NOTHING;
//   * a homeserver that does not implement extended profile fields is
//     `supported == false`, which is a different fact from "no banner" and
//     is what hides the whole editing surface rather than offering a control
//     that cannot work;
//   * only an mxc:// URI is ever accepted (enforced in Rust as well), because
//     a profile field is remote text and an http URL in one would make every
//     viewer who opens a profile card fetch it from a host its owner controls.
class ProfileBannerManager : public QObject
{
    Q_OBJECT

    // The backend can read extended profile fields at all.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // ...and this homeserver actually answered one. False once the server has
    // told us it does not know the endpoint.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // Bumped whenever any cached banner changes; QML bindings read it to
    // re-evaluate bannerFor().
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The local account's own banner, or "" — the Settings card's model.
    Q_PROPERTY(QString ownBanner READ ownBanner NOTIFY revisionChanged)
    // A set/clear is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit ProfileBannerManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool supported() const { return m_supported; }
    int revision() const { return m_revision; }
    bool busy() const { return m_pendingWrite != 0; }
    QString lastError() const { return m_lastError; }
    QString ownBanner() const;

    // "" when unknown or absent. Pure read, safe in a binding; re-read on
    // revisionChanged.
    Q_INVOKABLE QString bannerFor(const QString &userId) const;
    // Asks once per user per session. Idempotent and deduplicated: a profile
    // popover opening twice must not cost two requests.
    Q_INVOKABLE void request(const QString &userId);
    // A local file. Uploaded and written to BOTH field names.
    Q_INVOKABLE void setOwnBanner(const QString &localPath);
    Q_INVOKABLE void clearOwnBanner();

Q_SIGNALS:
    void availableChanged();
    void supportedChanged();
    void revisionChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void handleReceived(quint64 opId, const QString &userId, const QString &mxc,
                        bool supported);
    void handleSet(quint64 opId, bool ok, const QString &mxc,
                   const QString &category);
    void clearSession();
    void setLastError(const QString &error);

    // Bounded: one entry per profile card anyone has opened this session.
    static constexpr int kMaxCached = 256;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_cache;   // userId -> mxc ("" = asked, none)
    QSet<QString> m_asked;
    QHash<quint64, QString> m_inFlight;
    quint64 m_nextOpId = 1;
    quint64 m_pendingWrite = 0;
    int m_revision = 0;
    bool m_supported = true;
    QString m_lastError;
};
