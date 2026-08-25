#include "spaces/RailLayoutStore.h"

#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
constexpr auto kLayoutKey = "shell/railLayout";

// A pseudo row (All rooms, orphans) is not something the user can order or
// file: it is a view of everything, not a Space. Both keep their place at the
// top of the rail.
bool isPseudoSpace(const QString &id)
{
    return id.isEmpty() || id.startsWith(QLatin1Char('@'));
}
} // namespace

RailLayoutStore::RailLayoutStore(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
}

const RailLayoutStore::Layout &RailLayoutStore::load() const
{
    if (m_loaded)
        return m_cache;
    m_loaded = true;
    m_cache = {};
    if (!m_settings)
        return m_cache;

    const QString json =
        m_settings->appearanceValue(kLayoutKey, QString()).toString();
    if (json.isEmpty())
        return m_cache;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return m_cache;
    const QJsonObject object = doc.object();

    const QJsonArray folders = object.value(QStringLiteral("folders")).toArray();
    for (const QJsonValue &value : folders) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();
        Folder folder;
        folder.id = entry.value(QStringLiteral("id")).toString();
        if (folder.id.isEmpty())
            continue;
        folder.name = entry.value(QStringLiteral("name")).toString()
                          .left(kMaxNameLength);
        folder.collapsed = entry.value(QStringLiteral("collapsed")).toBool();
        for (const QJsonValue &member :
             entry.value(QStringLiteral("spaceIds")).toArray()) {
            const QString id = member.toString();
            // A pseudo row can never be filed, however the config got edited.
            if (!id.isEmpty() && !isPseudoSpace(id)
                && !folder.spaceIds.contains(id))
                folder.spaceIds.append(id);
        }
        m_cache.folders.append(folder);
        if (m_cache.folders.size() >= kMaxFolders)
            break;
    }
    for (const QJsonValue &value : object.value(QStringLiteral("order")).toArray()) {
        const QString id = value.toString();
        if (!id.isEmpty() && !isPseudoSpace(id) && !m_cache.order.contains(id))
            m_cache.order.append(id);
    }
    // Added after 0.7.6. The format is ADDITIVE on purpose: a layout written
    // by an older build has no "expanded" key and loads with nothing expanded,
    // which is exactly what it meant. Never migrate what can be defaulted.
    for (const QJsonValue &value :
         object.value(QStringLiteral("expanded")).toArray()) {
        const QString id = value.toString();
        if (!id.isEmpty() && !isPseudoSpace(id)
            && !m_cache.expanded.contains(id)) {
            m_cache.expanded.append(id);
        }
    }
    return m_cache;
}

void RailLayoutStore::save(const Layout &layout)
{
    if (!m_settings)
        return;
    QJsonArray folders;
    for (const Folder &folder : layout.folders) {
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), folder.id);
        entry.insert(QStringLiteral("name"), folder.name);
        entry.insert(QStringLiteral("collapsed"), folder.collapsed);
        entry.insert(QStringLiteral("spaceIds"),
                     QJsonArray::fromStringList(folder.spaceIds));
        folders.append(entry);
    }
    QJsonObject object;
    object.insert(QStringLiteral("folders"), folders);
    object.insert(QStringLiteral("order"),
                  QJsonArray::fromStringList(layout.order));
    object.insert(QStringLiteral("expanded"),
                  QJsonArray::fromStringList(layout.expanded));
    m_settings->setAppearanceValue(
        kLayoutKey, QString::fromUtf8(
                        QJsonDocument(object).toJson(QJsonDocument::Compact)));
    m_cache = layout;
    m_loaded = true;
    Q_EMIT layoutChanged();
}

QString RailLayoutStore::makeFolderId(const Layout &layout)
{
    for (int candidate = 1; candidate <= kMaxFolders * 4; ++candidate) {
        const QString id = QStringLiteral("f") + QString::number(candidate);
        bool taken = false;
        for (const Folder &folder : layout.folders) {
            if (folder.id == id) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return id;
    }
    return {};
}

QVariantList RailLayoutStore::folders() const
{
    QVariantList out;
    for (const Folder &folder : load().folders) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), folder.id);
        entry.insert(QStringLiteral("name"), folder.name);
        entry.insert(QStringLiteral("collapsed"), folder.collapsed);
        entry.insert(QStringLiteral("spaceIds"), folder.spaceIds);
        out.append(entry);
    }
    return out;
}

QStringList RailLayoutStore::order() const
{
    return load().order;
}

QString RailLayoutStore::createFolder(const QString &name)
{
    Layout layout = load();
    if (layout.folders.size() >= kMaxFolders)
        return {};
    Folder folder;
    folder.id = makeFolderId(layout);
    if (folder.id.isEmpty())
        return {};
    folder.name = name.trimmed().left(kMaxNameLength);
    if (folder.name.isEmpty())
        folder.name = tr("Folder");
    layout.folders.append(folder);
    layout.order.append(folder.id);
    save(layout);
    return folder.id;
}

void RailLayoutStore::renameFolder(const QString &folderId, const QString &name)
{
    Layout layout = load();
    for (Folder &folder : layout.folders) {
        if (folder.id != folderId)
            continue;
        const QString clean = name.trimmed().left(kMaxNameLength);
        if (clean.isEmpty() || clean == folder.name)
            return;
        folder.name = clean;
        save(layout);
        return;
    }
}

void RailLayoutStore::deleteFolder(const QString &folderId)
{
    Layout layout = load();
    for (int i = 0; i < layout.folders.size(); ++i) {
        if (layout.folders.at(i).id != folderId)
            continue;
        // The Spaces come back to the top level WHERE THE FOLDER WAS, not at
        // the end: deleting a folder is undoing the grouping, not scattering
        // its contents to the bottom of the rail.
        const QStringList members = layout.folders.at(i).spaceIds;
        layout.folders.removeAt(i);
        const int at = layout.order.indexOf(folderId);
        if (at >= 0) {
            layout.order.removeAt(at);
            for (int m = members.size() - 1; m >= 0; --m) {
                if (!layout.order.contains(members.at(m)))
                    layout.order.insert(at, members.at(m));
            }
        } else {
            for (const QString &id : members) {
                if (!layout.order.contains(id))
                    layout.order.append(id);
            }
        }
        save(layout);
        return;
    }
}

void RailLayoutStore::setFolderCollapsed(const QString &folderId, bool collapsed)
{
    Layout layout = load();
    for (Folder &folder : layout.folders) {
        if (folder.id != folderId || folder.collapsed == collapsed)
            continue;
        folder.collapsed = collapsed;
        save(layout);
        return;
    }
}

QString RailLayoutStore::folderOf(const QString &spaceId) const
{
    for (const Folder &folder : load().folders) {
        if (folder.spaceIds.contains(spaceId))
            return folder.id;
    }
    return {};
}

void RailLayoutStore::setSpaceFolder(const QString &spaceId,
                                     const QString &folderId)
{
    if (spaceId.isEmpty() || isPseudoSpace(spaceId))
        return;
    Layout layout = load();
    bool changed = false;
    // A Space is in at most one folder, so leaving the old one is part of
    // joining a new one rather than a separate call the caller could forget.
    for (Folder &folder : layout.folders) {
        if (folder.id != folderId && folder.spaceIds.removeAll(spaceId) > 0)
            changed = true;
    }
    if (folderId.isEmpty()) {
        // Back to the top level. It goes to the end rather than to a
        // remembered slot: the slot it came from belonged to the folder.
        if (!layout.order.contains(spaceId)) {
            layout.order.append(spaceId);
            changed = true;
        }
    } else {
        bool known = false;
        for (Folder &folder : layout.folders) {
            if (folder.id != folderId)
                continue;
            known = true;
            if (!folder.spaceIds.contains(spaceId)) {
                folder.spaceIds.append(spaceId);
                changed = true;
            }
        }
        if (!known)
            return;
        // A filed Space is no longer a top-level entry.
        if (layout.order.removeAll(spaceId) > 0)
            changed = true;
    }
    if (changed)
        save(layout);
}

void RailLayoutStore::moveEntry(const QString &entryId, int toIndex)
{
    if (entryId.isEmpty() || isPseudoSpace(entryId))
        return;
    Layout layout = load();
    // An entry that has never been dragged is not in `order` yet. Nothing can
    // be positioned relative to an implicit tail, so the caller's own view of
    // the order is what gets materialised: the rail passes the id it dragged
    // and the index it dropped it at, and everything before it is already
    // there by the time this runs.
    const int from = layout.order.indexOf(entryId);
    if (from >= 0)
        layout.order.removeAt(from);
    const int clamped = qBound(0, toIndex, layout.order.size());
    layout.order.insert(clamped, entryId);
    save(layout);
}

void RailLayoutStore::setTopLevelOrder(const QStringList &entryIds)
{
    Layout layout = load();
    QStringList next;
    for (const QString &id : entryIds) {
        if (id.isEmpty() || isPseudoSpace(id) || next.contains(id))
            continue;
        if (!folderOf(id).isEmpty())
            continue;   // filed: it is not a top-level entry
        next.append(id);
    }
    if (next == layout.order)
        return;
    layout.order = next;
    save(layout);
}

QStringList RailLayoutStore::expandedSpaceIds() const
{
    return load().expanded;
}

bool RailLayoutStore::spaceExpanded(const QString &spaceId) const
{
    return load().expanded.contains(spaceId);
}

void RailLayoutStore::setSpaceExpanded(const QString &spaceId, bool expanded)
{
    if (spaceId.isEmpty() || isPseudoSpace(spaceId))
        return;
    Layout layout = load();
    const bool has = layout.expanded.contains(spaceId);
    if (has == expanded)
        return;
    if (expanded)
        layout.expanded.append(spaceId);
    else
        layout.expanded.removeAll(spaceId);
    save(layout);
}

void RailLayoutStore::toggleSpaceExpanded(const QString &spaceId)
{
    setSpaceExpanded(spaceId, !spaceExpanded(spaceId));
}

QStringList RailLayoutStore::folderMembers(const QString &folderId) const
{
    for (const Folder &folder : load().folders) {
        if (folder.id == folderId)
            return folder.spaceIds;
    }
    return {};
}

QString RailLayoutStore::createFolderWithSpaces(const QStringList &spaceIds,
                                                int atIndex,
                                                const QString &name)
{
    QStringList members;
    for (const QString &id : spaceIds) {
        if (id.isEmpty() || isPseudoSpace(id) || members.contains(id))
            continue;
        members.append(id);
    }
    if (members.isEmpty())
        return {};
    Layout layout = load();
    if (layout.folders.size() >= kMaxFolders)
        return {};
    Folder folder;
    folder.id = makeFolderId(layout);
    if (folder.id.isEmpty())
        return {};
    folder.name = name.trimmed().left(kMaxNameLength);
    if (folder.name.isEmpty())
        folder.name = tr("Folder");
    folder.spaceIds = members;
    // The members leave wherever they were: another folder, or the top level.
    for (Folder &other : layout.folders) {
        for (const QString &id : members)
            other.spaceIds.removeAll(id);
    }
    for (const QString &id : members)
        layout.order.removeAll(id);
    layout.folders.append(folder);
    const int clamped = atIndex < 0 ? layout.order.size()
                                    : qBound(0, atIndex, layout.order.size());
    layout.order.insert(clamped, folder.id);
    save(layout);
    return folder.id;
}

void RailLayoutStore::moveSpaceToFolder(const QString &spaceId,
                                        const QString &folderId, int index)
{
    if (spaceId.isEmpty() || isPseudoSpace(spaceId) || folderId.isEmpty())
        return;
    Layout layout = load();
    bool known = false;
    for (const Folder &folder : layout.folders) {
        if (folder.id == folderId) {
            known = true;
            break;
        }
    }
    if (!known)
        return;
    Layout next = layout;
    for (Folder &folder : next.folders) {
        if (folder.id == folderId)
            continue;
        folder.spaceIds.removeAll(spaceId);
    }
    for (Folder &folder : next.folders) {
        if (folder.id != folderId)
            continue;
        folder.spaceIds.removeAll(spaceId);
        const int clamped = index < 0
                                ? folder.spaceIds.size()
                                : qBound(0, index, folder.spaceIds.size());
        folder.spaceIds.insert(clamped, spaceId);
    }
    next.order.removeAll(spaceId);
    if (next.folders == layout.folders && next.order == layout.order)
        return;
    save(next);
}

void RailLayoutStore::applyArrangement(const QStringList &topLevel,
                                       const QVariantMap &folderMembers)
{
    Layout layout = load();
    Layout next = layout;

    QSet<QString> knownFolders;
    for (const Folder &folder : next.folders)
        knownFolders.insert(folder.id);

    // Every space the caller placed inside a folder, and which folder it is.
    QHash<QString, QString> assigned;
    QHash<QString, QStringList> rendered;
    for (auto it = folderMembers.constBegin(); it != folderMembers.constEnd();
         ++it) {
        if (!knownFolders.contains(it.key()))
            continue;   // a folder that no longer exists names nothing
        QStringList members;
        for (const QString &id : it.value().toStringList()) {
            if (id.isEmpty() || isPseudoSpace(id) || knownFolders.contains(id)
                || members.contains(id) || assigned.contains(id)) {
                continue;   // a Space is in at most one place
            }
            members.append(id);
            assigned.insert(id, it.key());
        }
        rendered.insert(it.key(), members);
    }

    QSet<QString> topLevelSet;
    QStringList order;
    for (const QString &id : topLevel) {
        if (id.isEmpty() || isPseudoSpace(id) || order.contains(id))
            continue;
        if (assigned.contains(id))
            continue;   // named as a folder member: not a top-level entry
        order.append(id);
        topLevelSet.insert(id);
    }

    for (Folder &folder : next.folders) {
        const auto it = rendered.constFind(folder.id);
        if (it != rendered.constEnd()) {
            folder.spaceIds = *it;
            continue;
        }
        // A folder the caller did not render — a COLLAPSED one — keeps its
        // members, except any the call placed somewhere else. Replacing them
        // with an empty list here is how a drag past a collapsed folder would
        // silently empty it.
        for (int i = folder.spaceIds.size() - 1; i >= 0; --i) {
            const QString &id = folder.spaceIds.at(i);
            if (assigned.contains(id) || topLevelSet.contains(id))
                folder.spaceIds.removeAt(i);
        }
    }
    next.order = order;
    if (next.folders == layout.folders && next.order == layout.order)
        return;
    save(next);
}

QStringList RailLayoutStore::orderedSpaceIds(const QVariantList &spaces) const
{
    const Layout &layout = load();
    QStringList natural;
    QSet<QString> present;
    for (const QVariant &value : spaces) {
        const QString id =
            value.toMap().value(QStringLiteral("spaceId")).toString();
        if (id.isEmpty() || isPseudoSpace(id) || present.contains(id))
            continue;
        present.insert(id);
        natural.append(id);
    }

    QHash<QString, QString> folderOfSpace;
    for (const Folder &folder : layout.folders) {
        for (const QString &id : folder.spaceIds) {
            if (present.contains(id) && !folderOfSpace.contains(id))
                folderOfSpace.insert(id, folder.id);
        }
    }

    // The top-level walk, with each folder expanded in place regardless of
    // whether it is collapsed: this answers an ORDERING question, and a
    // collapsed folder still contains its Spaces.
    QStringList out;
    auto appendFolder = [&](const QString &folderId) {
        for (const Folder &folder : layout.folders) {
            if (folder.id != folderId)
                continue;
            for (const QString &id : folder.spaceIds) {
                if (present.contains(id) && !out.contains(id))
                    out.append(id);
            }
            return;
        }
    };
    auto isFolderId = [&layout](const QString &id) {
        for (const Folder &folder : layout.folders) {
            if (folder.id == id)
                return true;
        }
        return false;
    };
    QStringList top;
    for (const QString &id : layout.order) {
        if (folderOfSpace.contains(id))
            continue;
        top.append(id);
    }
    for (const Folder &folder : layout.folders) {
        if (!top.contains(folder.id))
            top.append(folder.id);
    }
    for (const QString &id : natural) {
        if (!folderOfSpace.contains(id) && !top.contains(id))
            top.append(id);
    }
    for (const QString &id : top) {
        if (isFolderId(id))
            appendFolder(id);
        else if (present.contains(id) && !out.contains(id))
            out.append(id);
    }
    return out;
}

QVariantList RailLayoutStore::arrange(const QVariantList &spaces) const
{
    const Layout &layout = load();

    QVariantList pseudo;
    QHash<QString, QVariantMap> byId;
    QStringList natural;
    for (const QVariant &value : spaces) {
        const QVariantMap entry = value.toMap();
        const QString id = entry.value(QStringLiteral("spaceId")).toString();
        if (isPseudoSpace(id)) {
            pseudo.append(entry);
            continue;
        }
        byId.insert(id, entry);
        natural.append(id);
    }

    // Folder membership, restricted to Spaces that actually exist right now.
    QHash<QString, QString> folderOfSpace;
    for (const Folder &folder : layout.folders) {
        for (const QString &id : folder.spaceIds) {
            if (byId.contains(id))
                folderOfSpace.insert(id, folder.id);
        }
    }

    // Top-level entries: the user's order first, then anything new, in the
    // order the model gave it. A newly joined Space appears at the bottom
    // rather than in the middle of a hand-made arrangement.
    QStringList top;
    for (const QString &id : layout.order) {
        if (folderOfSpace.contains(id))
            continue;            // filed since it was ordered
        if (byId.contains(id) || [&] {
                for (const Folder &folder : layout.folders) {
                    if (folder.id == id)
                        return true;
                }
                return false;
            }()) {
            top.append(id);
        }
    }
    for (const Folder &folder : layout.folders) {
        if (!top.contains(folder.id))
            top.append(folder.id);
    }
    for (const QString &id : natural) {
        if (!folderOfSpace.contains(id) && !top.contains(id))
            top.append(id);
    }

    QVariantList out = pseudo;
    for (const QString &id : top) {
        const Folder *folder = nullptr;
        for (const Folder &candidate : layout.folders) {
            if (candidate.id == id) {
                folder = &candidate;
                break;
            }
        }
        if (!folder) {
            auto it = byId.constFind(id);
            if (it == byId.constEnd())
                continue;
            QVariantMap entry = *it;
            entry.insert(QStringLiteral("kind"), QStringLiteral("space"));
            entry.insert(QStringLiteral("entryId"), id);
            entry.insert(QStringLiteral("folderId"), QString());
            entry.insert(QStringLiteral("folderLast"), false);
            out.append(entry);
            continue;
        }

        // The members that still exist, in the folder's own order.
        QStringList members;
        int unread = 0;
        int highlight = 0;
        // Up to four member avatars for the folder tile's composite preview.
        // Discord's folder icon is a grid of the servers inside it, and it is
        // the reason a collapsed folder is identifiable at all — a generic
        // letter tile says only "a folder", which on a 40px rail is the one
        // thing the user already knows.
        QVariantList preview;
        for (const QString &memberId : folder->spaceIds) {
            auto it = byId.constFind(memberId);
            if (it == byId.constEnd())
                continue;
            members.append(memberId);
            unread += it->value(QStringLiteral("unreadTotal")).toInt();
            highlight += it->value(QStringLiteral("highlightTotal")).toInt();
            if (preview.size() < 4) {
                preview.append(QVariantMap{
                    { QStringLiteral("spaceId"), memberId },
                    { QStringLiteral("name"),
                      it->value(QStringLiteral("name")) },
                    { QStringLiteral("avatarUrl"),
                      it->value(QStringLiteral("avatarUrl")) },
                });
            }
        }
        // An empty folder still renders: it is a place the user made, and one
        // that vanished when its last Space moved out would be a bug report.
        QVariantMap entry;
        entry.insert(QStringLiteral("kind"), QStringLiteral("folder"));
        entry.insert(QStringLiteral("entryId"), folder->id);
        entry.insert(QStringLiteral("folderId"), folder->id);
        entry.insert(QStringLiteral("spaceId"), QString());
        entry.insert(QStringLiteral("name"), folder->name);
        entry.insert(QStringLiteral("collapsed"), folder->collapsed);
        entry.insert(QStringLiteral("memberIds"), members);
        entry.insert(QStringLiteral("memberPreview"), preview);
        entry.insert(QStringLiteral("childCount"), int(members.size()));
        entry.insert(QStringLiteral("unreadTotal"), unread);
        entry.insert(QStringLiteral("highlightTotal"), highlight);
        out.append(entry);

        if (folder->collapsed)
            continue;
        for (int m = 0; m < members.size(); ++m) {
            const QString &memberId = members.at(m);
            QVariantMap member = byId.value(memberId);
            member.insert(QStringLiteral("kind"), QStringLiteral("space"));
            member.insert(QStringLiteral("entryId"), memberId);
            member.insert(QStringLiteral("folderId"), folder->id);
            // The open folder is drawn as ONE container behind its rows, so
            // the last member has to know it is the last: it carries the
            // rounded bottom, and without it the container reads as a band
            // that ran off the end of the group.
            member.insert(QStringLiteral("folderLast"),
                          m == members.size() - 1);
            out.append(member);
        }
    }
    return out;
}
