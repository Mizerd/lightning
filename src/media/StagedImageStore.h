#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

class StagedImageStore : public QObject
{
    Q_OBJECT

public:
    explicit StagedImageStore(QObject *parent = nullptr);

    // Registers `bytes` and returns an opaque token, or "" when full or empty.
    // Tokens are never reused, so a stale binding can only miss.
    QString add(const QByteArray &bytes);
    void remove(const QString &token);
    void clear();

    QByteArray bytes(const QString &token) const;

    int count() const;

    static constexpr int kMaxEntries = 64;

private:
    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_entries;
    quint64 m_nextToken = 1;
};
