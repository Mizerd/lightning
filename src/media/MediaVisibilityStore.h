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
// PERSISTED LOCALLY since 2026-08-29, after two testers reported the opposite
// as a bug ("hiding images is not persisting between sessions"). The three
// reasons this class was session-only are worth keeping, because two of them
// were arguments against the WRONG kind of persistence and the third was a
// real objection that is now answered:
//
//  * "No Matrix standard for it." Still true, and still the reason nothing is
//    written to ACCOUNT DATA. This is a local preference in the same class as
//    a collapsed rail folder, which is also persisted locally and which no
//    other client is expected to agree with.
//  * "A hidden image the user has forgotten about is content they cannot
//    find." That was the real objection, and it is answered rather than
//    ignored: Settings -> Privacy & security shows how many images are hidden
//    and offers a single Show-all. Without that surface this state must NOT
//    outlive the session.
//  * "Element's behaviour was not verified." Two users have now reported
//    Element persisting it, which is better evidence than the guess this
//    comment was avoiding.
//
// Storage is STRICTLY account-scoped with no global fallback: what you chose
// not to look at is your choice from your account, and another account on the
// same machine must never inherit it.
//
// The state lives HERE and not in a QML delegate because a timeline delegate is
// destroyed and recreated as the user scrolls: a flag inside one would be lost
// the moment the row left the cache buffer, which is precisely the bug this
// class exists to avoid.
//
// Keyed by the row's media identity (`mediaKey`, which on the Rust backend IS
// the event id once the event is remote), so it survives delegate recycling,
// room switches and re-entry for as long as the session lasts.
class SettingsManager;

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

    /// Attach persistence. Loads this account's hidden list immediately.
    /// Without a settings object the store behaves exactly as it used to —
    /// session-only — which is what every test fixture gets.
    void setSettings(SettingsManager *settings);
    /// Drop the in-memory set WITHOUT writing. This is the SIGN-OUT path:
    /// the account's saved list must survive its own sign-out, so clear(),
    /// which persists, would be a data-loss bug here.
    void resetForSession();
    /// Re-read for the account that is active NOW. Called on sign-in and on
    /// an account switch: the list is per account, so carrying the previous
    /// one over would hide images this account never hid.
    void reloadForAccount();

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
    void persist();

    SettingsManager *m_settings = nullptr;
    QSet<QString> m_hidden;
    /// Insertion order, for the cap's eviction. A QSet has none.
    QStringList m_order;
};
