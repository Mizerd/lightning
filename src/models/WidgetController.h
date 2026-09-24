#pragma once

#include <QAbstractListModel>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

/// The widgets a room advertises, opened in the user's own browser.
///
/// Embedding would need Qt WebEngine, which the Windows toolchain cannot
/// build, Flatpak can only run unsandboxed beside Megolm keys, and which forces
/// the whole scenegraph to OpenGL (see docs/widgets.md). The browser loses the
/// widget postMessage API but gives an OS-enforced boundary: a separate process
/// holding none of this one's tokens or keys.
///
/// Consent: a widget URL is room state any permitted member can write, and
/// opening it discloses whatever the URL templates (display name, device id,
/// room id) plus the connection. `disclosures` is derived from that URL, so the
/// notice never claims more than is actually shared.
class WidgetController : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY stateChanged)
    /// "idle" | "loading" | "ready" | "error"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    /// Whether this backend can read widgets at all, so a surface can be absent
    /// rather than empty.
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    /// Whether this account may add or remove widgets here, from the room's
    /// power levels as of the last read. False until a read answers.
    Q_PROPERTY(bool canManage READ canManage NOTIFY stateChanged)
    /// A write is in flight; the UI disables itself so a second click cannot
    /// race the first.
    Q_PROPERTY(bool writing READ writing NOTIFY stateChanged)
    /// Category of the last failed write; empty after a success or room change.
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
    /// Theme name and language are template variables a widget URL may carry.
    /// Pushed in as plain strings so this model owns neither.
    void setPresentation(const QString &themeName, const QString &language);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString state() const { return m_state; }
    bool supported() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();

    // ── Adding and removing ──────────────────────────────────────────────
    //
    // Adding writes a room-state event that clients list and open in a browser;
    // nothing is embedded. `kind` is one of the MSC1236 types offered
    // ("m.custom", "m.etherpad", "m.jitsi", "m.video", "m.image", "m.grafana").
    // `url` must be https with a host and no credentials, checked here and
    // again in Rust.
    Q_INVOKABLE bool urlIsAcceptable(const QString &text) const;
    Q_INVOKABLE void addWidget(const QString &kind, const QString &name,
                               const QString &url);
    /// Remove by row, never by an id QML could name.
    Q_INVOKABLE void removeWidget(int row);
    bool canManage() const { return m_canManage; }
    bool writing() const { return m_writeOp != 0; }
    QString lastWriteError() const { return m_lastWriteError; }

    /// The content addWidget() publishes, exposed for tests.
    /// `id`/`creatorUserId` duplicate the envelope for clients that read the
    /// content.
    static QJsonObject widgetContent(const QString &kind, const QString &name,
                                     const QString &url,
                                     const QString &creatorUserId);
    /// Open one widget in the user's browser, by row. QML never names an
    /// address, so no QML path can hand the desktop a URL that is not from this
    /// model's validated list. The URL is re-checked on the way out as a second
    /// gate. Returns false when the row cannot be opened.
    Q_INVOKABLE bool openWidget(int row);
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    /// A sentence for one disclosure key, so the consent sheet reads the same
    /// everywhere.
    Q_INVOKABLE QString disclosureText(const QString &key) const;
    /// Why a widget cannot be opened, in words.
    Q_INVOKABLE QString refusalText(const QString &reason) const;

Q_SIGNALS:
    void roomIdChanged();
    void stateChanged();
    /// A write finished. `category` is empty on success.
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
