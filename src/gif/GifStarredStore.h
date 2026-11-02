#pragma once

#include "gif/GifResponseParser.h"
#include "gif/GifStarredModel.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

class QSettings;

class GifStarredStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY countChanged)
    Q_PROPERTY(GifStarredModel *model READ model CONSTANT)

public:
    explicit GifStarredStore(QObject *parent = nullptr);
    // Out-of-line so std::unique_ptr<QSettings> is destroyed where QSettings is
    // complete (moc may instantiate the destructor elsewhere).
    ~GifStarredStore() override;

    // Picker model, role-compatible with GifResultModel. Never recreated;
    // openFor() repoints it, so GifSavedModel keeps a stable pointer across
    // account switches.
    GifStarredModel *model() const { return m_model.get(); }

    void openFor(const QString &accountDir);
    void close();
    bool isOpen() const { return !m_dir.isEmpty(); }
    QString currentDirectory() const { return m_dir; }

    void starBytes(const QString &mediaKey, const QByteArray &bytes);
    void reportFetchFailed(const QString &mediaKey, const QString &category)
    { Q_EMIT starFinished(mediaKey, false, category, categoryMessage(category)); }

    Q_INVOKABLE void unstar(const QString &hash);
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void unstarByMediaKey(const QString &mediaKey);
    Q_INVOKABLE bool isStarredThisSession(const QString &mediaKey) const
    { return m_mediaKeyToHash.contains(mediaKey); }
    Q_INVOKABLE bool hasHash(const QString &hash) const
    { return isOpen() && isSafeHashHex(hash) && m_model->hasHash(hash); }

    QByteArray readBytes(const QString &hash) const;

    Q_INVOKABLE QString source(const QString &hash) const;
    Q_INVOKABLE QString sourceExt(const QString &hash) const;

    qint64 totalBytes() const { return m_model->totalBytes(); }
    int count() const { return m_model->count(); }

    static constexpr int kMaxItems = GifStarredModel::kMaxStarred;
    static constexpr qint64 kMaxTotalBytes = 64LL * 1024 * 1024;

    void setCapsForTest(int maxItems, qint64 maxTotalBytes)
    { m_maxItems = maxItems; m_maxTotalBytes = maxTotalBytes; }

Q_SIGNALS:
    void starFinished(const QString &mediaKey, bool ok,
                      const QString &category, const QString &message);
    void unstarFinished(const QString &hash);
    void countChanged();

private:
    QString filePath(const QString &hash, const QString &ext = QString()) const;
    QString extForHash(const QString &hash) const;
    static bool isSafeHashHex(const QString &hash);
    bool ensureDirectory() const;
    QString categoryMessage(const QString &category) const;

    QString m_dir;
    int m_maxItems = kMaxItems;
    qint64 m_maxTotalBytes = kMaxTotalBytes;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<GifStarredModel> m_model;
    // Session-only mediaKey -> hash; never persisted or logged. Cleared on
    // openFor()/close().
    QHash<QString, QString> m_mediaKeyToHash;
};
