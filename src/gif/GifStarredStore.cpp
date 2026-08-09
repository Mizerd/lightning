#include "gif/GifStarredStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>

GifStarredStore::GifStarredStore(QObject *parent)
    : QObject(parent)
    // Constructed once, with no backing store, and kept alive for this
    // object's whole life — see the header comment on why a stable model
    // identity matters (GifPicker.qml's Starred tab binds this pointer
    // directly).
    , m_model(std::make_unique<GifStarredModel>(nullptr, this))
{
    connect(m_model.get(), &GifStoredModel::countChanged,
            this, &GifStarredStore::countChanged);
}

GifStarredStore::~GifStarredStore() = default;

bool GifStarredStore::isSafeHashHex(const QString &hash)
{
    if (hash.size() != 64) // sha256 hex digest length
        return false;
    for (const QChar c : hash) {
        const ushort u = c.unicode();
        const bool digit = u >= '0' && u <= '9';
        const bool lower = u >= 'a' && u <= 'f';
        if (!digit && !lower)
            return false;
    }
    return true;
}

QString GifStarredStore::filePath(const QString &hash) const
{
    return m_dir + QLatin1Char('/') + hash + QStringLiteral(".gif");
}

bool GifStarredStore::ensureDirectory() const
{
    if (m_dir.isEmpty())
        return false;
    if (!QDir().mkpath(m_dir))
        return false;
    // Owner-only: decrypted GIF bytes and the index live here.
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ExeOwner);
    return true;
}

QString GifStarredStore::categoryMessage(const QString &category) const
{
    if (category == QLatin1String("not_a_gif"))
        return tr("That file is not a GIF.");
    if (category == QLatin1String("invalid_media"))
        return tr("That GIF could not be read.");
    // v0.6.7: these are the sentences the timeline banner shows verbatim, so
    // they speak the one user-facing vocabulary the star now has everywhere —
    // "saved", never "starred". The category TOKENS are unchanged (they are a
    // stable programmatic contract, asserted by GifStarredStoreTest and
    // branched on by TimelinePane.qml); only the prose moved.
    if (category == QLatin1String("too_large"))
        return tr("That GIF is too large to save.");
    if (category == QLatin1String("cap_items")) {
        return tr("Saved GIFs is full (%1 items) — remove one first.")
            .arg(m_maxItems);
    }
    if (category == QLatin1String("cap_bytes")) {
        const qint64 mib = m_maxTotalBytes / (1024 * 1024);
        return tr("Saved GIFs is full (%1 MiB) — remove one first.")
            .arg(mib);
    }
    if (category == QLatin1String("write_failed"))
        return tr("Lightning could not save that GIF to this device.");
    if (category == QLatin1String("not_open"))
        return tr("Saved GIFs is unavailable right now.");
    if (category == QLatin1String("unavailable"))
        return tr("The GIF could not be fetched.");
    if (category == QLatin1String("timeout"))
        return tr("Fetching the GIF timed out.");
    if (category == QLatin1String("already_starred"))
        return tr("Already in your saved GIFs.");
    return tr("The GIF could not be saved.");
}

void GifStarredStore::openFor(const QString &accountDir)
{
    m_settings.reset();
    m_dir.clear();
    m_mediaKeyToHash.clear();

    const QString dir = accountDir.trimmed();
    if (dir.isEmpty()) {
        m_model->reopen(nullptr); // closed/inert — drop any previous rows
        return;
    }
    // Refuse a symlinked account root or (if it already exists from an
    // earlier session) store directory — recursing into, writing decrypted
    // bytes under, or later deleting through a link could land anywhere on
    // disk. Mirrors matrix::app_data::isSafeAccountIdentity's own symlink
    // rejection for the Rust store. The directory itself is created lazily
    // on the first real write (ensureDirectory()), not here — a fresh
    // account that never stars anything never gets an empty starred-gifs
    // directory materialized on every login.
    const QFileInfo dirInfo(dir);
    const QFileInfo parentInfo(dirInfo.dir().absolutePath());
    if (dirInfo.isSymLink() || parentInfo.isSymLink()) {
        m_model->reopen(nullptr);
        return;
    }
    m_dir = dir;
    m_settings = std::make_unique<QSettings>(
        dir + QStringLiteral("/index.ini"), QSettings::IniFormat);
    m_model->reopen(m_settings.get());

    // Disk state is authoritative in BOTH directions:
    //
    // (1) index -> file: a stale or tampered index entry whose backing file
    // is missing must never surface a tile that cannot actually be played
    // or sent.
    QStringList stale;
    for (int i = 0; i < m_model->count(); ++i) {
        const QString hash = m_model->resultAt(i).id;
        if (!isSafeHashHex(hash) || !QFileInfo::exists(filePath(hash)))
            stale.append(hash);
    }
    for (const QString &hash : std::as_const(stale))
        m_model->unstar(hash);

    // (2) file -> index (orphan sweep): after the pruning above, any *.gif
    // file whose basename is not a hash the (now-pruned) index actually
    // references is unreferenced decrypted content — left behind by a crash
    // between writing the file and registering the row, or by a
    // hand-edited/corrupted index. It must not silently escape the cap or
    // linger on disk forever. (A second concurrent Lightning instance on
    // the same account could in principle lose a just-written, not-yet-
    // indexed file here — multi-instance is unsupported anyway; the shared
    // index.ini would already conflict.)
    QDir dirObj(m_dir);
    if (dirObj.exists()) {
        const auto entries =
            dirObj.entryList({ QStringLiteral("*.gif") }, QDir::Files);
        for (const QString &entry : entries) {
            const QString hash = entry.chopped(4); // strip ".gif"
            if (!isSafeHashHex(hash) || !m_model->hasHash(hash))
                QFile::remove(dirObj.filePath(entry));
        }
    }
}

void GifStarredStore::close()
{
    openFor(QString());
}

void GifStarredStore::starBytes(const QString &mediaKey, const QByteArray &bytes)
{
    if (!isOpen()) {
        Q_EMIT starFinished(mediaKey, false, QStringLiteral("not_open"),
                            categoryMessage(QStringLiteral("not_open")));
        return;
    }
    const gif::GifByteValidation v = gif::validateGifBytes(bytes);
    if (!v.ok) {
        Q_EMIT starFinished(mediaKey, false, v.category,
                            categoryMessage(v.category));
        return;
    }
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    // Re-starring an already-indexed hash is only a cap-exempt no-write iff
    // the backing file is ACTUALLY still there — an index row surviving
    // some other process deleting the file underneath it must not skip the
    // write and leave the row pointing at nothing.
    const bool existing = m_model->hasHash(hash) && QFileInfo::exists(filePath(hash));
    if (!existing) {
        // Refused, never silently evicted: a local star is an explicit
        // "keep this" the user made — unlike Favorites/Recents (see
        // GifStoredModel::insertFront), the cap here is a hard refusal.
        if (m_model->count() >= m_maxItems) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("cap_items"),
                                categoryMessage(QStringLiteral("cap_items")));
            return;
        }
        if (m_model->totalBytes() + bytes.size() > m_maxTotalBytes) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("cap_bytes"),
                                categoryMessage(QStringLiteral("cap_bytes")));
            return;
        }
        if (!ensureDirectory()) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("write_failed"),
                                categoryMessage(QStringLiteral("write_failed")));
            return;
        }
        QSaveFile file(filePath(hash));
        if (!file.open(QIODevice::WriteOnly)) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("write_failed"),
                                categoryMessage(QStringLiteral("write_failed")));
            return;
        }
        // Owner-only, explicitly — matches MediaBridge's decrypted-payload
        // hardening (QSaveFile otherwise honors the umask).
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("write_failed"),
                                categoryMessage(QStringLiteral("write_failed")));
            return;
        }
    }
    gif::GifResult r;
    r.provider = QStringLiteral("local");
    r.id = hash;
    r.gifWidth = v.width;
    r.gifHeight = v.height;
    r.gifBytes = bytes.size();
    m_model->insertLocal(r);
    m_mediaKeyToHash.insert(mediaKey, hash);
    // v0.6.6 fix (review M1): `existing` true means this content hash was
    // ALREADY indexed before this call (no bytes rewritten above) — the
    // durable-check-was-unknown activation path documented in the class
    // comment. Report it honestly rather than implying a brand new star.
    if (existing) {
        Q_EMIT starFinished(mediaKey, true, QStringLiteral("already_starred"),
                            categoryMessage(QStringLiteral("already_starred")));
    } else {
        Q_EMIT starFinished(mediaKey, true, QString(), QString());
    }
}

void GifStarredStore::unstar(const QString &hash)
{
    if (!isOpen() || !isSafeHashHex(hash))
        return;
    QFile::remove(filePath(hash));
    // Drop every session mapping that pointed at this hash BEFORE the model
    // mutation: m_model->unstar() emits countChanged synchronously, and a
    // consumer refreshing from that signal (the hover star does exactly
    // that) must not observe a map that still claims this hash is starred —
    // it would keep a filled star whose next activation RE-STARS the file
    // the user just removed. Same ordering rule as openFor(). The trailing
    // unstarFinished only fires when the row existed, so it cannot be
    // relied on as the refresh path.
    for (auto it = m_mediaKeyToHash.begin(); it != m_mediaKeyToHash.end();) {
        if (it.value() == hash)
            it = m_mediaKeyToHash.erase(it);
        else
            ++it;
    }
    const bool removed = m_model->unstar(hash);
    if (removed)
        Q_EMIT unstarFinished(hash);
}

void GifStarredStore::unstarByMediaKey(const QString &mediaKey)
{
    const QString hash = m_mediaKeyToHash.value(mediaKey);
    if (!hash.isEmpty())
        unstar(hash);
}

void GifStarredStore::clearAll()
{
    if (!isOpen() || m_model->count() == 0)
        return;
    for (int i = 0; i < m_model->count(); ++i) {
        const QString hash = m_model->resultAt(i).id;
        if (isSafeHashHex(hash))
            QFile::remove(filePath(hash));
    }
    // Clear the session map BEFORE the model mutation — m_model->clearAll()
    // emits countChanged synchronously and is the ONLY signal this path
    // produces (no per-hash unstarFinished). A consumer refreshing from it
    // (the hover star) would otherwise read a still-populated map and keep
    // a filled star whose next activation re-persists decrypted bytes the
    // user just explicitly deleted. openFor() already orders it this way.
    m_mediaKeyToHash.clear();
    m_model->clearAll();
}

QByteArray GifStarredStore::readBytes(const QString &hash) const
{
    if (!isOpen() || !isSafeHashHex(hash))
        return {};
    QFile file(filePath(hash));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    // Bounded read: never trust the file's on-disk size unconditionally,
    // even though it was validated at write time — it may have been
    // replaced since (e.g. swapped for a different, larger file at the same
    // path outside the app). Reject rather than reading an unbounded amount
    // into memory.
    if (file.size() > gif::kMaxGifBytes)
        return {};
    return file.readAll();
}

QString GifStarredStore::source(const QString &hash) const
{
    if (!isOpen() || !isSafeHashHex(hash))
        return {};
    const QString path = filePath(hash);
    if (!QFileInfo::exists(path))
        return {};
    return QUrl::fromLocalFile(path).toString();
}
