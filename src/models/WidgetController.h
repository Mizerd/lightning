#pragma once

#include <QAbstractListModel>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

/// The widgets a room advertises, and the one thing Lightning does with them:
/// open them in the user's own browser.
///
/// # Why it opens rather than embeds
///
/// Element renders widgets in a sandboxed iframe. Doing that here needs Qt
/// WebEngine, and `docs/widgets.md` carries the measurements: the Windows
/// package is built from Fedora's mingw64 Qt and no `mingw64-qt6-qtwebengine`
/// exists (Chromium needs MSVC); Flatpak's seccomp blocklist stops Chromium's
/// own sandbox starting, so the documented workaround is to run it unsandboxed
/// — untrusted web content unsandboxed beside Megolm keys, which §6 does not
/// permit; `QtWebEngineQuick::initialize()` forces the whole application's Qt
/// Quick scenegraph to OpenGL; and the payload is ~429 MB.
///
/// Opening in the browser gives up the widget postMessage API. What it buys is
/// a containment boundary the operating system already enforces: a separate
/// process that holds none of this one's tokens, keys or memory. For the
/// widgets people actually meet — a Jitsi call, an Etherpad — that is the
/// whole of the feature anyway.
///
/// # Consent
///
/// A widget URL is room state, writable by any member with permission. Opening
/// one hands its origin whatever the URL templates — a display name, a device
/// id, a room id — plus the connection itself. `disclosures` is derived from
/// THAT URL rather than from widgets in general, so the notice never claims
/// more than is shared: a widget using no variables says only that the site
/// learns you connected to it. A notice that overstated would train people to
/// dismiss it.
class WidgetController : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY stateChanged)
    /// "idle" | "loading" | "ready" | "error"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    /// Whether this backend can read widgets at all — lets a surface be absent
    /// rather than present and empty.
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    /// Whether this account may ADD or REMOVE widgets here. From the room's
    /// own power levels via the last read; false until a read has answered.
    /// The absence of the claim is not permission.
    Q_PROPERTY(bool canManage READ canManage NOTIFY stateChanged)
    /// A write is in flight. One at a time: the UI disables itself on this so
    /// a second click cannot race the first.
    Q_PROPERTY(bool writing READ writing NOTIFY stateChanged)
    /// The category of the last write that failed, empty after a success or
    /// a room change — so the Widgets tab can say a Remove did nothing.
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
    /// The theme name and language are TEMPLATE VARIABLES a widget URL may
    /// carry, so a widget can match the client's look. Plain strings, pushed
    /// in: this model has no business owning either, and taking the managers
    /// would drag settings and i18n into a test that is about URLs.
    void setPresentation(const QString &themeName, const QString &language);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString state() const { return m_state; }
    bool supported() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();

    // ── Adding and removing (v0.9.0) ────────────────────────────────────
    //
    // Lightning still never EMBEDS a widget (docs/widgets.md). Adding one is
    // writing a room-state event that every client — including this one —
    // then lists and opens in a browser. `kind` is one of the MSC1236 types
    // this picker offers ("m.custom", "m.etherpad", "m.jitsi", "m.video",
    // "m.image", "m.grafana"); `url` must be https with a host and no
    // credentials, checked here so the dialog can refuse before sending and
    // again on the Rust side so a caller that did not ask cannot bypass it.
    Q_INVOKABLE bool urlIsAcceptable(const QString &text) const;
    Q_INVOKABLE void addWidget(const QString &kind, const QString &name,
                               const QString &url);
    /// Remove by ROW, never by an id QML could name: the id is the row's own.
    Q_INVOKABLE void removeWidget(int row);
    bool canManage() const { return m_canManage; }
    bool writing() const { return m_writeOp != 0; }
    QString lastWriteError() const { return m_lastWriteError; }

    /// The event content addWidget() would publish — exposed so a test can
    /// pin the shape without a client. `id`/`creatorUserId` are what Element
    /// reads back from the envelope anyway; they are written for clients that
    /// read the content instead.
    static QJsonObject widgetContent(const QString &kind, const QString &name,
                                     const QString &url,
                                     const QString &creatorUserId);
    /// Open one widget in the user's browser, BY ROW.
    ///
    /// Deliberately not "open this URL". QML never names an address here, so
    /// no QML path — present or future, in this file or another — can hand the
    /// desktop a URL that did not come from this model's own validated list.
    /// The row's URL is re-checked on the way out even so: Rust validated it
    /// when it was read, and this is the second gate rather than the first.
    ///
    /// Returns false when the row cannot be opened, so the caller can say so.
    Q_INVOKABLE bool openWidget(int row);
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    /// A human sentence for one disclosure key, so the consent sheet renders
    /// the same words everywhere and a new key cannot appear untranslated.
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
