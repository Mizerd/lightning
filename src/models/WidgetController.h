#pragma once

#include <QAbstractListModel>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

class WidgetController : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(bool canManage READ canManage NOTIFY stateChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY stateChanged)
    Q_PROPERTY(QString lastWriteError READ lastWriteError NOTIFY lastWriteErrorChanged)

public:
    enum Roles {
        WidgetIdRole = Qt::UserRole + 1,
        CreatorRole,
        KindRole,
        NameRole,
        UrlRole,
        RefusalRole,
        DisclosesRole,
        OpenableRole,
        StateKeyRole,
        RemovableRole,
    };
    Q_ENUM(Roles)

    explicit WidgetController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setPresentation(const QString &themeName, const QString &language);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString state() const { return m_state; }
    bool supported() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();

    Q_INVOKABLE bool urlIsAcceptable(const QString &text) const;
    Q_INVOKABLE void addWidget(const QString &kind, const QString &name,
                               const QString &url);
    Q_INVOKABLE void removeWidget(int row);
    bool canManage() const { return m_canManage; }
    bool writing() const { return m_writeOp != 0; }
    QString lastWriteError() const { return m_lastWriteError; }

    static QJsonObject widgetContent(const QString &kind, const QString &name,
                                     const QString &url,
                                     const QString &creatorUserId);
    /// Open one widget in the user's browser, by row. QML never names an
    /// address, so no QML path can hand the desktop a URL that is not from this
    /// model's validated list. The URL is re-checked on the way out as a second
    /// gate. Returns false when the row cannot be opened.
    Q_INVOKABLE bool openWidget(int row);
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    Q_INVOKABLE QString disclosureText(const QString &key) const;
    Q_INVOKABLE QString refusalText(const QString &reason) const;

Q_SIGNALS:
    void roomIdChanged();
    void stateChanged();
    void writeFinished(bool ok, const QString &category);
    void lastWriteErrorChanged();

private:
    void setState(const QString &state);
    void clear();

    MatrixClient *m_client = nullptr;
    QString m_themeName;
    QString m_language;
    QString m_roomId;
    QString m_state = QStringLiteral("idle");
    QList<QVariantMap> m_rows;
    quint64 m_pendingOp = 0;
    quint64 m_writeOp = 0;
    QString m_lastWriteError;
    bool m_canManage = false;
};
