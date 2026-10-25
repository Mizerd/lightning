#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "privacy/AccountPrefs.h"

class MatrixClient;
class EmojiCatalog;

// Honours and exposes two standard account-data preferences (exposed as
// `app.accountPrefs`):
//
//   * MSC4278 `m.media_preview_config`: whether media starts collapsed
//     (Always / Private rooms / Off) and whether invite avatars load. The
//     account's value is the source of truth and is NOT mirrored into local
//     settings (the SDK's own store already keeps it for an offline start).
//     Until the account answers, and when it holds nothing, the MSC defaults
//     apply (on); a value this build does not know reads as off.
//   * Element's `m.recent_emoji`: merged with Lightning's own recents when it
//     arrives, and bumped (+1) when an emoji is used. Lightning never uploads
//     its local history in bulk: only a use is written.
//
// Generation-safe: a sign-out or account switch returns everything to the
// defaults and a late answer for the old session is ignored.
class AccountPrefsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AccountPrefsController is exposed via app.accountPrefs")
    // "on" | "private" | "off"
    Q_PROPERTY(QString mediaPreviews READ mediaPreviews NOTIFY changed)
    Q_PROPERTY(bool inviteAvatars READ inviteAvatars NOTIFY changed)
    // True once the account's own copy was read (or written) this session.
    Q_PROPERTY(bool mediaPreviewKnown READ mediaPreviewKnown NOTIFY changed)
    // The last write failed; the displayed value is still the account's.
    Q_PROPERTY(bool writeFailed READ writeFailed NOTIFY changed)

public:
    explicit AccountPrefsController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    void setEmojiCatalog(EmojiCatalog *emoji);

    QString mediaPreviews() const { return m_media.mediaPreviews; }
    bool inviteAvatars() const { return m_media.inviteAvatars; }
    bool mediaPreviewKnown() const { return m_mediaKnown; }
    bool writeFailed() const { return m_writeFailed; }

    // Should media in this room start collapsed? `roomIsPrivate`: encrypted
    // or a direct message (the room list carries no join rule).
    Q_INVOKABLE bool collapsesMedia(bool roomIsPrivate) const;
    // The last recents write failed (and is being retried, at most 3 times).
    bool recentsWriteFailed() const { return m_emojiWriteFailed; }
    // Optimistic: the new value shows at once and a failed write reverts it.
    Q_INVOKABLE void setMediaPreviews(const QString &mode);
    Q_INVOKABLE void setInviteAvatars(bool on);

    // (Re-)read both from the account.
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
    AccountPrefs::MediaPreview m_media;          // what the UI shows
    AccountPrefs::MediaPreview m_mediaConfirmed; // what the account holds
    QVariantMap m_mediaContent;                  // its full content
    bool m_mediaKnown = false;
    bool m_writeFailed = false;
    QList<AccountPrefs::EmojiCount> m_emojiServer;
    QVariantMap m_emojiContent;
    bool m_emojiRead = false;
    // Emoji used before the account's list arrived; bumped once it does.
    QStringList m_emojiPending;
    QTimer m_emojiFlush;
    // Bounded retry of a failed recents write (account-scoped: cleared with
    // the rest of this account's state on a client change).
    QTimer m_emojiRetry;
    int m_emojiAttempts = 0;
    bool m_emojiWriteFailed = false;
};
