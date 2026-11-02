#include "privacy/AccountPrefsController.h"

#include "matrix/MatrixClient.h"
#include "models/EmojiCatalog.h"

#include <algorithm>

using namespace AccountPrefs;

AccountPrefsController::AccountPrefsController(QObject *parent)
    : QObject(parent)
{
    m_emojiFlush.setSingleShot(true);
    m_emojiFlush.setInterval(1500);
    connect(&m_emojiFlush, &QTimer::timeout, this, [this] {
        m_emojiAttempts = 0;
        flushEmoji();
    });
    m_emojiRetry.setSingleShot(true);
    connect(&m_emojiRetry, &QTimer::timeout, this,
            &AccountPrefsController::flushEmoji);
}

void AccountPrefsController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    reset();
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::accountPrefRead, this,
            &AccountPrefsController::onRead);
    connect(m_client, &MatrixClient::accountPrefChanged, this,
            [this](const QString &type) {
        if (m_client)
            m_client->readAccountPref(type);
    });
    connect(m_client, &MatrixClient::accountPrefWritten, this,
            [this](const QString &type, bool ok, const QString &) {
        onWritten(type, ok);
    });
    connect(m_client, &MatrixClient::accountPrefReadFailed, this,
            [this](const QString &type, const QString &) {
        if (type == QLatin1String(kMediaPreviewType)) {
            return;
        }
        if (type == QLatin1String(kInvitePermissionType))
            m_inviteStableRead = false;
        else if (type == QLatin1String(kInvitePermissionUnstableType))
            m_inviteUnstableRead = false;
        else
            return;
        m_inviteReadFailed = true;
        Q_EMIT changed();
    });
    connect(m_client, &MatrixClient::invitePolicySupportReceived, this,
            [this](bool ok, bool stable, bool unstable) {
        m_inviteSupportKnown = true;
        m_inviteSupportOk = ok;
        m_inviteStable = ok && stable;
        m_inviteUnstable = ok && unstable;
        Q_EMIT changed();
    });
    connect(m_client, &MatrixClient::loginSucceeded, this,
            [this](const QString &) { reset(); refresh(); });
    connect(m_client, &MatrixClient::initialSyncDoneChanged, this, [this] {
        if (m_client && m_client->initialSyncDone())
            refresh();
    });
    connect(m_client, &MatrixClient::loggedOut, this,
            &AccountPrefsController::reset);
    // No account data on this backend (mock, HTTP fallback): there is no
    // stored choice to wait for, so the fail-closed gates open on the
    // defaults now rather than hiding every picture until a read that the
    // backend answers only after sign-in.
    if (!m_client->supportsAccountPrefs()) {
        m_mediaKnown = true;
        Q_EMIT changed();
    }
}

void AccountPrefsController::setEmojiCatalog(EmojiCatalog *emoji)
{
    if (m_emoji == emoji)
        return;
    if (m_emoji)
        disconnect(m_emoji, nullptr, this, nullptr);
    m_emoji = emoji;
    if (m_emoji)
        connect(m_emoji, &EmojiCatalog::recentEmojiUsed, this,
                &AccountPrefsController::onEmojiUsed);
}

void AccountPrefsController::reset()
{
    m_emojiFlush.stop();
    m_emojiRetry.stop();
    m_emojiAttempts = 0;
    m_emojiWriteFailed = false;
    m_media = {};
    m_mediaConfirmed = {};
    m_mediaContent.clear();
    m_mediaKnown = false;
    m_writeFailed = false;
    m_emojiServer.clear();
    m_emojiContent.clear();
    m_emojiRead = false;
    m_emojiPending.clear();
    m_inviteSupportKnown = false;
    m_inviteSupportOk = false;
    m_inviteStable = false;
    m_inviteUnstable = false;
    m_inviteStableRead = false;
    m_inviteUnstableRead = false;
    m_inviteStableContent.clear();
    m_inviteUnstableContent.clear();
    m_invitePending = 0;
    m_inviteWriteFailed = false;
    m_inviteReadFailed = false;
    m_mediaInFlight.clear();
    m_mediaHeld.clear();
    m_mediaHolding = false;
    m_mediaHeldMisses = 0;
    m_inviteInFlight.clear();
    m_inviteHeld.clear();
    m_inviteHeldMisses.clear();
    Q_EMIT changed();
}

void AccountPrefsController::refresh()
{
    if (!m_client)
        return;
    m_client->readAccountPref(QString::fromLatin1(kMediaPreviewType));
    m_client->readAccountPref(QString::fromLatin1(kRecentEmojiType));
    m_client->readAccountPref(QString::fromLatin1(kInvitePermissionType));
    m_client->readAccountPref(QString::fromLatin1(kInvitePermissionUnstableType));
    m_client->requestInvitePolicySupport();
}

QString AccountPrefsController::invitePolicyState() const
{
    if (!m_inviteSupportKnown)
        return QStringLiteral("unknown");
    if (!m_inviteSupportOk)
        return QStringLiteral("offline");
    if (!m_inviteStable && !m_inviteUnstable)
        return QStringLiteral("unsupported");
    // A write composed before both forms were read could drop a field or
    // leave a stale block behind. A failed read is "offline": its value is
    // unknown, not absent.
    if (!m_inviteStableRead || !m_inviteUnstableRead)
        return m_inviteReadFailed ? QStringLiteral("offline")
                                  : QStringLiteral("unknown");
    return QStringLiteral("ready");
}

bool AccountPrefsController::invitesBlocked() const
{
    return stableInvitePolicyBlocks(m_inviteStableContent)
           || unstableInvitePolicyBlocks(m_inviteUnstableContent);
}

void AccountPrefsController::setInvitesBlocked(bool block)
{
    if (!m_client || invitePolicyState() != QLatin1String("ready")
        || m_invitePending > 0)
        return;
    const QList<AccountDataWrite> writes = composeInvitePolicy(
        m_inviteStable, m_inviteUnstable, m_inviteStableContent,
        m_inviteUnstableContent, block);
    if (writes.isEmpty())
        return;
    m_inviteWriteFailed = false;
    m_invitePending = int(writes.size());
    for (const AccountDataWrite &w : writes)
        m_inviteInFlight.insert(w.type, w.content);
    Q_EMIT changed();
    for (const AccountDataWrite &w : writes)
        m_client->writeAccountPref(w.type, w.content);
}

void AccountPrefsController::setInviteContent(const QString &type,
                                              const QVariantMap &content)
{
    if (type == QLatin1String(kInvitePermissionType)) {
        m_inviteStableContent = content;
        m_inviteStableRead = true;
    } else {
        m_inviteUnstableContent = content;
        m_inviteUnstableRead = true;
    }
    if (m_inviteStableRead && m_inviteUnstableRead)
        m_inviteReadFailed = false;
}

void AccountPrefsController::applyInviteRead(const QString &type,
                                             const QVariantMap &content)
{
    if (m_inviteHeld.contains(type)) {
        if (content == m_inviteHeld.value(type)) {
            m_inviteHeld.remove(type);
            m_inviteHeldMisses.remove(type);
        } else if (++m_inviteHeldMisses[type] <= kMaxHeldMisses) {
            if (m_client)
                m_client->readAccountPref(type);
            return;
        } else {
            m_inviteHeld.remove(type);
            m_inviteHeldMisses.remove(type);
        }
    }
    setInviteContent(type, content);
}

bool AccountPrefsController::collapsesMedia(const QString &joinRule, bool encrypted,
                                            bool direct) const
{
    if (!m_mediaKnown)
        return true;
    return AccountPrefs::collapsesMedia(m_media.mediaPreviews, joinRule, encrypted,
                                        direct, m_media.privateMeansNonPublic);
}

void AccountPrefsController::setMediaPreviews(const QString &mode)
{
    if (mode != QLatin1String("on") && mode != QLatin1String("private")
        && mode != QLatin1String("off"))
        return;
    MediaPreview next = m_media;
    next.mediaPreviews = mode;
    if (mode == QLatin1String("private"))
        next.privateMeansNonPublic = true;
    writeMedia(next);
}

void AccountPrefsController::setInviteAvatars(bool on)
{
    MediaPreview next = m_media;
    next.inviteAvatars = on;
    writeMedia(next);
}

void AccountPrefsController::writeMedia(const MediaPreview &next)
{
    // Never compose a write before the account's own copy was read: it would
    // be built from nothing and drop the fields this build does not own.
    if (!m_client || !m_mediaKnown)
        return;
    m_media = next;
    m_writeFailed = false;
    m_mediaInFlight = composeMediaPreview(m_mediaContent, next.mediaPreviews,
                                          next.inviteAvatars,
                                          next.privateMeansNonPublic);
    Q_EMIT changed();
    m_client->writeAccountPref(QString::fromLatin1(kMediaPreviewType),
                               m_mediaInFlight);
}

void AccountPrefsController::onRead(const QString &type, bool present,
                                    const QVariantMap &content)
{
    if (type == QLatin1String(kInvitePermissionType)
        || type == QLatin1String(kInvitePermissionUnstableType)) {
        applyInviteRead(type, present ? content : QVariantMap());
        Q_EMIT changed();
        return;
    }
    if (type == QLatin1String(kMediaPreviewType)) {
        const QVariantMap answer = present ? content : QVariantMap();
        if (m_mediaHolding) {
            if (answer == m_mediaHeld) {
                m_mediaHolding = false;
                m_mediaHeldMisses = 0;
            } else if (++m_mediaHeldMisses <= kMaxHeldMisses) {
                if (m_client)
                    m_client->readAccountPref(type);
                return;
            } else {
                m_mediaHolding = false;
                m_mediaHeldMisses = 0;
            }
        }
        m_mediaContent = answer;
        m_mediaConfirmed = present ? parseMediaPreview(content) : MediaPreview();
        m_media = m_mediaConfirmed;
        m_mediaKnown = true;
        m_writeFailed = false;
        Q_EMIT changed();
        return;
    }
    if (type == QLatin1String(kRecentEmojiType)) {
        m_emojiContent = present ? content : QVariantMap();
        QList<EmojiCount> incoming = present ? parseRecentEmoji(content)
                                             : QList<EmojiCount>();
        if (m_emojiFlush.isActive() || m_emojiRetry.isActive()) {
            QList<EmojiCount> merged = m_emojiServer;
            for (const EmojiCount &theirs : std::as_const(incoming)) {
                bool found = false;
                for (EmojiCount &mine : merged) {
                    if (mine.emoji == theirs.emoji) {
                        mine.total = std::max(mine.total, theirs.total);
                        found = true;
                        break;
                    }
                }
                if (!found && merged.size() < kRecentEmojiServerLimit)
                    merged.append(theirs);
            }
            incoming = merged;
        }
        m_emojiServer = incoming;
        const bool firstRead = !m_emojiRead;
        m_emojiRead = true;
        if (m_emoji) {
            m_emoji->adoptRecents(mergeRecentEmoji(
                m_emoji->recentEmoji(), m_emojiServer, 100));
        }
        if (firstRead && !m_emojiPending.isEmpty()) {
            for (const QString &e : std::as_const(m_emojiPending))
                m_emojiServer = bumpRecentEmoji(m_emojiServer, e);
            m_emojiPending.clear();
            m_emojiFlush.start();
        }
    }
}

void AccountPrefsController::onWritten(const QString &type, bool ok)
{
    if (type == QLatin1String(kRecentEmojiType)) {
        if (ok) {
            m_emojiAttempts = 0;
            m_emojiRetry.stop();
            m_emojiWriteFailed = false;
        } else {
            m_emojiWriteFailed = true;
            if (m_emojiAttempts < 3) {
                m_emojiRetry.setInterval(m_emojiRetryBaseMs * m_emojiAttempts);
                m_emojiRetry.start();
            }
        }
        return;
    }
    if (type == QLatin1String(kInvitePermissionType)
        || type == QLatin1String(kInvitePermissionUnstableType)) {
        if (m_invitePending > 0)
            --m_invitePending;
        const QVariantMap written = m_inviteInFlight.take(type);
        if (ok) {
            setInviteContent(type, written);
            m_inviteHeld.insert(type, written);
            m_inviteHeldMisses.remove(type);
        } else {
            m_inviteWriteFailed = true;
        }
        if (m_client)
            m_client->readAccountPref(type);
        Q_EMIT changed();
        return;
    }
    if (type != QLatin1String(kMediaPreviewType))
        return;
    if (ok) {
        m_mediaConfirmed = m_media;
        m_mediaContent = m_mediaInFlight;
        m_mediaKnown = true;
        m_mediaHeld = m_mediaInFlight;
        m_mediaHolding = true;
        m_mediaHeldMisses = 0;
    } else {
        m_media = m_mediaConfirmed;
        m_writeFailed = true;
    }
    m_mediaInFlight.clear();
    if (m_client)
        m_client->readAccountPref(type);
    Q_EMIT changed();
}

void AccountPrefsController::onEmojiUsed(const QString &emoji)
{
    if (!m_client)
        return;
    if (!m_emojiRead) {
        if (m_emojiPending.size() < 32)
            m_emojiPending.append(emoji);
        m_client->readAccountPref(QString::fromLatin1(kRecentEmojiType));
        return;
    }
    m_emojiServer = bumpRecentEmoji(m_emojiServer, emoji);
    m_emojiFlush.start();
}

void AccountPrefsController::flushEmoji()
{
    if (!m_client || !m_emojiRead)
        return;
    ++m_emojiAttempts;
    m_client->writeAccountPref(
        QString::fromLatin1(kRecentEmojiType),
        composeRecentEmoji(m_emojiContent, m_emojiServer));
}
