#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

class LinkPreviewController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool autoLoadUnencrypted READ autoLoadUnencrypted
                   WRITE setAutoLoadUnencrypted NOTIFY policyChanged)
    Q_PROPERTY(bool allowEncrypted READ allowEncrypted
                   WRITE setAllowEncrypted NOTIFY policyChanged)
    Q_PROPERTY(bool inlineMedia READ inlineMedia WRITE setInlineMedia
                   NOTIFY inlineMediaChanged)
    Q_PROPERTY(qint64 linkImageMaxBytes READ linkImageMaxBytes CONSTANT)
    Q_PROPERTY(qint64 linkVideoMaxBytes READ linkVideoMaxBytes CONSTANT)

public:
    explicit LinkPreviewController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    bool supported() const;

    bool autoLoadUnencrypted() const { return m_autoLoadUnencrypted; }
    void setAutoLoadUnencrypted(bool value);
    bool allowEncrypted() const { return m_allowEncrypted; }
    void setAllowEncrypted(bool value);
    bool inlineMedia() const { return m_inlineMedia; }
    void setInlineMedia(bool value);
    static constexpr qint64 kLinkImageMaxBytes = 25 * 1024 * 1024;
    static constexpr qint64 kLinkVideoMaxBytes = 100 * 1024 * 1024;
    qint64 linkImageMaxBytes() const { return kLinkImageMaxBytes; }
    qint64 linkVideoMaxBytes() const { return kLinkVideoMaxBytes; }

    // MediaBridge key for a link's own media: "link:" and the first 40 hex of
    // SHA-256 over the URL, so no URL reaches a key, a log tag or a file name.
    static QString mediaKeyForUrl(const QString &url);

    QVariantMap resolveLinkMedia(const QString &mediaKey) const;
    Q_INVOKABLE QVariantMap viewerEntry(const QString &mediaKey) const;
    Q_INVOKABLE bool linkMediaAvailable(const QString &mediaKey) const;

    Q_INVOKABLE QVariantMap previewFor(const QString &itemKey,
                                       const QString &body,
                                       bool roomEncrypted);
    Q_INVOKABLE QVariantMap previewForEvent(const QString &roomId,
                                            const QString &stableEventId,
                                            const QString &body,
                                            bool roomEncrypted);
    Q_INVOKABLE void requestPreview(const QString &itemKey);
    Q_INVOKABLE void requestPreviewForEvent(const QString &roomId,
                                            const QString &stableEventId);
    Q_INVOKABLE void retry(const QString &itemKey);
    Q_INVOKABLE void retryForEvent(const QString &roomId,
                                   const QString &stableEventId);

    Q_INVOKABLE void dismissPreview(const QString &itemKey);
    Q_INVOKABLE void dismissPreviewForEvent(const QString &roomId,
                                            const QString &stableEventId);
    Q_INVOKABLE void restorePreview(const QString &itemKey);
    Q_INVOKABLE void restorePreviewForEvent(const QString &roomId,
                                            const QString &stableEventId);
    Q_INVOKABLE bool isPreviewDismissed(const QString &itemKey) const;
    Q_INVOKABLE void clear();

    static constexpr int kMaxDismissed = 4096;
    Q_INVOKABLE QString linkifiedBody(const QString &body) const;
    Q_INVOKABLE QString linkifiedTopic(const QString &topic) const;

    void setUrlCacheLimit(int limit) { m_urlCacheLimit = limit; }
    int cachedUrlCount() const { return m_urls.size(); }
    int dismissedCount() const { return int(m_dismissed.size()); }
    void setHeldBytesBudget(qint64 bytes) { m_heldBytesBudget = bytes; }
    qint64 heldBytes() const { return m_heldBytes; }
    int mediaKeyCount() const { return int(m_mediaKeys.size()); }

    static constexpr qint64 kHeldBytesBudget = 64 * 1024 * 1024;

Q_SIGNALS:
    void supportedChanged();
    void policyChanged();
    void inlineMediaChanged();
    void previewChanged(const QString &itemKey);

private Q_SLOTS:
    void onPreviewFinished(quint64 opId, bool ok, const QVariantMap &fields,
                           const QString &category, int httpStatus = 0,
                           int redirectCount = 0);
    void onLoggedOut();

private:
    struct UrlEntry {
        QString state = QStringLiteral("loading");
        QString category;
        QVariantMap fields;
        qint64 heldBytes = 0;
    };
    struct ItemEntry {
        QString url;
        bool encrypted = false;
        bool consented = false;
    };

    void dispatch(const QString &url);
    static QString ownershipKey(const QString &roomId,
                                const QString &stableEventId);
    void evictIfNeeded(const QString &keep = {});
    void dropUrl(const QString &url);
    QVariantMap stateFor(const ItemEntry &item) const;
    void forgetDismissal(const QString &itemKey);

    MatrixClient *m_client = nullptr;
    // Fail closed: a fetch exposes the reader's IP to a sender-chosen host, so
    // without a policy nothing loads. AppController pushes the real setting.
    bool m_autoLoadUnencrypted = false;
    bool m_allowEncrypted = false;
    bool m_inlineMedia = true;
    qint64 m_heldBytes = 0;
    qint64 m_heldBytesBudget = kHeldBytesBudget;
    QHash<QString, QString> m_mediaKeys;

    QHash<QString, ItemEntry> m_items;
    QHash<QString, UrlEntry> m_urls;
    QList<QString> m_urlOrder;
    QHash<quint64, QString> m_inflight;
    QHash<QString, QStringList> m_urlItems;

    QSet<QString> m_dismissed;
    QList<QString> m_dismissedOrder;

    int m_urlCacheLimit = 256;
    static constexpr int kMaxTrackedItems = 4096;
};
