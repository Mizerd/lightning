#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QTemporaryDir>

#include <memory>

// Applies the photo-metadata policy (media/ImageMetadata.h) to one attachment
// on its way out, and owns the lifetime of the stripped copies it writes.
// Shared by the room composer and the thread controller so no send path can
// skip it.
//
// The user's own file is never modified: a changed copy is written under a
// private temp directory, under the original file name, and removed when the
// send op resolves. Copies are also removed when the owner is destroyed, on
// releaseAll() (client change / sign-out), after 15 minutes if an op's result
// never arrives, and a directory abandoned by a crashed run is swept at the
// next use.
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
    // The copy in `sub` belongs to `opId` until finished(opId).
    void track(quint64 opId, const QString &sub);
    void finished(quint64 opId);
    // The op never started: remove the copy now.
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
