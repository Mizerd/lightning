#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QTemporaryDir>

#include <memory>

class ImageSendPrep
{
public:
    ImageSendPrep();
    ~ImageSendPrep();

    // `mode` is the stored setting string. `path` or `data` is replaced by what
    // must be sent; `*tempSub` receives the directory to hand to track() (empty
    // when nothing was written). False = the image must NOT be sent (removal
    // was requested and could not be done).
    bool prepare(const QString &mode, const QString &mime, QString &path,
                 QByteArray &data, QString *tempSub);
    void track(quint64 opId, const QString &sub);
    void finished(quint64 opId);
    static void discard(const QString &sub);
    void releaseAll();

private:
    struct Tracked {
        QString sub;
        QDateTime at;
    };
    void purgeOld();
    static void sweepStale();

    std::unique_ptr<QTemporaryDir> m_dir;
    QHash<quint64, Tracked> m_ops;
    quint64 m_next = 1;
    static bool s_swept;
};
