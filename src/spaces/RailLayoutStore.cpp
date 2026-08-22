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
            out.append(entry);
            continue;
        }

        // The members that still exist, in the folder's own order.
        QStringList members;
        int unread = 0;
        int highlight = 0;
        for (const QString &memberId : folder->spaceIds) {
            auto it = byId.constFind(memberId);
            if (it == byId.constEnd())
                continue;
            members.append(memberId);
            unread += it->value(QStringLiteral("unreadTotal")).toInt();
            highlight += it->value(QStringLiteral("highlightTotal")).toInt();
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
        entry.insert(QStringLiteral("childCount"), int(members.size()));
        entry.insert(QStringLiteral("unreadTotal"), unread);
        entry.insert(QStringLiteral("highlightTotal"), highlight);
        out.append(entry);

        if (folder->collapsed)
            continue;
        for (const QString &memberId : members) {
            QVariantMap member = byId.value(memberId);
            member.insert(QStringLiteral("kind"), QStringLiteral("space"));
            member.insert(QStringLiteral("entryId"), memberId);
            member.insert(QStringLiteral("folderId"), folder->id);
            out.append(member);
        }
    }
    return out;
}
