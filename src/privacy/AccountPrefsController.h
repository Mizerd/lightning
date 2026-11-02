#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "privacy/AccountPrefs.h"

class MatrixClient;
class EmojiCatalog;

class AccountPrefsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AccountPrefsController is exposed via app.accountPrefs")
    Q_PROPERTY(QString mediaPreviews READ mediaPreviews NOTIFY changed)
    Q_PROPERTY(bool inviteAvatars READ inviteAvatars NOTIFY changed)
    Q_PROPERTY(bool mediaPreviewKnown READ mediaPreviewKnown NOTIFY changed)
    Q_PROPERTY(bool privateMeansNonPublic READ privateMeansNonPublic NOTIFY changed)
    // Whether a stranger-supplied invitation avatar may be fetched NOW. Unlike
    // `inviteAvatars` (the MSC default, on, until the account answers) this
    // is fail-closed: false until the account's own copy is known, because a
    // fetch made in that window cannot be taken back once the answer is
    // "off". Every invitation identity surface reads this one.
    Q_PROPERTY(bool inviteAvatarsAllowed READ inviteAvatarsAllowed NOTIFY changed)
    Q_PROPERTY(bool writeFailed READ writeFailed NOTIFY changed)
    Q_PROPERTY(QString invitePolicyState READ invitePolicyState NOTIFY changed)
    Q_PROPERTY(bool invitesBlocked READ invitesBlocked NOTIFY changed)
    Q_PROPERTY(bool invitePolicyPending READ invitePolicyPending NOTIFY changed)
    Q_PROPERTY(bool invitePolicyWriteFailed READ invitePolicyWriteFailed NOTIFY changed)

public:
    explicit AccountPrefsController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    void setEmojiCatalog(EmojiCatalog *emoji);

    QString mediaPreviews() const { return m_media.mediaPreviews; }
    bool inviteAvatars() const { return m_media.inviteAvatars; }
    bool mediaPreviewKnown() const { return m_mediaKnown; }
    bool privateMeansNonPublic() const { return m_media.privateMeansNonPublic; }
    bool inviteAvatarsAllowed() const
    { return m_mediaKnown && m_media.inviteAvatars; }
    bool writeFailed() const { return m_writeFailed; }

    // Should media in this room start collapsed? Fail-closed: true while the
    // account's own choice is not known yet (a fetch made in that window
    // cannot be taken back). See AccountPrefs::collapsesMedia for the rest.
    Q_INVOKABLE bool collapsesMedia(const QString &joinRule, bool encrypted,
                                    bool direct) const;
    bool recentsWriteFailed() const { return m_emojiWriteFailed; }
    bool recentsRetryScheduled() const { return m_emojiRetry.isActive(); }
    int recentsWriteAttempts() const { return m_emojiAttempts; }
    void setRecentsRetryBaseMsForTest(int ms) { m_emojiRetryBaseMs = ms; }
    Q_INVOKABLE void setMediaPreviews(const QString &mode);
    Q_INVOKABLE void setInviteAvatars(bool on);
    Q_INVOKABLE void setInvitesBlocked(bool block);

    QString invitePolicyState() const;
    bool invitesBlocked() const;
    bool invitePolicyPending() const { return m_invitePending > 0; }
    bool invitePolicyWriteFailed() const { return m_inviteWriteFailed; }

    void refresh();

Q_SIGNALS:
    void changed();

private:
    void onRead(const QString &type, bool present, const QVariantMap &content);
    void onWritten(const QString &type, bool ok);
    void onEmojiUsed(const QString &emoji);
    void flushEmoji();
    void writeMedia(const AccountPrefs::MediaPreview &next);
    void reset();

    MatrixClient *m_client = nullptr;
    EmojiCatalog *m_emoji = nullptr;
    AccountPrefs::MediaPreview m_media;
    AccountPrefs::MediaPreview m_mediaConfirmed;
    QVariantMap m_mediaContent;
    bool m_mediaKnown = false;
    bool m_writeFailed = false;
    QList<AccountPrefs::EmojiCount> m_emojiServer;
    QVariantMap m_emojiContent;
    bool m_emojiRead = false;
    QStringList m_emojiPending;
    QTimer m_emojiFlush;
    QTimer m_emojiRetry;
    int m_emojiAttempts = 0;
    int m_emojiRetryBaseMs = 5000;
    bool m_emojiWriteFailed = false;

    bool m_inviteSupportKnown = false;
    bool m_inviteSupportOk = false;
    bool m_inviteStable = false;
    bool m_inviteUnstable = false;
    bool m_inviteStableRead = false;
    bool m_inviteUnstableRead = false;
    QVariantMap m_inviteStableContent;
    QVariantMap m_inviteUnstableContent;
    int m_invitePending = 0;
    bool m_inviteWriteFailed = false;
    bool m_inviteReadFailed = false;
    QVariantMap m_mediaInFlight;
    QVariantMap m_mediaHeld;
    bool m_mediaHolding = false;
    int m_mediaHeldMisses = 0;
    QHash<QString, QVariantMap> m_inviteInFlight;
    QHash<QString, QVariantMap> m_inviteHeld;
    QHash<QString, int> m_inviteHeldMisses;
    static constexpr int kMaxHeldMisses = 3;
    void applyInviteRead(const QString &type, const QVariantMap &content);
    void setInviteContent(const QString &type, const QVariantMap &content);
};
