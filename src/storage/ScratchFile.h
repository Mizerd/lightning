#pragma once

// QSaveFile for files that only live as long as the session.
//
// QSaveFile commits with an fsync (fdatasync on Linux): right for a file that
// must survive a power cut, pure cost for the decrypted media, animations and
// GIF previews MediaBridge, PlayableFileWriter and GifPreviewCache write into a
// session scratch directory that is deleted at exit. The fsync forces every
// byte to the disk at once and makes the writing thread (the GUI thread for
// two of them) wait for it; without it a short-lived file is often deleted
// before the kernel writes it back at all, so decrypted bytes reach the disk
// less often too.
//
// What stays the same as QSaveFile: the bytes go to a temporary beside the
// target and are renamed over it only when complete, so a reader never sees a
// partial file under the real name; the temporary is owner-only before the
// first byte; a write that is cancelled, fails or is destroyed uncommitted
// leaves nothing behind. Header-only on purpose: several test targets compile
// these writers' sources one by one.

#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QString>

#include <filesystem>
#include <system_error>

namespace lightning::scratch {

class ScratchFile
{
public:
    explicit ScratchFile(const QString &path)
        : m_path(path)
        // Unique, like QSaveFile's temporary, so two writers of one path can
        // never write into the same temporary.
        , m_part(path + QStringLiteral(".part-")
                 + QString::number(QRandomGenerator::global()->generate64(), 36))
    {
    }
    ~ScratchFile() { cancelWriting(); }
    ScratchFile(const ScratchFile &) = delete;
    ScratchFile &operator=(const ScratchFile &) = delete;

    bool open()
    {
        m_file.setFileName(m_part);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return false;
        m_opened = true;
        // Owner-only before any byte is written; the scratch directory's 0700
        // is the first layer, this the second.
        m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return true;
    }

    qint64 write(const char *data, qint64 size) { return m_file.write(data, size); }
    qint64 write(const QByteArray &bytes) { return m_file.write(bytes); }

    // Publishes the file under its real name, replacing any file there. No
    // fsync: see the top of this file.
    bool commit()
    {
        if (!m_opened || !m_file.isOpen())
            return false;
        const bool flushed = m_file.flush(); // Qt's buffer to the kernel only
        m_file.close();
        if (!flushed) {
            cancelWriting();
            return false;
        }
        std::error_code error;
        // Replaces an existing target, as QSaveFile does (rename(2) on POSIX,
        // MoveFileEx with REPLACE_EXISTING on Windows).
        std::filesystem::rename(QFileInfo(m_part).filesystemAbsoluteFilePath(),
                                QFileInfo(m_path).filesystemAbsoluteFilePath(),
                                error);
        if (error) {
            cancelWriting();
            return false;
        }
        m_opened = false;
        return true;
    }

    // Discards the temporary; the target is untouched.
    void cancelWriting()
    {
        if (m_file.isOpen())
            m_file.close();
        if (m_opened) {
            QFile::remove(m_part);
            m_opened = false;
        }
    }

private:
    QString m_path;
    QString m_part;
    QFile m_file;
    bool m_opened = false;
};

} // namespace lightning::scratch
