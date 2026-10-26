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
    // "Private rooms only" has been chosen under its non-public meaning.
    Q_PROPERTY(bool privateMeansNonPublic READ privateMeansNonPublic NOTIFY changed)
    // Whether a stranger-supplied invitation avatar may be fetched NOW. Unlike
    // `inviteAvatars` (the MSC default, on, until the account answers) this
    // is fail-closed: false until the account's own copy is known, because a
    // fetch made in that window cannot be taken back once the answer is
    // "off". Every invitation identity surface reads this one.
    Q_PROPERTY(bool inviteAvatarsAllowed READ inviteAvatarsAllowed NOTIFY changed)
    // The last write failed; the displayed value is still the account's.
    Q_PROPERTY(bool writeFailed READ writeFailed NOTIFY changed)
    // Incoming-invite policy (spec v1.18 m.invite_permission_config, or
    // MSC4380): "unknown" until the server and the account have answered,
    // "offline" when /versions could not be read, "unsupported" when the
    // server implements neither form (the setting is then NOT offered: a
    // switch that only hid invitations would claim a rejection the server
    // never performs), "ready" otherwise.
    Q_PROPERTY(QString invitePolicyState READ invitePolicyState NOTIFY changed)
    // What the ACCOUNT holds now (either form blocking counts), never the
    // optimistic value of a write in flight.
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
    // The last recents write failed (and is being retried, at most 3 times).
    bool recentsWriteFailed() const { return m_emojiWriteFailed; }
    // Test seams for the bounded retry: whether a retry is scheduled, how
    // many writes this burst has made, and a shorter base delay (the delay is
    // base x attempt number, 5000 ms in production).
    bool recentsRetryScheduled() const { return m_emojiRetry.isActive(); }
    int recentsWriteAttempts() const { return m_emojiAttempts; }
    void setRecentsRetryBaseMsForTest(int ms) { m_emojiRetryBaseMs = ms; }
    // Optimistic: the new value shows at once and a failed write reverts it.
    Q_INVOKABLE void setMediaPreviews(const QString &mode);
    Q_INVOKABLE void setInviteAvatars(bool on);
    // Block (true) or allow (false) incoming invitations, server-side. Only
    // when invitePolicyState is "ready" and no write is pending.
    Q_INVOKABLE void setInvitesBlocked(bool block);

    QString invitePolicyState() const;
    bool invitesBlocked() const;
    bool invitePolicyPending() const { return m_invitePending > 0; }
    bool invitePolicyWriteFailed() const { return m_inviteWriteFailed; }

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
    int m_emojiRetryBaseMs = 5000;
    bool m_emojiWriteFailed = false;

    // Incoming-invite policy.
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
    // A server read of a policy type failed: its value is unknown, so the
    // switch is not offered ("offline") until a read answers.
    bool m_inviteReadFailed = false;
    // type -> content of a write in flight, and of a confirmed write still
    // waiting for a server read that agrees with it. A read requested before
    // the write landed can arrive after it; while held, a disagreeing read is
    // not adopted (the confirmed write is what the account holds) and is
    // asked again, at most kMaxHeldMisses times before the server wins.
    QHash<QString, QVariantMap> m_inviteInFlight;
    QHash<QString, QVariantMap> m_inviteHeld;
    QHash<QString, int> m_inviteHeldMisses;
    static constexpr int kMaxHeldMisses = 3;
    void applyInviteRead(const QString &type, const QVariantMap &content);
    void setInviteContent(const QString &type, const QVariantMap &content);
};
