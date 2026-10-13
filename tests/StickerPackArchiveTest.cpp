// Sticker pack ZIP: what we write reads back identically, and what we read is
// untrusted: hostile names, SVG, lying extensions, oversized entries, bad
// manifests. Nothing here touches Matrix.

#include "stickers/StickerPackArchive.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace stickers::archive;

namespace {

const QByteArray kPng = QByteArray("\x89PNG\r\n\x1A\n", 8) + QByteArray(40, 'p');
const QByteArray kJpg = QByteArray("\xFF\xD8\xFF\xE0", 4) + QByteArray(40, 'j');
const QByteArray kWebp = QByteArray("RIFF\x30\0\0\0WEBP", 12) + QByteArray(40, 'w');
const QByteArray kGif = QByteArray("GIF89a", 6) + QByteArray(40, 'g');
const QByteArray kSvg = QByteArrayLiteral(
    "<svg xmlns='http://www.w3.org/2000/svg'><script>alert(1)</script></svg>");

quint32 crc(const QByteArray &data)
{
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data) {
        c ^= quint8(ch);
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return ~c;
}
void le16(QByteArray &b, quint16 v) { b.append(char(v)); b.append(char(v >> 8)); }
void le32(QByteArray &b, quint32 v) { le16(b, quint16(v)); le16(b, quint16(v >> 16)); }

// A hand-made stored ZIP that accepts ANY entry name, which the production
// writer never would.
QByteArray zipOf(const QList<QPair<QByteArray, QByteArray>> &entries)
{
    QByteArray out, central;
    for (const auto &e : entries) {
        const quint32 offset = quint32(out.size());
        const quint32 c = crc(e.second);
        le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 0);
        le16(out, 0); le16(out, 0x21); le32(out, c);
        le32(out, quint32(e.second.size())); le32(out, quint32(e.second.size()));
        le16(out, quint16(e.first.size())); le16(out, 0);
        out.append(e.first); out.append(e.second);
        le32(central, 0x02014b50); le16(central, 20); le16(central, 20);
        le16(central, 0); le16(central, 0); le16(central, 0); le16(central, 0x21);
        le32(central, c); le32(central, quint32(e.second.size()));
        le32(central, quint32(e.second.size()));
        le16(central, quint16(e.first.size())); le16(central, 0); le16(central, 0);
        le16(central, 0); le16(central, 0); le32(central, 0); le32(central, offset);
        central.append(e.first);
    }
    const quint32 centralOffset = quint32(out.size());
    out.append(central);
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0);
    le16(out, quint16(entries.size())); le16(out, quint16(entries.size()));
    le32(out, quint32(central.size())); le32(out, centralOffset); le16(out, 0);
    return out;
}

QByteArray manifest(const QJsonArray &images, const QString &format = QStringLiteral("lightning.sticker-pack"),
                    int version = 1)
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), format);
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("name"), QStringLiteral("Test pack"));
    o.insert(QStringLiteral("images"), images);
    return QJsonDocument(o).toJson();
}

QJsonObject imageEntry(const QString &code, const QString &file)
{
    QJsonObject o;
    o.insert(QStringLiteral("shortcode"), code);
    o.insert(QStringLiteral("file"), file);
    return o;
}

QString writeTemp(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(bytes);
    return path;
}

}  // namespace

class StickerPackArchiveTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void extensionsComeFromTheBytesAndSvgIsNeverOne()
    {
        QCOMPARE(extensionForBytes(kPng), QStringLiteral("png"));
        QCOMPARE(extensionForBytes(kJpg), QStringLiteral("jpg"));
        QCOMPARE(extensionForBytes(kWebp), QStringLiteral("webp"));
        QCOMPARE(extensionForBytes(kGif), QStringLiteral("gif"));
        QCOMPARE(extensionForBytes(kSvg), QString());
        QCOMPARE(extensionForBytes(QByteArray("\x1F\x8B", 2) + QByteArray(30, 'z')),
                 QString());  // SVGZ
        QCOMPARE(extensionForBytes(QByteArray()), QString());
    }

    void shortcodesAreRepairedLikeTheBackendDoes()
    {
        QCOMPARE(sanitizeShortcode(QStringLiteral("Cat Face!")), QStringLiteral("Cat_Face"));
        QCOMPARE(sanitizeShortcode(QStringLiteral("a__b")), QStringLiteral("a_b"));
        QCOMPARE(sanitizeShortcode(QStringLiteral("-x-")), QStringLiteral("x"));
        QCOMPARE(sanitizeShortcode(QStringLiteral(":cat:")), QStringLiteral("cat"));
        QCOMPARE(sanitizeShortcode(QStringLiteral("café")), QStringLiteral("caf"));
        QCOMPARE(sanitizeShortcode(QStringLiteral("a--b")), QStringLiteral("a--b"));
        QCOMPARE(sanitizeShortcode(QString()), QString());
        QVERIFY(sanitizeShortcode(QString(200, QLatin1Char('a'))).size() <= 64);
    }

    void aPackSurvivesAWriteAndARead()
    {
        Pack pack;
        pack.name = QStringLiteral("My pack");
        pack.attribution = QStringLiteral("by me");
        pack.images = {
            {QStringLiteral("cat"), QStringLiteral("A cat"), {}, QStringLiteral("sticker"), kPng, {}},
            {QStringLiteral("dog"), QStringLiteral("dog"), {}, QStringLiteral("emoticon"), kJpg, {}},
            {QStringLiteral("owl"), QStringLiteral("Owl"), {}, QStringLiteral("both"), kWebp, {}},
            {QStringLiteral("fox"), QStringLiteral("Fox"), {}, QStringLiteral("bogus"), kGif, {}},
        };
        int skipped = -1;
        const QByteArray zip = buildArchive(pack, &skipped);
        QCOMPARE(skipped, 0);

        QTemporaryDir dir;
        const QString zipPath = writeTemp(dir, QStringLiteral("p.zip"), zip);
        QTemporaryDir work;
        const ReadResult r = readArchive(zipPath, work.path());
        QCOMPARE(int(r.error), int(ReadError::None));
        QCOMPARE(r.refused, 0);
        QCOMPARE(r.pack.name, QStringLiteral("My pack"));
        QCOMPARE(r.pack.attribution, QStringLiteral("by me"));
        QCOMPARE(r.pack.images.size(), 4);
        QCOMPARE(r.pack.images[0].shortcode, QStringLiteral("cat"));
        QCOMPARE(r.pack.images[0].body, QStringLiteral("A cat"));
        QCOMPARE(r.pack.images[0].usage, QStringLiteral("sticker"));
        QCOMPARE(r.pack.images[1].usage, QStringLiteral("emoticon"));
        QCOMPARE(r.pack.images[2].usage, QStringLiteral("both"));
        // An unknown usage reads back as the conservative one.
        QCOMPARE(r.pack.images[3].usage, QStringLiteral("sticker"));
        QCOMPARE(r.pack.images[1].mimetype, QStringLiteral("image/jpeg"));
        QFile f(r.pack.images[0].path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), kPng);
    }

    void unusableImagesAreSkippedOnTheWayOut()
    {
        Pack pack;
        pack.images = {
            {QStringLiteral("ok"), {}, {}, QStringLiteral("sticker"), kPng, {}},
            {QStringLiteral("svg"), {}, {}, QStringLiteral("sticker"), kSvg, {}},
            {QStringLiteral("empty"), {}, {}, QStringLiteral("sticker"), QByteArray(), {}},
            {QStringLiteral("huge"), {}, {}, QStringLiteral("sticker"),
             kPng + QByteArray(int(kMaxImageBytes), 'x'), {}},
            {QStringLiteral("???"), {}, {}, QStringLiteral("sticker"), kPng, {}},
        };
        int skipped = 0;
        const QByteArray zip = buildArchive(pack, &skipped);
        QCOMPARE(skipped, 4);
        QVERIFY(!zip.contains("<svg"));
        QVERIFY(!zip.contains("script"));
    }

    void aNonZipIsUnreadable()
    {
        QTemporaryDir dir, work;
        const QString path = writeTemp(dir, QStringLiteral("x.zip"), QByteArray("not a zip at all, sorry"));
        QCOMPARE(int(readArchive(path, work.path()).error), int(ReadError::Unreadable));
        QCOMPARE(int(readArchive(dir.filePath(QStringLiteral("missing.zip")), work.path()).error),
                 int(ReadError::Unreadable));
    }

    void aTraversalNameIsRefusedBeforeAnythingIsWritten()
    {
        QTemporaryDir dir, work;
        const QByteArray zip = zipOf({
            {"pack.json", manifest({imageEntry(QStringLiteral("a"), QStringLiteral("images/a.png"))})},
            {"images/a.png", kPng},
            {"../escaped.png", kPng},
        });
        const QString path = writeTemp(dir, QStringLiteral("evil.zip"), zip);
        const ReadResult r = readArchive(path, work.path());
        QCOMPARE(int(r.error), int(ReadError::Unreadable));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("escaped.png"))));
        QVERIFY(!QFile::exists(QDir(work.path()).absoluteFilePath(QStringLiteral("../escaped.png"))));
    }

    void anSvgIsRefusedWhateverItIsCalled()
    {
        QTemporaryDir dir, work;
        const QByteArray zip = zipOf({
            {"pack.json", manifest({
                 imageEntry(QStringLiteral("a"), QStringLiteral("images/a.png")),
                 imageEntry(QStringLiteral("b"), QStringLiteral("images/b.svg")),
                 imageEntry(QStringLiteral("good"), QStringLiteral("images/good.png"))})},
            {"images/a.png", kSvg},     // lies about its extension
            {"images/b.svg", kSvg},
            {"images/good.png", kPng},
        });
        const ReadResult r = readArchive(writeTemp(dir, QStringLiteral("s.zip"), zip), work.path());
        QCOMPARE(int(r.error), int(ReadError::None));
        QCOMPARE(r.refused, 2);
        QCOMPARE(r.pack.images.size(), 1);
        QCOMPARE(r.pack.images[0].shortcode, QStringLiteral("good"));
    }

    void anAllSvgPackHasNoImages()
    {
        QTemporaryDir dir, work;
        const QByteArray zip = zipOf({
            {"pack.json", manifest({imageEntry(QStringLiteral("a"), QStringLiteral("images/a.svg"))})},
            {"images/a.svg", kSvg},
        });
        const ReadResult r = readArchive(writeTemp(dir, QStringLiteral("s.zip"), zip), work.path());
        QCOMPARE(int(r.error), int(ReadError::NoImages));
        QCOMPARE(r.refused, 1);
    }

    void manifestEntriesCannotPointOutsideImages()
    {
        QTemporaryDir dir, work;
        const QByteArray zip = zipOf({
            {"pack.json", manifest({
                 imageEntry(QStringLiteral("a"), QStringLiteral("../pack.json")),
                 imageEntry(QStringLiteral("b"), QStringLiteral("/etc/passwd")),
                 imageEntry(QStringLiteral("c"), QStringLiteral("images/../pack.json")),
                 imageEntry(QStringLiteral("d"), QStringLiteral("images/sub/d.png")),
                 imageEntry(QStringLiteral("e"), QStringLiteral("images\\e.png")),
                 imageEntry(QStringLiteral("f"), QStringLiteral("images/missing.png")),
                 imageEntry(QStringLiteral("g"), QStringLiteral("images/g.png"))})},
            {"images/g.png", kPng},
        });
        const ReadResult r = readArchive(writeTemp(dir, QStringLiteral("o.zip"), zip), work.path());
        QCOMPARE(int(r.error), int(ReadError::None));
        QCOMPARE(r.refused, 6);
        QCOMPARE(r.pack.images.size(), 1);
        QVERIFY(r.pack.images[0].path.startsWith(work.path()));
    }

    void aBadManifestIsNamedAsSuch()
    {
        QTemporaryDir dir, work1, work2, work3, work4;
        QCOMPARE(int(readArchive(writeTemp(dir, QStringLiteral("1.zip"), zipOf({{"images/a.png", kPng}})),
                                 work1.path()).error), int(ReadError::NoManifest));
        QCOMPARE(int(readArchive(writeTemp(dir, QStringLiteral("2.zip"), zipOf({{"pack.json", "{not json"}})),
                                 work2.path()).error), int(ReadError::BadManifest));
        QCOMPARE(int(readArchive(writeTemp(dir, QStringLiteral("3.zip"),
                                           zipOf({{"pack.json", manifest({}, QStringLiteral("somebody.else"))}})),
                                 work3.path()).error), int(ReadError::BadManifest));
        // A newer format is refused, not guessed at.
        QCOMPARE(int(readArchive(writeTemp(dir, QStringLiteral("4.zip"),
                                           zipOf({{"pack.json", manifest({}, QStringLiteral("lightning.sticker-pack"), 2)}})),
                                 work4.path()).error), int(ReadError::BadManifest));
    }

    void namesAreRepairedAndDuplicatesAreRefused()
    {
        QTemporaryDir dir, work;
        const QByteArray zip = zipOf({
            {"pack.json", manifest({
                 imageEntry(QStringLiteral("Cat Face!"), QStringLiteral("images/a.png")),
                 imageEntry(QStringLiteral("cat_face"), QStringLiteral("images/b.png")),
                 imageEntry(QStringLiteral("???"), QStringLiteral("images/c.png"))})},
            {"images/a.png", kPng}, {"images/b.png", kPng}, {"images/c.png", kJpg},
        });
        const ReadResult r = readArchive(writeTemp(dir, QStringLiteral("n.zip"), zip), work.path());
        QCOMPARE(int(r.error), int(ReadError::None));
        QCOMPARE(r.refused, 1);  // b clashes with a, case-insensitively
        QCOMPARE(r.pack.images.size(), 2);
        QCOMPARE(r.pack.images[0].shortcode, QStringLiteral("Cat_Face"));
        // An unusable name falls back to the file's own.
        QCOMPARE(r.pack.images[1].shortcode, QStringLiteral("c"));
    }

    void anOversizedEntryEndsTheReadWithNothingKept()
    {
        QTemporaryDir dir, work;
        const QByteArray zip = zipOf({
            {"pack.json", manifest({imageEntry(QStringLiteral("a"), QStringLiteral("images/a.png"))})},
            {"images/a.png", kPng + QByteArray(int(kMaxImageBytes), 'x')},
        });
        QCOMPARE(int(readArchive(writeTemp(dir, QStringLiteral("big.zip"), zip), work.path()).error),
                 int(ReadError::Unreadable));
    }

    void tooManyImagesAreCappedNotTrusted()
    {
        QTemporaryDir dir, work;
        QJsonArray images;
        QList<QPair<QByteArray, QByteArray>> entries;
        for (int i = 0; i < kMaxImages + 5; ++i) {
            const QString file = QStringLiteral("images/i%1.png").arg(i);
            images.append(imageEntry(QStringLiteral("i%1").arg(i), file));
            entries.append({file.toUtf8(), kPng});
        }
        entries.prepend({"pack.json", manifest(images)});
        const ReadResult r = readArchive(writeTemp(dir, QStringLiteral("many.zip"), zipOf(entries)), work.path());
        // Over the entry cap the extractor itself refuses the archive.
        QVERIFY(r.error == ReadError::Unreadable
                || r.pack.images.size() <= kMaxImages);
    }
};

QTEST_APPLESS_MAIN(StickerPackArchiveTest)
#include "StickerPackArchiveTest.moc"
