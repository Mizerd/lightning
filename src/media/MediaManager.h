#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class MatrixClient;

// v0.3: coordinates media picking + upload/send. The composer bar in QML
// calls pickAndSendMedia(...) which uses a QML FileDialog for path selection
// and then forwards to the client's send{Image,File}(). Downloading is
// handled via MatrixClient::mediaDownloadUrl -> QDesktopServices::openUrl.
class MediaManager : public QObject
{
    Q_OBJECT
public:
    explicit MediaManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    // Called from QML with the path that a FileDialog produced.
    Q_INVOKABLE void sendPickedImage(const QString &roomId, const QUrl &fileUrl);
    Q_INVOKABLE void sendPickedFile(const QString &roomId, const QUrl &fileUrl);
    Q_INVOKABLE void openExternal(const QUrl &url);
    Q_INVOKABLE void openWebUrl(const QUrl &url);

Q_SIGNALS:
    void uploadProgress(const QString &localPath, qint64 sent, qint64 total);
    void uploaded(const QString &localPath, const QString &mxcUri);
    void downloaded(const QString &mxcUri, const QString &localPath);
    void failed(const QString &target, const QString &reason);

private:
    static QString localPathFromUrl(const QUrl &url);
    MatrixClient *m_client = nullptr;
};
