#include "media/ImageSendPrep.h"

#include "media/ImageMetadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {
constexpr auto kPrefix = "lightning-stripped-";
}

bool ImageSendPrep::s_swept = false;

ImageSendPrep::ImageSendPrep() = default;
ImageSendPrep::~ImageSendPrep() = default;

void ImageSendPrep::sweepStale()
{
    if (s_swept)
        return;
    s_swept = true;
    // A previous run killed mid-send leaves its directory behind.
    const QDir tmp(QDir::tempPath());
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &info : tmp.entryInfoList(
             {QString::fromLatin1(kPrefix) + QLatin1Char('*')},
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (info.lastModified() < cutoff)
            QDir(info.absoluteFilePath()).removeRecursively();
    }
}

void ImageSendPrep::purgeOld()
{
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-15 * 60);
    for (auto it = m_ops.begin(); it != m_ops.end();) {
        if (it->at < cutoff) {
            QDir(it->sub).removeRecursively();
            it = m_ops.erase(it);
        } else {
            ++it;
        }
    }
}

bool ImageSendPrep::prepare(const QString &modeString, const QString &mime,
                            QString &path, QByteArray &data, QString *tempSub)
{
    if (tempSub)
        tempSub->clear();
    const imagemeta::Mode mode = imagemeta::modeFromString(modeString);
    if (mode == imagemeta::Mode::Keep
        || !mime.startsWith(QLatin1String("image/")))
        return true;

    QByteArray bytes = data;
    if (!path.isEmpty()) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        bytes = file.readAll();
    }
    const imagemeta::Result r = imagemeta::strip(bytes, mode);
    switch (r.status) {
    case imagemeta::Status::Unchanged:
    case imagemeta::Status::Unsupported:
        return true;
    case imagemeta::Status::Failed:
        return false;
    case imagemeta::Status::Stripped:
        break;
    }
    if (path.isEmpty()) {
        data = r.bytes;
        return true;
    }
    sweepStale();
    if (!m_dir)
        m_dir = std::make_unique<QTemporaryDir>(
            QDir::tempPath() + QLatin1Char('/') + QString::fromLatin1(kPrefix)
            + QStringLiteral("XXXXXX"));
    if (!m_dir->isValid())
        return false;
    const QString sub = m_dir->filePath(QString::number(m_next++));
    if (!QDir().mkpath(sub))
        return false;
    const QString target = sub + QLatin1Char('/') + QFileInfo(path).fileName();
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly)
        || out.write(r.bytes) != r.bytes.size()) {
        QDir(sub).removeRecursively();
        return false;
    }
    out.close();
    path = target;
    if (tempSub)
        *tempSub = sub;
    return true;
}

void ImageSendPrep::track(quint64 opId, const QString &sub)
{
    if (sub.isEmpty())
        return;
    purgeOld();
    m_ops.insert(opId, {sub, QDateTime::currentDateTime()});
}

void ImageSendPrep::finished(quint64 opId)
{
    const auto it = m_ops.find(opId);
    if (it == m_ops.end())
        return;
    QDir(it->sub).removeRecursively();
    m_ops.erase(it);
}

void ImageSendPrep::discard(const QString &sub)
{
    if (!sub.isEmpty())
        QDir(sub).removeRecursively();
}

void ImageSendPrep::releaseAll()
{
    for (const Tracked &t : std::as_const(m_ops))
        QDir(t.sub).removeRecursively();
    m_ops.clear();
}
