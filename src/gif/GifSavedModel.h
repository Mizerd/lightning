#pragma once

#include <QConcatenateTablesProxyModel>
#include <QVariantMap>

class GifStoredModel;

// v0.6.7: the ONE "Saved" collection behind the picker's Saved tab.
//
// Until 0.6.6 a star meant two unrelated things depending on where it was
// pressed: on a GIPHY/KLIPY tile it bookmarked a provider GIF by id (into
// GifFavoritesModel, surfaced as a "Favorites" chip *underneath* a provider
// tab, even though that list was always cross-provider), while on a chat GIF
// it copied the decrypted bytes to disk (into GifStarredStore, surfaced as a
// separate top-level "Starred" tab). One glyph, two verbs, two destinations —
// the maintainer's report. This model is the storage half of collapsing that
// into a single user-visible list: one star, one verb ("save"), one place to
// find it again.
//
// Deliberately a PRESENTATION merge only. The two backing stores stay
// completely separate, because they are not the same kind of thing and their
// security properties differ sharply:
//
//   - provider favorites hold a provider id + public CDN URLs and nothing
//     else; nothing is copied anywhere;
//   - locally-saved chat GIFs hold real bytes on disk, and are the documented
//     CLAUDE.md §7 exception to the rule against persisting decrypted media —
//     bounded, account-scoped, disclosed in Settings, and deleted on sign-out
//     and account removal.
//
// Merging the two *stores* would have put decrypted media behind a code path
// that has none of those guarantees. Merging the two *views* costs nothing and
// is what the user actually asked for. GifPicker.qml marks the on-disk rows
// with a device badge so the distinction stays visible where it matters.
//
// Local rows are listed FIRST, provider favorites follow — each group
// internally newest-first (both sources prepend on insert). This is a
// grouped-by-kind merge, NOT a chronological interleave: gif::GifResult
// carries no saved-at timestamp (both stores track recency purely by list
// position), and adding a persisted field only to interleave two lists that
// are each already newest-first was not worth it here.
//
// A GIF can legitimately appear twice — once as a GIPHY favorite and once as
// an on-disk copy saved from a chat. That is not a bug to dedupe away: it
// cannot be detected even in principle, because a locally-saved row records
// NO provenance (no room, event, sender, or provider id — see
// GifStarredStore's header for why), and it is honest, because the two rows
// really are different things (a link vs. a copy).
//
// QConcatenateTablesProxyModel (QtCore) already does the hard part correctly —
// row-offset bookkeeping across inserts/removes/moves/resets from either
// source. Its data(index, role) forwards the numeric `role` AS GIVEN to
// whichever source row the proxy index maps to, with NO per-source remapping
// by name; that only produces the right value because BOTH sources return the
// IDENTICAL GifResultModel::roleNames() table (same GifResultModel::Roles
// enum, same int -> name pairing), so role number N means the same field in
// either source. Two sources with similarly-named but differently-numbered
// roles would silently cross-wire fields; that risk does not exist here
// because GifStoredModel — the base of both GifStarredModel and
// GifFavoritesModel — always answers roleNames() with GifResultModel's table,
// never its own.
//
// This subclass adds only the slice of GifStoredModel's QML-facing contract
// GifPicker.qml actually needs (count/get), so a merged row can be captured as
// a plain snapshot exactly like a single model's row. The picker's send path
// (choose(), which calls activeModel.get(row)) therefore needs no special
// case, and the star path resolves a tile's OWN captured snapshot rather than
// re-deriving anything by index — routing saved-vs-unsaved off the snapshot's
// `provider` field ("local" vs. a real provider id), never off a row position.
class GifSavedModel : public QConcatenateTablesProxyModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // Neither source is owned; both must outlive this model. In practice both
    // are controller-owned singletons living for the application's whole run
    // (GifSearchController's m_starred model and m_favorites), exactly as
    // GifSearchController already assumes for m_favorites/m_recent. Neither is
    // retained as a member: everything below goes through the proxy's own
    // source mapping, so there is no second copy of the concatenation rule to
    // fall out of step.
    GifSavedModel(GifStoredModel *local, GifStoredModel *provider,
                  QObject *parent = nullptr);

    // 2026-08-28 (LIVE BUG, packaged builds only): the shared role table,
    // stated EXPLICITLY rather than inherited.
    //
    // QConcatenateTablesProxyModel does NOT forward its sources' role names on
    // every Qt this project ships against. Measured, same probe, same source
    // models:
    //
    //   Qt 6.11.1 (nix dev shell — every local build and every local test):
    //     roleNames() = the sources' 15 GifResultModel roles + Qt's 6 defaults
    //   Qt 6.8.2 (debian:13.6-slim — the deb/rpm/flatpak/AppImage build base):
    //     roleNames() = Qt's 6 defaults, and NOTHING else
    //
    // GifPicker.qml's grid delegate declares `required property string
    // provider` and twelve more, all resolved BY NAME through roleNames(). A
    // required property the model cannot supply makes QQmlDelegateModel refuse
    // to build the delegate at all — so on a packaged build the Saved tab
    // rendered zero tiles while count() was right, and because count() was
    // right the picker's "No saved GIFs yet" overlay stayed empty too: an
    // entirely blank panel that said nothing, which is exactly what the
    // 2026-08-28 report showed. Recent and the provider grids were unaffected
    // because they are plain GifStoredModel/GifResultModel instances answering
    // their own table.
    //
    // data() was never affected — the base class forwards the numeric role
    // as given — so every C++ path (count, get(), the star's store lookups)
    // kept working, which is why no test and no C++ assertion could see it.
    //
    // Answering GifResultModel's table here is correct on BOTH Qt versions and
    // is not merely a workaround: it is the invariant this class already
    // depends on for role forwarding to mean anything (see the note above on
    // why the numeric role may be forwarded unremapped — both sources answer
    // this identical table). Stating it makes that dependency enforceable, and
    // GifCollectionsTest/GifSavedTabQmlTest pin it against both sources.
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    // Row -> the same QVariantMap the single models answer with. Out-of-range
    // rows answer an empty map (never a neighbouring row).
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();
};
