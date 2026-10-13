#include "media/ImageMetadata.h"

#include <QList>

#include <cstring>

namespace imagemeta {
namespace {

// What a worker learned, beyond the bytes it produced. Read by the inspection
// helpers, so "does this file carry a location" and "what is its orientation"
// are answered by the very code that strips, not by a second parser that could
// disagree with it.
struct Ctx {
    Mode mode = Mode::RemoveLocation;
    bool foundLocation = false;
    int orientation = 0;  // first readable Exif orientation, 0 = none
};

// -- byte helpers ------------------------------------------------------------

inline quint16 be16(const uchar *p) { return quint16((p[0] << 8) | p[1]); }
inline quint32 be32(const uchar *p)
{
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8)
           | quint32(p[3]);
}
inline quint32 le32(const uchar *p)
{
    return (quint32(p[3]) << 24) | (quint32(p[2]) << 16) | (quint32(p[1]) << 8)
           | quint32(p[0]);
}
inline void putBe16(QByteArray &b, quint16 v)
{
    b.append(char(v >> 8));
    b.append(char(v & 0xFF));
}
inline void putBe32(QByteArray &b, quint32 v)
{
    putBe16(b, quint16(v >> 16));
    putBe16(b, quint16(v & 0xFFFF));
}
inline void putLe32(QByteArray &b, quint32 v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 24) & 0xFF));
}

bool contains(const QByteArray &hay, const char *needle)
{
    return hay.contains(needle);
}

// -- TIFF (the body of Exif) -------------------------------------------------

struct Tiff {
    uchar *d = nullptr;
    qsizetype n = 0;
    bool le = false;

    quint16 u16(qsizetype o) const
    {
        return le ? quint16(d[o] | (d[o + 1] << 8)) : be16(d + o);
    }
    quint32 u32(qsizetype o) const
    {
        return le ? le32(d + o) : be32(d + o);
    }
    void setU16(qsizetype o, quint16 v)
    {
        if (le) {
            d[o] = uchar(v & 0xFF);
            d[o + 1] = uchar(v >> 8);
        } else {
            d[o] = uchar(v >> 8);
            d[o + 1] = uchar(v & 0xFF);
        }
    }
    bool inb(qsizetype o, qsizetype len) const
    {
        return o >= 0 && len >= 0 && o <= n && len <= n - o;
    }
};

bool openTiff(uchar *d, qsizetype n, Tiff &t)
{
    if (n < 8)
        return false;
    if (d[0] == 'I' && d[1] == 'I' && d[2] == 0x2A && d[3] == 0x00)
        t.le = true;
    else if (d[0] == 'M' && d[1] == 'M' && d[2] == 0x00 && d[3] == 0x2A)
        t.le = false;
    else
        return false;
    t.d = d;
    t.n = n;
    return true;
}

constexpr quint16 kTagOrientation = 0x0112;
constexpr quint16 kTagGpsIfd = 0x8825;

qsizetype typeSize(quint16 type)
{
    switch (type) {
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 1;
    }
}

// Validates an IFD at `off` and returns its entry count, or -1.
int ifdCount(const Tiff &t, qsizetype off)
{
    if (!t.inb(off, 2))
        return -1;
    const int count = t.u16(off);
    if (!t.inb(off, 2 + qsizetype(count) * 12 + 4))
        return -1;
    return count;
}

// The IFD chain (IFD0, IFD1, ...), bounded so a looping file ends.
QList<qsizetype> ifdChain(const Tiff &t)
{
    QList<qsizetype> chain;
    if (!t.inb(4, 4))
        return chain;
    qsizetype off = qsizetype(t.u32(4));
    while (off != 0 && chain.size() < 4 && !chain.contains(off)) {
        const int c = ifdCount(t, off);
        if (c < 0)
            break;
        chain.append(off);
        off = qsizetype(t.u32(off + 2 + qsizetype(c) * 12));
    }
    return chain;
}

int findEntry(const Tiff &t, qsizetype ifd, quint16 tag)
{
    const int c = ifdCount(t, ifd);
    for (int i = 0; i < c; ++i)
        if (t.u16(ifd + 2 + qsizetype(i) * 12) == tag)
            return i;
    return -1;
}

int tiffOrientation(Tiff &t)
{
    const QList<qsizetype> chain = ifdChain(t);
    if (chain.isEmpty())
        return 0;
    const int i = findEntry(t, chain.first(), kTagOrientation);
    if (i < 0)
        return 0;
    const qsizetype e = chain.first() + 2 + qsizetype(i) * 12;
    if (t.u16(e + 2) != 3)  // SHORT
        return 0;
    const int v = t.u16(e + 8);
    return (v >= 1 && v <= 8) ? v : 0;
}

bool tiffHasGps(const Tiff &t)
{
    for (const qsizetype ifd : ifdChain(t))
        if (findEntry(t, ifd, kTagGpsIfd) >= 0)
            return true;
    return false;
}

void zero(Tiff &t, qsizetype off, qsizetype len)
{
    if (t.inb(off, len))
        std::memset(t.d + off, 0, size_t(len));
}

// Removes every GPS IFD pointer in place: the GPS IFD and its out-of-line
// values are zeroed, the pointer entry is squeezed out of its IFD and the IFD's
// count is reduced. The buffer keeps its length, so no other offset moves; the
// freed 12 bytes sit as zeros after the IFD's next-pointer.
void tiffRemoveGps(Tiff &t)
{
    for (const qsizetype ifd : ifdChain(t)) {
        int i;
        while ((i = findEntry(t, ifd, kTagGpsIfd)) >= 0) {
            const int count = ifdCount(t, ifd);
            const qsizetype entry = ifd + 2 + qsizetype(i) * 12;
            const qsizetype gps = qsizetype(t.u32(entry + 8));
            const int gpsCount = gps > 0 ? ifdCount(t, gps) : -1;
            if (gpsCount >= 0) {
                for (int k = 0; k < gpsCount; ++k) {
                    const qsizetype ge = gps + 2 + qsizetype(k) * 12;
                    const qsizetype bytes =
                        typeSize(t.u16(ge + 2)) * qsizetype(t.u32(ge + 4));
                    if (bytes > 4)
                        zero(t, qsizetype(t.u32(ge + 8)), bytes);
                }
                zero(t, gps, 2 + qsizetype(gpsCount) * 12 + 4);
            }
            // Squeeze the pointer entry out. The next-IFD pointer moves up with
            // the entries behind it.
            const qsizetype tail = qsizetype(count - 1 - i) * 12 + 4;
            std::memmove(t.d + entry, t.d + entry + 12, size_t(tail));
            zero(t, ifd + 2 + qsizetype(count - 1) * 12 + 4, 12);
            t.setU16(ifd, quint16(count - 1));
        }
    }
}

// A valid TIFF holding only the orientation (26 bytes), or an empty one
// (14 bytes) when there is nothing worth keeping.
QByteArray minimalTiff(int orientation)
{
    QByteArray b;
    b.append("MM", 2);
    putBe16(b, 0x002A);
    putBe32(b, 8);
    if (orientation >= 2 && orientation <= 8) {
        putBe16(b, 1);
        putBe16(b, kTagOrientation);
        putBe16(b, 3);
        putBe32(b, 1);
        putBe16(b, quint16(orientation));
        putBe16(b, 0);
    } else {
        putBe16(b, 0);
    }
    putBe32(b, 0);
    return b;
}

// Rewrites a TIFF block in place so it holds only the orientation. The block
// is zeroed first, so nothing of the old content survives.
void tiffScrubToOrientation(uchar *d, qsizetype n, int orientation)
{
    const QByteArray min = minimalTiff(orientation);
    std::memset(d, 0, size_t(n));
    if (n >= min.size())
        std::memcpy(d, min.constData(), size_t(min.size()));
    else if (n >= 14) {
        const QByteArray empty = minimalTiff(0);
        std::memcpy(d, empty.constData(), size_t(empty.size()));
    }
}

// Reads orientation and location out of a TIFF block without changing it.
void inspectTiff(const QByteArray &block, Ctx &ctx)
{
    QByteArray copy = block;
    Tiff t;
    if (!openTiff(reinterpret_cast<uchar *>(copy.data()), copy.size(), t))
        return;
    if (ctx.orientation == 0)
        ctx.orientation = tiffOrientation(t);
    if (tiffHasGps(t))
        ctx.foundLocation = true;
}

// Applies the mode to an Exif TIFF block that is being KEPT (Location mode).
// False = the block is not a TIFF we can edit.
bool editTiffForLocation(QByteArray &block, Ctx &ctx)
{
    Tiff t;
    if (!openTiff(reinterpret_cast<uchar *>(block.data()), block.size(), t))
        return false;
    if (ctx.orientation == 0)
        ctx.orientation = tiffOrientation(t);
    if (tiffHasGps(t)) {
        ctx.foundLocation = true;
        tiffRemoveGps(t);
    }
    return true;
}

bool xmpHasGps(const QByteArray &xmp)
{
    // Exif GPS properties live in the exif: namespace as exif:GPS*.
    return contains(xmp, "GPS");
}

// -- CRC32 for PNG -----------------------------------------------------------

quint32 crc32(const uchar *p, qsizetype n)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (qsizetype i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// -- JPEG --------------------------------------------------------------------

bool startsWith(const uchar *p, qsizetype n, const char *id, qsizetype idLen)
{
    return n >= idLen && std::memcmp(p, id, size_t(idLen)) == 0;
}

QByteArray jpegSegment(uchar marker, const QByteArray &payload)
{
    QByteArray s;
    s.append(char(0xFF));
    s.append(char(marker));
    putBe16(s, quint16(payload.size() + 2));
    s.append(payload);
    return s;
}

Result processJpeg(const QByteArray &in, Ctx &ctx)
{
    const auto *s = reinterpret_cast<const uchar *>(in.constData());
    const qsizetype n = in.size();
    const Mode mode = ctx.mode;
    Result fail;
    fail.status = Status::Failed;

    QByteArray out;
    out.reserve(n);
    out.append("\xFF\xD8", 2);
    qsizetype pos = 2;
    bool done = false;

    while (!done) {
        if (pos + 2 > n || s[pos] != 0xFF)
            return fail;
        const uchar marker = s[pos + 1];
        if (marker == 0xFF) {  // fill byte
            ++pos;
            continue;
        }
        if (marker == 0xD9) {  // EOI before any scan
            out.append("\xFF\xD9", 2);
            break;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            out.append(in.constData() + pos, 2);
            pos += 2;
            continue;
        }
        if (pos + 4 > n)
            return fail;
        const qsizetype len = be16(s + pos + 2);
        if (len < 2 || pos + 2 + len > n)
            return fail;
        const uchar *payload = s + pos + 4;
        const qsizetype plen = len - 2;
        const QByteArray whole = in.mid(pos, 2 + len);

        if (marker == 0xDA) {
            // Start of scan: the header, then entropy-coded data to the EOI.
            qsizetype i = pos + 2 + len;
            qsizetype end = -1;
            while (i < n) {
                if (s[i] != 0xFF) {
                    ++i;
                    continue;
                }
                if (i + 1 >= n)
                    return fail;
                const uchar m = s[i + 1];
                if (m == 0x00 || (m >= 0xD0 && m <= 0xD7)) {
                    i += 2;
                    continue;
                }
                if (m == 0xFF) {
                    ++i;
                    continue;
                }
                if (m == 0xD9) {
                    end = i + 2;
                    break;
                }
                if (i + 4 > n)
                    return fail;
                i += 2 + qsizetype(be16(s + i + 2));  // DHT / SOS / DNL ...
            }
            if (end < 0)
                return fail;  // no EOI: truncated, cannot vouch for it
            out.append(in.constData() + pos, end - pos);
            // Anything after the EOI is not the picture: Samsung / Apple /
            // Google append whole secondary images (depth, gain map) there, with
            // Exif and XMP of their own. Dropped in both stripping modes.
            done = true;
            break;
        }

        enum { Keep, Drop, Edit } action = Keep;
        QByteArray edited;
        if (marker == 0xE1) {
            if (startsWith(payload, plen, "Exif\0\0", 6)) {
                if (mode == Mode::RemoveAll) {
                    inspectTiff(QByteArray(
                                    reinterpret_cast<const char *>(payload) + 6,
                                    int(plen - 6)),
                                ctx);
                    action = Drop;
                } else {
                    QByteArray tiff(reinterpret_cast<const char *>(payload) + 6,
                                    int(plen - 6));
                    if (!editTiffForLocation(tiff, ctx))
                        return fail;
                    edited = QByteArray(reinterpret_cast<const char *>(payload),
                                        6)
                             + tiff;
                    action = Edit;
                }
            } else if (startsWith(payload, plen,
                                  "http://ns.adobe.com/xap/1.0/\0", 29)) {
                const QByteArray body(reinterpret_cast<const char *>(payload),
                                      int(plen));
                if (xmpHasGps(body))
                    ctx.foundLocation = true;
                action = (mode == Mode::RemoveAll || xmpHasGps(body)) ? Drop
                                                                       : Keep;
            } else if (startsWith(payload, plen,
                                  "http://ns.adobe.com/xmp/extension/\0", 35)) {
                // Extended XMP is split over segments; a GPS block cannot be
                // judged from one slice, so none of it is kept.
                action = Drop;
            } else if (mode == Mode::RemoveAll) {
                action = Drop;
            }
        } else if (marker == 0xE2) {
            if (startsWith(payload, plen, "ICC_PROFILE\0", 12))
                action = Keep;  // colour, not metadata: dropping it shifts colours
            else if (startsWith(payload, plen, "MPF\0", 4))
                action = Drop;  // offsets into the trailing images dropped above
            else if (mode == Mode::RemoveAll)
                action = Drop;
        } else if (marker == 0xED) {
            action = Drop;  // Photoshop / IPTC: captions and place names
        } else if (marker == 0xFE) {
            if (mode == Mode::RemoveAll)
                action = Drop;
        } else if (marker >= 0xE3 && marker <= 0xEF && marker != 0xEE
                   && marker != 0xED) {
            if (mode == Mode::RemoveAll)
                action = Drop;
        }

        switch (action) {
        case Keep: out.append(whole); break;
        case Edit: out.append(jpegSegment(marker, edited)); break;
        case Drop: break;
        }
        pos += 2 + len;
    }

    if (mode == Mode::RemoveAll && ctx.orientation >= 2) {
        const QByteArray exif = QByteArray("Exif\0\0", 6)
                                + minimalTiff(ctx.orientation);
        out.insert(2, jpegSegment(0xE1, exif));
    }

    Result r;
    r.bytes = out;
    r.status = (out == in) ? Status::Unchanged : Status::Stripped;
    return r;
}

// -- PNG ---------------------------------------------------------------------

QByteArray pngChunk(const char type[4], const QByteArray &data)
{
    QByteArray c;
    putBe32(c, quint32(data.size()));
    QByteArray body(type, 4);
    body.append(data);
    c.append(body);
    putBe32(c, crc32(reinterpret_cast<const uchar *>(body.constData()),
                     body.size()));
    return c;
}

Result processPng(const QByteArray &in, Ctx &ctx)
{
    const auto *s = reinterpret_cast<const uchar *>(in.constData());
    const qsizetype n = in.size();
    const Mode mode = ctx.mode;
    Result fail;
    fail.status = Status::Failed;

    QByteArray out = in.left(8);
    qsizetype pos = 8;
    qsizetype afterIhdr = -1;
    bool sawIend = false;

    while (pos + 12 <= n && !sawIend) {
        const qsizetype len = qsizetype(be32(s + pos));
        if (len < 0 || len > n - pos - 12)
            return fail;
        const QByteArray type = in.mid(pos + 4, 4);
        const QByteArray data = in.mid(pos + 8, len);
        const QByteArray whole = in.mid(pos, 12 + len);
        bool keep = true;
        QByteArray replacement;

        if (type == "eXIf") {
            if (mode == Mode::RemoveAll) {
                inspectTiff(data, ctx);
                keep = false;
            } else {
                QByteArray tiff = data;
                if (!editTiffForLocation(tiff, ctx))
                    return fail;
                replacement = pngChunk("eXIf", tiff);
            }
        } else if (type == "iTXt") {
            if (mode == Mode::RemoveAll) {
                keep = false;
            } else if (data.startsWith("XML:com.adobe.xmp")) {
                // keyword NUL, compression flag, method, then text. A
                // compressed block cannot be inspected, so it goes.
                const int nul = data.indexOf('\0');
                const bool compressed = nul >= 0 && nul + 1 < data.size()
                                        && data.at(nul + 1) != 0;
                if (compressed || xmpHasGps(data)) {
                    if (!compressed)
                        ctx.foundLocation = true;
                    keep = false;
                }
            }
        } else if (type == "tEXt" || type == "zTXt" || type == "tIME") {
            if (mode == Mode::RemoveAll)
                keep = false;
        } else if (type == "IEND") {
            sawIend = true;
        }

        if (keep)
            out.append(replacement.isEmpty() ? whole : replacement);
        if (type == "IHDR")
            afterIhdr = out.size();
        pos += 12 + len;
    }
    if (!sawIend)
        return fail;  // truncated: no IEND to end the picture at

    if (mode == Mode::RemoveAll && ctx.orientation >= 2 && afterIhdr > 0)
        out.insert(afterIhdr, pngChunk("eXIf", minimalTiff(ctx.orientation)));

    Result r;
    r.bytes = out;  // anything after IEND is not part of the picture
    r.status = (out == in) ? Status::Unchanged : Status::Stripped;
    return r;
}

// -- WebP --------------------------------------------------------------------

QByteArray webpChunk(const char fourcc[4], const QByteArray &data)
{
    QByteArray c(fourcc, 4);
    putLe32(c, quint32(data.size()));
    c.append(data);
    if (data.size() & 1)
        c.append('\0');
    return c;
}

Result processWebp(const QByteArray &in, Ctx &ctx)
{
    const auto *s = reinterpret_cast<const uchar *>(in.constData());
    const qsizetype n = in.size();
    const Mode mode = ctx.mode;
    Result fail;
    fail.status = Status::Failed;

    const qsizetype limit = qsizetype(le32(s + 4)) + 8;
    if (limit < 12 || limit > n)
        return fail;

    QByteArray body;  // everything after "WEBP"
    qsizetype pos = 12;
    qsizetype vp8xAt = -1;  // offset of the VP8X payload inside `body`
    bool hasExif = false;
    bool hasXmp = false;

    while (pos + 8 <= limit) {
        const qsizetype size = qsizetype(le32(s + pos + 4));
        if (size > limit - pos - 8)
            return fail;
        const qsizetype padded = size + (size & 1);
        const qsizetype chunkEnd =
            (pos + 8 + padded <= limit) ? pos + 8 + padded : pos + 8 + size;
        const QByteArray fourcc = in.mid(pos, 4);
        const QByteArray data = in.mid(pos + 8, size);
        bool keep = true;
        QByteArray replacement;

        if (fourcc == "EXIF") {
            if (mode == Mode::RemoveAll) {
                QByteArray tiff = data;
                if (tiff.startsWith("Exif\0\0"))
                    tiff.remove(0, 6);
                inspectTiff(tiff, ctx);
                keep = false;
            } else {
                QByteArray edit = data;
                const int off = edit.startsWith("Exif\0\0") ? 6 : 0;
                QByteArray tiff = edit.mid(off);
                if (!editTiffForLocation(tiff, ctx))
                    return fail;
                edit.replace(off, tiff.size(), tiff);
                replacement = webpChunk("EXIF", edit);
                hasExif = true;
            }
        } else if (fourcc == "XMP ") {
            if (xmpHasGps(data))
                ctx.foundLocation = true;
            if (mode == Mode::RemoveAll || xmpHasGps(data))
                keep = false;
            else
                hasXmp = true;
        } else if (fourcc == "VP8X") {
            vp8xAt = body.size() + 8;
        }

        if (keep)
            body.append(replacement.isEmpty() ? in.mid(pos, chunkEnd - pos)
                                              : replacement);
        pos = chunkEnd;
    }

    if (mode == Mode::RemoveAll && ctx.orientation >= 2 && vp8xAt >= 0) {
        body.append(webpChunk("EXIF", minimalTiff(ctx.orientation)));
        hasExif = true;
    }
    if (vp8xAt >= 0 && vp8xAt < body.size()) {
        uchar flags = uchar(body.at(vp8xAt));
        flags = uchar(hasExif ? (flags | 0x08) : (flags & ~0x08));
        flags = uchar(hasXmp ? (flags | 0x04) : (flags & ~0x04));
        body[vp8xAt] = char(flags);
    }

    QByteArray out("RIFF", 4);
    putLe32(out, quint32(body.size() + 4));
    out.append("WEBP", 4);
    out.append(body);

    Result r;
    r.bytes = out;
    r.status = (out == in) ? Status::Unchanged : Status::Stripped;
    return r;
}

// -- HEIC / AVIF (ISO BMFF) --------------------------------------------------

struct Box {
    qsizetype start = 0;   // box header
    qsizetype body = 0;    // first byte after the header
    qsizetype end = 0;
    char type[4] = {0, 0, 0, 0};
};

bool readBox(const uchar *s, qsizetype n, qsizetype pos, qsizetype limit,
             Box &b)
{
    if (pos + 8 > limit || limit > n)
        return false;
    quint64 size = be32(s + pos);
    qsizetype header = 8;
    std::memcpy(b.type, s + pos + 4, 4);
    if (size == 1) {
        if (pos + 16 > limit)
            return false;
        size = (quint64(be32(s + pos + 8)) << 32) | be32(s + pos + 12);
        header = 16;
    } else if (size == 0) {
        size = quint64(limit - pos);
    }
    if (size < quint64(header) || size > quint64(limit - pos))
        return false;
    b.start = pos;
    b.body = pos + header;
    b.end = pos + qsizetype(size);
    return true;
}

bool isType(const Box &b, const char *t) { return std::memcmp(b.type, t, 4) == 0; }

struct Extent {
    quint64 offset = 0;
    quint64 length = 0;
};
struct Item {
    quint32 id = 0;
    char type[4] = {0, 0, 0, 0};
    QByteArray contentType;
    QList<Extent> extents;
    int constructionMethod = 0;
    bool located = false;
};

quint64 readN(const uchar *p, int size)
{
    quint64 v = 0;
    for (int i = 0; i < size; ++i)
        v = (v << 8) | p[i];
    return v;
}

bool isBmffBrand(const uchar *s, qsizetype n)
{
    if (n < 16 || std::memcmp(s + 4, "ftyp", 4) != 0)
        return false;
    static const char *brands[] = {"heic", "heix", "hevc", "hevx", "heim",
                                   "heis", "hevm", "hevs", "mif1", "msf1",
                                   "avif", "avis"};
    const qsizetype size = qMin<qsizetype>(qsizetype(be32(s)), n);
    // Major brand, then the compatible list.
    for (qsizetype o = 8; o + 4 <= size; o += (o == 8 ? 8 : 4))
        for (const char *b : brands)
            if (std::memcmp(s + o, b, 4) == 0)
                return true;
    return false;
}

Result processBmff(const QByteArray &in, Ctx &ctx)
{
    QByteArray out = in;
    auto *s = reinterpret_cast<uchar *>(out.data());
    const qsizetype n = out.size();
    const Mode mode = ctx.mode;
    Result fail;
    fail.status = Status::Failed;

    // Top level: find 'meta'.
    Box meta;
    bool haveMeta = false;
    for (qsizetype pos = 0; pos < n;) {
        Box b;
        if (!readBox(s, n, pos, n, b))
            return fail;
        if (isType(b, "meta")) {
            meta = b;
            haveMeta = true;
            break;
        }
        pos = b.end;
    }
    if (!haveMeta) {
        Result r;
        r.bytes = in;
        r.status = Status::Unchanged;
        return r;
    }

    QList<Item> items;
    bool haveIinf = false, haveIloc = false;
    // 'meta' is a full box: 4 bytes of version and flags before its children.
    for (qsizetype pos = meta.body + 4; pos < meta.end;) {
        Box b;
        if (!readBox(s, n, pos, meta.end, b))
            return fail;
        if (isType(b, "iinf")) {
            haveIinf = true;
            if (b.body + 6 > b.end)
                return fail;
            const int version = s[b.body];
            qsizetype p = b.body + 4;
            p += (version == 0) ? 2 : 4;  // entry_count
            while (p < b.end) {
                Box infe;
                if (!readBox(s, n, p, b.end, infe))
                    return fail;
                if (isType(infe, "infe") && infe.body + 4 <= infe.end) {
                    const int v = s[infe.body];
                    qsizetype q = infe.body + 4;
                    Item it;
                    if (v >= 2) {
                        const int idSize = (v == 2) ? 2 : 4;
                        if (q + idSize + 2 + 4 > infe.end)
                            return fail;
                        it.id = quint32(readN(s + q, idSize));
                        q += idSize + 2;  // item_ID, protection index
                        std::memcpy(it.type, s + q, 4);
                        q += 4;
                        // item_name\0 then, for 'mime', content_type\0
                        while (q < infe.end && s[q] != 0)
                            ++q;
                        ++q;
                        if (std::memcmp(it.type, "mime", 4) == 0 && q < infe.end) {
                            qsizetype e = q;
                            while (e < infe.end && s[e] != 0)
                                ++e;
                            it.contentType = QByteArray(
                                reinterpret_cast<const char *>(s + q), int(e - q));
                        }
                        items.append(it);
                    }
                }
                p = infe.end;
            }
        } else if (isType(b, "iloc")) {
            haveIloc = true;
            if (b.body + 8 > b.end)
                return fail;
            const int version = s[b.body];
            qsizetype p = b.body + 4;
            const int offsetSize = s[p] >> 4;
            const int lengthSize = s[p] & 0xF;
            const int baseSize = s[p + 1] >> 4;
            const int indexSize = (version == 1 || version == 2) ? (s[p + 1] & 0xF) : 0;
            p += 2;
            quint32 itemCount;
            if (version < 2) {
                itemCount = be16(s + p);
                p += 2;
            } else {
                if (p + 4 > b.end)
                    return fail;
                itemCount = be32(s + p);
                p += 4;
            }
            for (quint32 k = 0; k < itemCount; ++k) {
                const int idSize = (version < 2) ? 2 : 4;
                if (p + idSize > b.end)
                    return fail;
                const quint32 id = quint32(readN(s + p, idSize));
                p += idSize;
                int cm = 0;
                if (version == 1 || version == 2) {
                    if (p + 2 > b.end)
                        return fail;
                    cm = be16(s + p) & 0xF;
                    p += 2;
                }
                if (p + 2 + baseSize + 2 > b.end)
                    return fail;
                p += 2;  // data_reference_index
                const quint64 base = readN(s + p, baseSize);
                p += baseSize;
                const int extentCount = be16(s + p);
                p += 2;
                QList<Extent> extents;
                for (int e = 0; e < extentCount; ++e) {
                    if (p + indexSize + offsetSize + lengthSize > b.end)
                        return fail;
                    p += indexSize;
                    Extent ex;
                    ex.offset = base + readN(s + p, offsetSize);
                    p += offsetSize;
                    ex.length = readN(s + p, lengthSize);
                    p += lengthSize;
                    extents.append(ex);
                }
                for (Item &it : items)
                    if (it.id == id) {
                        it.extents = extents;
                        it.constructionMethod = cm;
                        it.located = true;
                    }
            }
        }
        pos = b.end;
    }

    bool touched = false;
    for (const Item &it : items) {
        const bool isExif = std::memcmp(it.type, "Exif", 4) == 0;
        const bool isXmp = std::memcmp(it.type, "mime", 4) == 0
                           && (it.contentType.contains("xmp")
                               || it.contentType.contains("rdf"));
        if (!isExif && !isXmp)
            continue;
        // An item whose bytes we cannot point at cannot be vouched for.
        if (!haveIinf || !haveIloc || !it.located || it.constructionMethod != 0
            || it.extents.size() != 1)
            return fail;
        const Extent &ex = it.extents.first();
        if (ex.offset > quint64(n) || ex.length > quint64(n) - ex.offset)
            return fail;
        const qsizetype off = qsizetype(ex.offset);
        const qsizetype len = qsizetype(ex.length);
        if (isExif) {
            // 4 bytes: offset from here to the TIFF header (usually 0, or 6
            // when the "Exif\0\0" prefix is kept).
            if (len < 4 + 8)
                return fail;
            const qsizetype tiffOff = 4 + qsizetype(be32(s + off));
            if (tiffOff > len)
                return fail;
            Tiff t;
            if (!openTiff(s + off + tiffOff, len - tiffOff, t))
                return fail;
            const int orient = tiffOrientation(t);
            if (ctx.orientation == 0)
                ctx.orientation = orient;
            if (tiffHasGps(t))
                ctx.foundLocation = true;
            if (mode == Mode::RemoveAll) {
                tiffScrubToOrientation(s + off + tiffOff, len - tiffOff, orient);
                touched = true;
            } else if (tiffHasGps(t)) {
                tiffRemoveGps(t);
                touched = true;
            }
        } else {
            const QByteArray xmp(reinterpret_cast<const char *>(s + off), int(len));
            if (xmpHasGps(xmp))
                ctx.foundLocation = true;
            if (mode == Mode::RemoveAll || xmpHasGps(xmp)) {
                std::memset(s + off, ' ', size_t(len));
                touched = true;
            }
        }
    }

    Result r;
    r.bytes = touched ? out : in;
    r.status = (touched && out != in) ? Status::Stripped : Status::Unchanged;
    return r;
}

Result run(const QByteArray &in, Ctx &ctx)
{
    const auto *s = reinterpret_cast<const uchar *>(in.constData());
    const qsizetype n = in.size();
    Result unsupported;
    unsupported.status = Status::Unsupported;
    unsupported.bytes = in;

    Result r;
    if (n >= 4 && s[0] == 0xFF && s[1] == 0xD8 && s[2] == 0xFF)
        r = processJpeg(in, ctx);
    else if (n >= 8 && std::memcmp(s, "\x89PNG\r\n\x1A\n", 8) == 0)
        r = processPng(in, ctx);
    else if (n >= 12 && std::memcmp(s, "RIFF", 4) == 0
             && std::memcmp(s + 8, "WEBP", 4) == 0)
        r = processWebp(in, ctx);
    else if (isBmffBrand(s, n))
        r = processBmff(in, ctx);
    else
        return unsupported;

    if (r.status == Status::Failed || r.status == Status::Unchanged)
        r.bytes = in;
    return r;
}

}  // namespace

Result strip(const QByteArray &input, Mode mode)
{
    if (mode == Mode::Keep) {
        Result r;
        r.status = Status::Unchanged;
        r.bytes = input;
        return r;
    }
    Ctx ctx;
    ctx.mode = mode;
    return run(input, ctx);
}

bool containsLocation(const QByteArray &input)
{
    Ctx ctx;
    ctx.mode = Mode::RemoveLocation;
    run(input, ctx);
    return ctx.foundLocation;
}

int exifOrientation(const QByteArray &input)
{
    Ctx ctx;
    ctx.mode = Mode::RemoveLocation;
    run(input, ctx);
    return ctx.orientation;
}

}  // namespace imagemeta
