#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

// Profile banners (MSC4427 over MSC4133 extended profile fields). The protocol
// half is rust/src/banner.rs. Reads and writes both `m.banner_url` and the
// deployed `chat.commet.profile_banner`, for interoperability with Commet,
// Sable and Haven.
//
//   * No banner and not-yet-asked both render as nothing.
//   * A server without extended profile fields is `supported == false`, which
//     hides the editor.
//   * Only mxc:// URIs are accepted (also enforced in Rust): an http URL in a
//     remote field would make every viewer fetch from its owner's host.
class ProfileBannerManager : public QObject
{
    Q_OBJECT

    // The backend can read extended profile fields at all.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // ...and this homeserver answered one; false once it reported the endpoint
    // unknown.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // Bumped when any cached banner changes, so bannerFor() bindings
    // re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The account's own banner, or "".
    Q_PROPERTY(QString ownBanner READ ownBanner NOTIFY revisionChanged)
    // A set/clear is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    // The backend can carry a room/Space banner (a custom state event).
    Q_PROPERTY(bool roomBannersAvailable READ roomBannersAvailable
                   NOTIFY availableChanged)

public:
    explicit ProfileBannerManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool supported() const { return m_supported; }
    int revision() const { return m_revision; }
    bool busy() const { return m_pendingWrite != 0; }
    QString lastError() const { return m_lastError; }
    QString ownBanner() const;
    bool roomBannersAvailable() const;

    // "" when unknown or absent. Pure read.
    Q_INVOKABLE QString bannerFor(const QString &userId) const;
    // Asks once per user per session; deduplicated.
    Q_INVOKABLE void request(const QString &userId);
    // A local path or file:// URL; converted here so callers cannot break
    // Windows paths.
    Q_INVOKABLE void setOwnBanner(const QString &pathOrUrl);
    Q_INVOKABLE void clearOwnBanner();

    // --- Room / Space banners -------------------------------------------
    // Same rules as profile banners: "" means none or not asked, asked once per
    // room per session, never optimistic. Covered by `revision`.
    Q_INVOKABLE QString roomBannerFor(const QString &roomId) const;
    // Whether this account may change the room's banner; false until asked.
    Q_INVOKABLE bool canSetRoomBanner(const QString &roomId) const;
    Q_INVOKABLE void requestRoom(const QString &roomId);
    // Re-asks regardless, e.g. after a permission change or our own write.
    Q_INVOKABLE void refreshRoom(const QString &roomId);
    Q_INVOKABLE void setRoomBanner(const QString &roomId,
                                   const QString &pathOrUrl);
    Q_INVOKABLE void clearRoomBanner(const QString &roomId);

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
    void handleRoomReceived(quint64 opId, const QString &roomId,
                            const QString &mxc, bool canSet);
    void handleRoomSet(quint64 opId, const QString &roomId, bool ok,
                       const QString &mxc, const QString &category);
    void clearSession();
    void setLastError(const QString &error);

    // Bounded: one entry per profile card opened this session.
    static constexpr int kMaxCached = 256;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_cache;   // userId -> mxc ("" = asked, none)
    QSet<QString> m_asked;
    QHash<quint64, QString> m_inFlight;
    QHash<QString, QString> m_roomCache;    // roomId -> mxc ("" = none)
    QSet<QString> m_roomAsked;
    QSet<QString> m_roomWritable;
    QHash<quint64, QString> m_roomInFlight;
    quint64 m_nextOpId = 1;
    quint64 m_pendingWrite = 0;
    int m_revision = 0;
    bool m_supported = true;
    QString m_lastError;
};
