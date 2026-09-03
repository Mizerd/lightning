#pragma once

#include <QAbstractListModel>
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
};
