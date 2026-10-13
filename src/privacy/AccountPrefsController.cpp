#include "privacy/AccountPrefsController.h"

#include "matrix/MatrixClient.h"
#include "models/EmojiCatalog.h"

using namespace AccountPrefs;

AccountPrefsController::AccountPrefsController(QObject *parent)
    : QObject(parent)
{
    // Uses within a short burst (picking several reactions) are one write.
    m_emojiFlush.setSingleShot(true);
    m_emojiFlush.setInterval(1500);
    connect(&m_emojiFlush, &QTimer::timeout, this,
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
    // The SDK's store holds the account data from the last session, so a
    // read right after sign-in answers at once; the second one, after the
    // first sync, picks up what changed while this device was away.
    connect(m_client, &MatrixClient::loginSucceeded, this,
            [this](const QString &) { reset(); refresh(); });
    connect(m_client, &MatrixClient::initialSyncDoneChanged, this, [this] {
        if (m_client && m_client->initialSyncDone())
            refresh();
    });
    connect(m_client, &MatrixClient::loggedOut, this,
            &AccountPrefsController::reset);
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
    // Back to the MSC defaults: nothing of the previous account may decide
    // what this one loads.
    m_emojiFlush.stop();
    m_media = {};
    m_mediaConfirmed = {};
    m_mediaContent.clear();
    m_mediaKnown = false;
    m_writeFailed = false;
    m_emojiServer.clear();
    m_emojiContent.clear();
    m_emojiRead = false;
    m_emojiPending.clear();
    Q_EMIT changed();
}

void AccountPrefsController::refresh()
{
    if (!m_client)
        return;
    m_client->readAccountPref(QString::fromLatin1(kMediaPreviewType));
    m_client->readAccountPref(QString::fromLatin1(kRecentEmojiType));
}

bool AccountPrefsController::collapsesMedia(bool roomIsPrivate) const
{
    return AccountPrefs::collapsesMedia(m_media.mediaPreviews, roomIsPrivate);
}

void AccountPrefsController::setMediaPreviews(const QString &mode)
{
    if (mode != QLatin1String("on") && mode != QLatin1String("private")
        && mode != QLatin1String("off"))
        return;
    MediaPreview next = m_media;
    next.mediaPreviews = mode;
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
    if (!m_client)
        return;
    m_media = next;
    m_writeFailed = false;
    Q_EMIT changed();
    m_client->writeAccountPref(
        QString::fromLatin1(kMediaPreviewType),
        composeMediaPreview(m_mediaContent, next.mediaPreviews,
                            next.inviteAvatars));
}

void AccountPrefsController::onRead(const QString &type, bool present,
                                    const QVariantMap &content)
{
    if (type == QLatin1String(kMediaPreviewType)) {
        m_mediaContent = present ? content : QVariantMap();
        m_mediaConfirmed = present ? parseMediaPreview(content) : MediaPreview();
        m_media = m_mediaConfirmed;
        m_mediaKnown = true;
        m_writeFailed = false;
        Q_EMIT changed();
        return;
    }
    if (type == QLatin1String(kRecentEmojiType)) {
        m_emojiContent = present ? content : QVariantMap();
        m_emojiServer = present ? parseRecentEmoji(content)
                                : QList<EmojiCount>();
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
    if (type != QLatin1String(kMediaPreviewType))
        return;
    if (ok) {
        m_mediaConfirmed = m_media;
        m_mediaKnown = true;
    } else {
        m_media = m_mediaConfirmed;
        m_writeFailed = true;
    }
    Q_EMIT changed();
}

void AccountPrefsController::onEmojiUsed(const QString &emoji)
{
    if (!m_client)
        return;
    if (!m_emojiRead) {
        // Never write a list built from nothing over the account's own.
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
    m_client->writeAccountPref(
        QString::fromLatin1(kRecentEmojiType),
        composeRecentEmoji(m_emojiContent, m_emojiServer));
}
