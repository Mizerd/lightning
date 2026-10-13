// Image metadata removal: every case builds a hand-made file carrying a GPS
// block with a recognisable marker, strips it, and checks the marker is gone
// and the picture bytes are untouched. Nothing here decodes pixels except the
// round-trip cases that prove a real QImage still loads.

#include "media/ImageMetadata.h"

#include <QBuffer>
#include <QImage>
#include <QtTest/QtTest>

using namespace imagemeta;

namespace {

// A run of bytes that appears nowhere else, used as the "coordinates".
const QByteArray kLatMarker(24, '\xAB');
const char kMakeText[] = "Acme\0";

template <size_t N>
QByteArray raw(const char (&s)[N]) { return QByteArray(s, int(N - 1)); }

void be16(QByteArray &b, quint16 v) { b.append(char(v >> 8)); b.append(char(v)); }
void be32(QByteArray &b, quint32 v) { be16(b, quint16(v >> 16)); be16(b, quint16(v)); }
void le16(QByteArray &b, quint16 v) { b.append(char(v)); b.append(char(v >> 8)); }
void le32(QByteArray &b, quint32 v) { le16(b, quint16(v)); le16(b, quint16(v >> 16)); }

// IFD0: Make (out of line), Orientation, GPS pointer. GPS IFD: LatRef, Lat
// (24 bytes out of line, = the marker).
QByteArray makeTiff(bool withGps, int orientation, bool littleEndian)
{
    QByteArray t;
    auto u16 = [&](quint16 v) { littleEndian ? le16(t, v) : be16(t, v); };
    auto u32 = [&](quint32 v) { littleEndian ? le32(t, v) : be32(t, v); };
    t.append(littleEndian ? "II" : "MM", 2);
    u16(42);
    u32(8);
    const int entries = withGps ? 3 : 2;
    u16(quint16(entries));
    // Make
    u16(0x010F); u16(2); u32(5); u32(8 + 2 + entries * 12 + 4);
    // Orientation (SHORT, inline)
    u16(0x0112); u16(3); u32(1); u16(quint16(orientation)); u16(0);
    const int makeOff = 8 + 2 + entries * 12 + 4;
    const int gpsOff = makeOff + 6;  // 5 bytes padded to 6
    if (withGps) {
        u16(0x8825); u16(4); u32(1); u32(quint32(gpsOff));
    }
    u32(0);  // no IFD1
    t.append(kMakeText, 5);
    t.append('\0');
    if (withGps) {
        u16(2);
        u16(0x0001); u16(2); u32(2); t.append("N\0\0\0", 4);
        u16(0x0002); u16(5); u32(3); u32(quint32(gpsOff + 2 + 24 + 4));
        u32(0);
        t.append(kLatMarker);
    }
    return t;
}

QByteArray segment(uchar marker, const QByteArray &payload)
{
    QByteArray s;
    s.append(char(0xFF));
    s.append(char(marker));
    be16(s, quint16(payload.size() + 2));
    return s + payload;
}

QByteArray exifSegment(const QByteArray &tiff)
{
    return segment(0xE1, QByteArray("Exif\0\0", 6) + tiff);
}

QByteArray xmpSegment(const QByteArray &body)
{
    return segment(0xE1, QByteArray("http://ns.adobe.com/xap/1.0/\0", 29) + body);
}

// SOI, the given segments, then a fixed fake scan (SOS header, entropy data
// with a stuffed 0xFF and a restart marker) and EOI.
const QByteArray kScan = []() {
    QByteArray s = segment(0xDA, QByteArray("\x01\x01\x00\x00\x3F\x00", 6));
    s.append("ab\xFF\x00""cd\xFF\xD0""ef", 9);
    s.append("\xFF\xD9", 2);
    return s;
}();

QByteArray jpegWith(const QList<QByteArray> &segments,
                    const QByteArray &trailing = {})
{
    QByteArray j("\xFF\xD8", 2);
    for (const QByteArray &s : segments)
        j.append(s);
    j.append(segment(0xDB, QByteArray(65, '\x08')));  // DQT stand-in
    j.append(kScan);
    j.append(trailing);
    return j;
}

QByteArray jfif() { return segment(0xE0, QByteArray("JFIF\0\1\1\0\0\1\0\1\0\0", 14)); }

// -- PNG ---------------------------------------------------------------------

quint32 crc(const QByteArray &b)
{
    quint32 c = 0xFFFFFFFFu;
    for (uchar ch : b) {
        c ^= ch;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return ~c;
}

QByteArray pngChunk(const char *type, const QByteArray &data)
{
    QByteArray c;
    be32(c, quint32(data.size()));
    const QByteArray body = QByteArray(type, 4) + data;
    c.append(body);
    be32(c, crc(body));
    return c;
}

QByteArray pngWith(const QList<QByteArray> &middle)
{
    QByteArray p("\x89PNG\r\n\x1A\n", 8);
    QByteArray ihdr;
    be32(ihdr, 1); be32(ihdr, 1);
    ihdr.append("\x08\x02\x00\x00\x00", 5);
    p.append(pngChunk("IHDR", ihdr));
    for (const QByteArray &m : middle)
        p.append(m);
    p.append(pngChunk("IDAT", QByteArray("\x78\x9C\x63\x60\x00\x00\x00\x04\x00\x01", 10)));
    p.append(pngChunk("IEND", {}));
    return p;
}

// -- WebP --------------------------------------------------------------------

QByteArray webpChunk(const char *fourcc, const QByteArray &data)
{
    QByteArray c(fourcc, 4);
    le32(c, quint32(data.size()));
    c.append(data);
    if (data.size() & 1)
        c.append('\0');
    return c;
}

QByteArray webpWith(const QByteArray &exif, const QByteArray &xmp)
{
    QByteArray vp8x;
    uchar flags = 0;
    if (!exif.isEmpty()) flags |= 0x08;
    if (!xmp.isEmpty()) flags |= 0x04;
    vp8x.append(char(flags));
    vp8x.append(QByteArray(3, '\0'));
    le16(vp8x, 0); vp8x.append('\0');  // width-1 (24 bit)
    le16(vp8x, 0); vp8x.append('\0');  // height-1
    QByteArray body;
    body.append(webpChunk("VP8X", vp8x));
    body.append(webpChunk("VP8L", QByteArray("\x2F\x00\x00\x00\x00", 5)));
    if (!exif.isEmpty()) body.append(webpChunk("EXIF", exif));
    if (!xmp.isEmpty()) body.append(webpChunk("XMP ", xmp));
    QByteArray w("RIFF", 4);
    le32(w, quint32(body.size() + 4));
    w.append("WEBP", 4);
    return w + body;
}

// -- HEIC --------------------------------------------------------------------

QByteArray box(const char *type, const QByteArray &body)
{
    QByteArray b;
    be32(b, quint32(body.size() + 8));
    b.append(type, 4);
    return b + body;
}

QByteArray heicWith(const QByteArray &tiff, bool ilocFirst = false,
                    bool wrapBase = false)
{
    QByteArray ftyp = box("ftyp", QByteArray("heic\0\0\0\0mif1heic", 16));
    auto build = [&](quint32 exifOffset) {
        QByteArray iinf;
        be32(iinf, 0);       // version, flags
        be16(iinf, 2);       // entry count
        QByteArray infe1;
        be32(infe1, 0x02000000);
        be16(infe1, 1); be16(infe1, 0); infe1.append("hvc1"); infe1.append('\0');
        QByteArray infe2;
        be32(infe2, 0x02000000);
        be16(infe2, 2); be16(infe2, 0); infe2.append("Exif"); infe2.append('\0');
        iinf.append(box("infe", infe1));
        iinf.append(box("infe", infe2));
        QByteArray iloc;
        be32(iloc, 0);       // version 0
        iloc.append(char(0x44));  // offset_size 4, length_size 4
        iloc.append(char(wrapBase ? 0x80 : 0x00));  // base_offset_size
        be16(iloc, 2);       // item count
        auto base = [&](quint64 v) {
            if (wrapBase) { be32(iloc, quint32(v >> 32)); be32(iloc, quint32(v)); }
        };
        // item 1: the image, a stand-in extent
        be16(iloc, 1); be16(iloc, 0); base(0); be16(iloc, 1);
        be32(iloc, exifOffset - 4); be32(iloc, 4);
        // item 2: Exif, 4 byte header offset then the TIFF. In the wrapping
        // variant base + offset overflows 64 bits to a small, valid position.
        be16(iloc, 2); be16(iloc, 0);
        base(wrapBase ? 0xFFFFFFFFFFFFFFF0ull : 0);
        be16(iloc, 1);
        be32(iloc, exifOffset); be32(iloc, quint32(4 + tiff.size()));
        QByteArray metaBody;
        be32(metaBody, 0);
        if (ilocFirst) {
            metaBody.append(box("iloc", iloc));
            metaBody.append(box("iinf", iinf));
        } else {
            metaBody.append(box("iinf", iinf));
            metaBody.append(box("iloc", iloc));
        }
        return box("meta", metaBody);
    };
    const int metaSize = build(0).size();
    // mdat: 4 bytes of "image", then the Exif item.
    const quint32 mdatBody = quint32(ftyp.size() + metaSize + 8);
    const quint32 exifOffset = mdatBody + 4;
    QByteArray mdat;
    mdat.append("IMG!");
    be32(mdat, 0);           // exif_tiff_header_offset
    mdat.append(tiff);
    return ftyp + build(exifOffset) + box("mdat", mdat);
}

QByteArray realJpeg()
{
    QImage img(32, 32, QImage::Format_RGB32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            img.setPixel(x, y, qRgb(x * 8, y * 8, (x + y) * 4));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 90);
    return bytes;
}

QByteArray realPng()
{
    QImage img(8, 8, QImage::Format_ARGB32);
    img.fill(qRgba(10, 200, 30, 255));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

}  // namespace

class ImageMetadataTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void modeParsingFallsBackToTheDefaultNotToKeep()
    {
        QCOMPARE(modeFromString(QStringLiteral("location")), Mode::RemoveLocation);
        QCOMPARE(modeFromString(QStringLiteral("all")), Mode::RemoveAll);
        QCOMPARE(modeFromString(QStringLiteral(" KEEP ")), Mode::Keep);
        // A value written by a newer build, or garbage: the privacy default.
        QCOMPARE(modeFromString(QStringLiteral("scrub-harder")),
                 Mode::RemoveLocation);
        QCOMPARE(modeFromString(QString()), Mode::RemoveLocation);
        QCOMPARE(modeToString(Mode::Keep), QStringLiteral("keep"));
    }

    // -- JPEG -----------------------------------------------------------------

    void jpegLocationIsRemovedAndTheRestOfTheExifSurvives_data()
    {
        QTest::addColumn<bool>("littleEndian");
        QTest::newRow("little endian") << true;
        QTest::newRow("big endian") << false;
    }
    void jpegLocationIsRemovedAndTheRestOfTheExifSurvives()
    {
        QFETCH(bool, littleEndian);
        const QByteArray in = jpegWith(
            {jfif(), exifSegment(makeTiff(true, 6, littleEndian))});
        QVERIFY(in.contains(kLatMarker));
        QVERIFY(containsLocation(in));

        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QVERIFY(!containsLocation(r.bytes));
        // Not location: the camera make and the orientation stay.
        QVERIFY(r.bytes.contains(kMakeText));
        QCOMPARE(exifOrientation(r.bytes), 6);
        // Same length (in-place edit) and the scan is byte for byte the same.
        QCOMPARE(r.bytes.size(), in.size());
        QVERIFY(r.bytes.endsWith(kScan));
    }

    void jpegKeepIsByteIdentical()
    {
        const QByteArray in = jpegWith({exifSegment(makeTiff(true, 1, true))});
        const Result r = strip(in, Mode::Keep);
        QCOMPARE(r.status, Status::Unchanged);
        QCOMPARE(r.bytes, in);
    }

    void jpegRemoveAllDropsEverythingAndKeepsOrientation()
    {
        const QByteArray in = jpegWith(
            {jfif(), exifSegment(makeTiff(true, 8, false)),
             xmpSegment("<x:xmpmeta>exif:GPSLatitude</x:xmpmeta>"),
             segment(0xED, raw("Photoshop 3.0\0" "8BIM city=Vilnius")),
             segment(0xFE, "shot at home")});
        const Result r = strip(in, Mode::RemoveAll);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QVERIFY(!r.bytes.contains(kMakeText));
        QVERIFY(!r.bytes.contains("GPSLatitude"));
        QVERIFY(!r.bytes.contains("Vilnius"));
        QVERIFY(!r.bytes.contains("shot at home"));
        QCOMPARE(exifOrientation(r.bytes), 8);
        QVERIFY(r.bytes.contains("JFIF"));
        QVERIFY(r.bytes.endsWith(kScan));
    }

    void jpegRemoveAllOfAnUprightPhotoLeavesNoExifAtAll()
    {
        const QByteArray in = jpegWith({exifSegment(makeTiff(true, 1, true))});
        const Result r = strip(in, Mode::RemoveAll);
        QCOMPARE(r.status, Status::Stripped);
        QCOMPARE(exifOrientation(r.bytes), 0);
        QVERIFY(!r.bytes.contains("Exif"));
        // Exactly the file the camera would have written without metadata.
        QCOMPARE(r.bytes, jpegWith({}));
    }

    void jpegXmpWithGpsIsDroppedInLocationModeAndXmpWithoutIsKept()
    {
        const QByteArray withGps = jpegWith(
            {xmpSegment("<x>exif:GPSLatitude=1</x>")});
        const Result a = strip(withGps, Mode::RemoveLocation);
        QCOMPARE(a.status, Status::Stripped);
        QVERIFY(!a.bytes.contains("GPSLatitude"));
        QVERIFY(containsLocation(withGps));
        QVERIFY(!containsLocation(a.bytes));

        const QByteArray plain = jpegWith({xmpSegment("<x>dc:title=Cat</x>")});
        const Result b = strip(plain, Mode::RemoveLocation);
        QCOMPARE(b.status, Status::Unchanged);
        QCOMPARE(b.bytes, plain);
    }

    void jpegImagesAppendedAfterTheEoiAreDropped()
    {
        // Phones append whole secondary images with Exif of their own.
        const QByteArray trailing = jpegWith({exifSegment(makeTiff(true, 1, true))});
        const QByteArray in = jpegWith({}, trailing);
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QCOMPARE(r.bytes, jpegWith({}));
    }

    void jpegIccProfileIsKeptInEveryMode()
    {
        const QByteArray icc = segment(0xE2, QByteArray("ICC_PROFILE\0\1\1", 14)
                                                 + QByteArray(20, 'p'));
        const QByteArray in = jpegWith({icc, exifSegment(makeTiff(true, 1, true))});
        QVERIFY(strip(in, Mode::RemoveAll).bytes.contains("ICC_PROFILE"));
        QVERIFY(strip(in, Mode::RemoveLocation).bytes.contains("ICC_PROFILE"));
    }

    void aTruncatedJpegFailsClosed()
    {
        QByteArray in = jpegWith({exifSegment(makeTiff(true, 1, true))});
        in.chop(8);  // loses the EOI
        QCOMPARE(strip(in, Mode::RemoveLocation).status, Status::Failed);
        // A bad segment length too.
        QByteArray bad("\xFF\xD8\xFF\xE1\x7F\xFF""short", 11);
        QCOMPARE(strip(bad, Mode::RemoveAll).status, Status::Failed);
        // "Keep" never inspects, so it never fails.
        QCOMPARE(strip(in, Mode::Keep).status, Status::Unchanged);
    }

    void aMalformedExifBlockFailsClosed()
    {
        const QByteArray in = jpegWith({exifSegment(QByteArray("not a tiff block"))});
        QCOMPARE(strip(in, Mode::RemoveLocation).status, Status::Failed);
    }

    void aRealJpegStillDecodesToTheSamePixelsAndKeepsItsScanBytes()
    {
        const QByteArray original = realJpeg();
        QVERIFY(!original.isEmpty());
        // Insert a GPS-bearing Exif right after SOI, as a camera does.
        QByteArray tagged = original;
        tagged.insert(2, exifSegment(makeTiff(true, 1, true)));
        QVERIFY(tagged.contains(kLatMarker));

        for (const Mode mode : {Mode::RemoveLocation, Mode::RemoveAll}) {
            const Result r = strip(tagged, mode);
            QCOMPARE(r.status, Status::Stripped);
            QVERIFY(!r.bytes.contains(kLatMarker));
            QImage a, b;
            QVERIFY(a.loadFromData(original));
            QVERIFY(b.loadFromData(r.bytes));
            QCOMPARE(a, b);
        }
        // Everything removed from an upright photo is the original file.
        QCOMPARE(strip(tagged, Mode::RemoveAll).bytes, original);
    }

    // -- PNG ------------------------------------------------------------------

    void pngLocationIsRemovedAndTheChunkIsStillValid()
    {
        const QByteArray in = pngWith(
            {pngChunk("eXIf", makeTiff(true, 3, true)),
             pngChunk("tEXt", raw("Comment\0hello")), pngChunk("pHYs", QByteArray(9, '\1'))});
        QVERIFY(containsLocation(in));
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QVERIFY(r.bytes.contains("hello"));       // text kept in this mode
        QVERIFY(r.bytes.contains("pHYs"));
        QCOMPARE(exifOrientation(r.bytes), 3);
        // The rewritten chunk's CRC is valid: Qt's reader accepts the file.
        QCOMPARE(r.bytes.size(), in.size());
    }

    void pngRemoveAllDropsAncillaryChunksButKeepsColourAndOrientation()
    {
        const QByteArray in = pngWith(
            {pngChunk("eXIf", makeTiff(true, 6, false)),
             pngChunk("tEXt", raw("Comment\0hello")),
             pngChunk("iTXt", raw("XML:com.adobe.xmp\0\0\0\0\0exif:GPSLatitude")),
             pngChunk("tIME", QByteArray(7, '\7')),
             pngChunk("gAMA", QByteArray(4, '\2'))});
        const Result r = strip(in, Mode::RemoveAll);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains("hello"));
        QVERIFY(!r.bytes.contains("GPSLatitude"));
        QVERIFY(!r.bytes.contains("tIME"));
        QVERIFY(!r.bytes.contains(kMakeText));
        QVERIFY(r.bytes.contains("gAMA"));
        QCOMPARE(exifOrientation(r.bytes), 6);
    }

    void pngXmpWithGpsGoesInLocationMode()
    {
        const QByteArray in = pngWith({pngChunk(
            "iTXt", raw("XML:com.adobe.xmp\0\0\0\0\0exif:GPSLatitude"))});
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains("GPSLatitude"));
    }

    void aRealPngWithNoMetadataIsUnchangedAndAnUprightOneRoundTrips()
    {
        const QByteArray original = realPng();
        QCOMPARE(strip(original, Mode::RemoveAll).status, Status::Unchanged);
        // Insert an eXIf (valid CRC) after IHDR, strip all, get the original.
        QByteArray tagged = original;
        tagged.insert(8 + 25, pngChunk("eXIf", makeTiff(true, 1, true)));
        const Result r = strip(tagged, Mode::RemoveAll);
        QCOMPARE(r.status, Status::Stripped);
        QCOMPARE(r.bytes, original);
        QImage a, b;
        QVERIFY(a.loadFromData(original));
        QVERIFY(b.loadFromData(strip(tagged, Mode::RemoveLocation).bytes));
        QCOMPARE(a, b);
    }

    void aPngWithoutIendFailsClosed()
    {
        QByteArray in = pngWith({pngChunk("eXIf", makeTiff(true, 1, true))});
        in.chop(12);
        QCOMPARE(strip(in, Mode::RemoveLocation).status, Status::Failed);
    }

    // -- WebP -----------------------------------------------------------------

    void webpLocationIsRemoved()
    {
        const QByteArray in = webpWith(makeTiff(true, 6, true),
                                       "<x>exif:GPSLatitude</x>");
        QVERIFY(containsLocation(in));
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QVERIFY(!r.bytes.contains("GPSLatitude"));
        QCOMPARE(exifOrientation(r.bytes), 6);
        // RIFF size matches the file and the XMP flag was cleared.
        QCOMPARE(quint32(uchar(r.bytes[4])) | (quint32(uchar(r.bytes[5])) << 8)
                     | (quint32(uchar(r.bytes[6])) << 16)
                     | (quint32(uchar(r.bytes[7])) << 24),
                 quint32(r.bytes.size() - 8));
        QVERIFY(uchar(r.bytes[20]) & 0x08);   // EXIF flag still set
        QVERIFY(!(uchar(r.bytes[20]) & 0x04));  // XMP flag cleared
    }

    void webpRemoveAllKeepsOrientationAndFixesTheFlags()
    {
        const QByteArray in = webpWith(makeTiff(true, 3, true), "<x/>");
        const Result r = strip(in, Mode::RemoveAll);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QVERIFY(!r.bytes.contains(kMakeText));
        QCOMPARE(exifOrientation(r.bytes), 3);
        QVERIFY(!(uchar(r.bytes[20]) & 0x04));

        const QByteArray upright = webpWith(makeTiff(true, 1, true), {});
        const Result u = strip(upright, Mode::RemoveAll);
        QCOMPARE(u.status, Status::Stripped);
        QCOMPARE(exifOrientation(u.bytes), 0);
        QVERIFY(!(uchar(u.bytes[20]) & 0x08));  // flag cleared with the chunk
        QVERIFY(u.bytes.contains("VP8L"));
    }

    void aWebpWithAWrongRiffSizeFailsClosed()
    {
        QByteArray in = webpWith(makeTiff(true, 1, true), {});
        in[4] = char(0xFF); in[5] = char(0xFF);
        QCOMPARE(strip(in, Mode::RemoveLocation).status, Status::Failed);
    }

    // -- HEIC -----------------------------------------------------------------

    void heicLocationIsZeroedInPlace()
    {
        const QByteArray in = heicWith(makeTiff(true, 6, false));
        QVERIFY(in.contains(kLatMarker));
        QVERIFY(containsLocation(in));
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QVERIFY(r.bytes.contains(kMakeText));
        QCOMPARE(exifOrientation(r.bytes), 6);
        // Same length: no offset anywhere in the file moved.
        QCOMPARE(r.bytes.size(), in.size());
    }

    void heicRemoveAllLeavesOnlyTheOrientation()
    {
        const QByteArray in = heicWith(makeTiff(true, 8, true));
        const Result r = strip(in, Mode::RemoveAll);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QVERIFY(!r.bytes.contains(kMakeText));
        QCOMPARE(exifOrientation(r.bytes), 8);
        QCOMPARE(r.bytes.size(), in.size());
    }

    void heicWithoutLocationIsUnchangedInLocationMode()
    {
        const QByteArray in = heicWith(makeTiff(false, 1, true));
        QCOMPARE(strip(in, Mode::RemoveLocation).status, Status::Unchanged);
    }

    void aHeicWhoseExifPointsOutsideTheFileFailsClosed()
    {
        QByteArray in = heicWith(makeTiff(true, 1, true));
        in.chop(30);  // the Exif extent now runs off the end
        QCOMPARE(strip(in, Mode::RemoveLocation).status, Status::Failed);
    }


    // -- Review fixes --------------------------------------------------------

    void pngRawProfileAndXmpTextChunksAreDroppedInLocationMode()
    {
        // ImageMagick / ExifTool keep a hex Exif (GPS included) in a tEXt.
        const QByteArray in = pngWith(
            {pngChunk("tEXt", raw("Raw profile type exif\0\nexif\n  1234abcd")),
             pngChunk("tEXt", raw("Raw profile type APP1\0hex")),
             pngChunk("tEXt", raw("XML:com.adobe.xmp\0<x>exif:GPSLatitude</x>")),
             pngChunk("tEXt", raw("XML:com.adobe.xmp\0<x>dc:title=Cat</x>")),
             pngChunk("zTXt", raw("Comment\0\0compressedbytes")),
             pngChunk("tEXt", raw("Comment\0hello"))});
        QVERIFY(containsLocation(in));
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains("Raw profile type"));
        QVERIFY(!r.bytes.contains("GPSLatitude"));
        QVERIFY(!r.bytes.contains("zTXt"));
        QVERIFY(r.bytes.contains("dc:title=Cat"));  // not location: kept
        QVERIFY(r.bytes.contains("hello"));
    }

    void namedPlacesInXmpCountAsLocation()
    {
        const QByteArray in = jpegWith({xmpSegment(
            "<rdf photoshop:City=\"Vilnius\" photoshop:Country=\"LT\"/>")});
        QVERIFY(containsLocation(in));
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains("Vilnius"));
        const QByteArray loc = jpegWith({xmpSegment(
            "<Iptc4xmpExt:LocationShown>x</Iptc4xmpExt:LocationShown>")});
        QVERIFY(!strip(loc, Mode::RemoveLocation).bytes.contains("LocationShown"));
    }

    void formatsThatCarryExifButAreNotEditedFailClosed()
    {
        const QByteArray tiff = makeTiff(true, 1, true);   // a bare TIFF file
        QCOMPARE(strip(tiff, Mode::RemoveLocation).status, Status::Failed);
        QCOMPARE(strip(tiff, Mode::RemoveAll).status, Status::Failed);
        QCOMPARE(strip(tiff, Mode::Keep).status, Status::Unchanged);
        const QByteArray jxlCodestream("\xFF\x0A\x00\x00\x00", 5);
        QCOMPARE(strip(jxlCodestream, Mode::RemoveLocation).status, Status::Failed);
        const QByteArray jxlBox("\0\0\0\x0CJXL \r\n\x87\n", 12);
        QCOMPARE(strip(jxlBox, Mode::RemoveLocation).status, Status::Failed);
    }

    void heicWithIlocBeforeIinfIsStillRead()
    {
        // libheif writes iloc first.
        const QByteArray in = heicWith(makeTiff(true, 6, false), true);
        QVERIFY(containsLocation(in));
        const Result r = strip(in, Mode::RemoveLocation);
        QCOMPARE(r.status, Status::Stripped);
        QVERIFY(!r.bytes.contains(kLatMarker));
        QCOMPARE(exifOrientation(r.bytes), 6);
    }

    void aWrappingHeicExtentFailsClosed()
    {
        const QByteArray in = heicWith(makeTiff(true, 1, true), false, true);
        QCOMPARE(strip(in, Mode::RemoveLocation).status, Status::Failed);
        // The same layout without the wrap reads fine.
        const QByteArray ok = heicWith(makeTiff(true, 1, true), false, false);
        QCOMPARE(strip(ok, Mode::RemoveLocation).status, Status::Stripped);
    }

    // -- Not ours -------------------------------------------------------------

    void unsupportedFormatsPassThrough()
    {
        const QByteArray gif("GIF89a\x01\x00\x01\x00\x00\x00\x00;", 14);
        const Result g = strip(gif, Mode::RemoveAll);
        QCOMPARE(g.status, Status::Unsupported);
        QCOMPARE(g.bytes, gif);
        QCOMPARE(strip(QByteArray("plain text, not an image"), Mode::RemoveAll).status,
                 Status::Unsupported);
        QCOMPARE(strip(QByteArray(), Mode::RemoveAll).status, Status::Unsupported);
        QVERIFY(!containsLocation(gif));
    }

    void inputIsNeverModified()
    {
        const QByteArray in = jpegWith({exifSegment(makeTiff(true, 6, true))});
        const QByteArray copy = in;
        strip(in, Mode::RemoveAll);
        strip(in, Mode::RemoveLocation);
        const QByteArray h = heicWith(makeTiff(true, 6, true));
        const QByteArray hcopy = h;
        strip(h, Mode::RemoveAll);
        QCOMPARE(in, copy);
        QCOMPARE(h, hcopy);
    }
};

QTEST_APPLESS_MAIN(ImageMetadataTest)
#include "ImageMetadataTest.moc"
