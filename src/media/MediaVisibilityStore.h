#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Which images the user has hidden in the timeline, Element-style.
//
// PURELY LOCAL RENDERING STATE. Hiding an image sends nothing, edits nothing,
// redacts nothing and changes nothing about what any other client sees. It is
// the same class of thing as a collapsed folder: a statement about this
// screen.
//
// SESSION-ONLY, deliberately. Three reasons, in order of weight:
//
//  * There is no Matrix standard for it. Persisting it would mean inventing an
//    account-data key only Lightning could read — which is exactly what this
//    project refuses to put in someone's account — or a local database of
//    event ids that no other client would ever agree with.
//  * A hidden image the user has forgotten about is content they cannot find.
//    There is no list of hidden media and no discoverable "unhide everything",
//    so a flag that outlived the session would quietly remove pictures from a
//    room with no way back except by scrolling to each one.
//  * Element's own behaviour was not verified here, so inventing durable
//    storage to match a guess would be worse than a clean session-local
//    implementation that says so.
//
// The state lives HERE and not in a QML delegate because a timeline delegate is
// destroyed and recreated as the user scrolls: a flag inside one would be lost
// the moment the row left the cache buffer, which is precisely the bug this
// class exists to avoid.
//
// Keyed by the row's media identity (`mediaKey`, which on the Rust backend IS
// the event id once the event is remote), so it survives delegate recycling,
// room switches and re-entry for as long as the session lasts.
class MediaVisibilityStore : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenCountChanged)

public:
    explicit MediaVisibilityStore(QObject *parent = nullptr);

    Q_INVOKABLE bool isHidden(const QString &key) const;
    Q_INVOKABLE void hide(const QString &key);
    Q_INVOKABLE void show(const QString &key);
    Q_INVOKABLE void setHidden(const QString &key, bool hidden);
    /// Wiped on sign-out and on an account switch: what one account hid is not
    /// a statement about the next one's rooms.
    Q_INVOKABLE void clear();

    int hiddenCount() const { return int(m_hidden.size()); }

    /// Bounded, because the key set grows with every hide and nothing outside
    /// this class ever prunes it. At the cap the OLDEST hidden image is
    /// revealed again rather than the new one being refused: refusing to hide
    /// something the user just asked to hide is the worse failure, and 4096
    /// hidden images in one session is far past any real use.
    static constexpr int kMaxHidden = 4096;

Q_SIGNALS:
    /// One key changed. QML rows re-query on this rather than binding, because
    /// `isHidden` is a call and carries no per-key NOTIFY of its own.
    void hiddenChanged(const QString &key, bool hidden);
    void hiddenCountChanged();

private:
    QSet<QString> m_hidden;
    /// Insertion order, for the cap's eviction. A QSet has none.
    QStringList m_order;
};
