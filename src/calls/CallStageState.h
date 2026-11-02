#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class CallShareModel;

class CallStageState : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallStageState is exposed via app.groupCall.stageState")

    Q_PROPERTY(QString pinnedIdentity READ pinnedIdentity
                   NOTIFY pinnedIdentityChanged)
    Q_PROPERTY(QString layoutPreference READ layoutPreference
                   NOTIFY layoutPreferenceChanged)
    Q_PROPERTY(QString spotlightShareId READ spotlightShareId
                   NOTIFY spotlightChanged)
    Q_PROPERTY(bool restorableShareAvailable READ restorableShareAvailable
                   NOTIFY spotlightChanged)
    Q_PROPERTY(int dismissedShareCount READ dismissedShareCount
                   NOTIFY spotlightChanged)
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY fullScreenChanged)
    // Picture-in-picture: a small always-on-top window carrying the call.
    // Mutually exclusive with full screen: SfuVideoRouter holds one sink per
    // track and the last attach wins, so two surfaces would leave one black.
    Q_PROPERTY(bool pictureInPicture READ pictureInPicture
                   NOTIFY pictureInPictureChanged)
    Q_PROPERTY(bool traceEnabled READ traceEnabled CONSTANT)

public:
    explicit CallStageState(QObject *parent = nullptr);

    void setShareModel(CallShareModel *shares);

    QString pinnedIdentity() const { return m_pinnedIdentity; }
    QString layoutPreference() const { return m_layoutPreference; }
    QString spotlightShareId() const;
    bool restorableShareAvailable() const;
    int dismissedShareCount() const;
    bool fullScreen() const { return m_fullScreen; }
    bool traceEnabled() const { return m_traceEnabled; }

    Q_INVOKABLE void setFullScreen(bool fullScreen);
    Q_INVOKABLE void toggleFullScreen();
    bool pictureInPicture() const { return m_pictureInPicture; }
    Q_INVOKABLE void setPictureInPicture(bool pip);
    Q_INVOKABLE void togglePictureInPicture();

    Q_INVOKABLE void dismissShare(const QString &shareId);
    Q_INVOKABLE void restoreShare(const QString &shareId);
    Q_INVOKABLE void restoreAllShares();
    Q_INVOKABLE bool isShareDismissed(const QString &shareId) const;

    Q_INVOKABLE void pin(const QString &identity);
    Q_INVOKABLE void clearPin();

    Q_INVOKABLE void setLayoutPreference(const QString &mode);

    void clear();

Q_SIGNALS:
    void pinnedIdentityChanged();
    void layoutPreferenceChanged();
    void spotlightChanged();
    void fullScreenChanged();
    void pictureInPictureChanged();

private Q_SLOTS:
    void onShareEnded(const QString &shareId);
    void onShareAppeared(const QString &shareId);
    void onSharesChanged();

private:
    QPointer<CallShareModel> m_shares;
    QString m_pinnedIdentity;
    QString m_layoutPreference = QStringLiteral("auto");
    QSet<QString> m_dismissedShareIds;
    QString m_lastSpotlightShareId;
    bool m_lastRestorable = false;
    bool m_fullScreen = false;
    bool m_pictureInPicture = false;
    bool m_traceEnabled = false;

    bool hasFocusedSurface() const;
    void enforceFullScreenHasSurface();
};
