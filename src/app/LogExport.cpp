#include "app/LogExport.h"

#include "app/AsyncLogSink.h"
#include "app/SessionDiagnostics.h"
#include "crypto/E2eeDiagnostics.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <atomic>

namespace lightning::logexport {

// =========================================================================
// sanitizer
// =========================================================================

namespace {

constexpr int kMaxLineChars = 4000;

// Categories whose lines are dropped outright, matched as a prefix on a dot
// boundary. Recovery and secret-storage handling, password changes and the
// crypto bootstrap carry the material section 6 forbids logging; they are
// expected to log none, and this is the second line of defence.
const char *const kSensitiveCategories[] = {
    "lightning.e2ee",
    "lightning.crypto",
    "lightning.account.password",
    "matrix.secret",
};

// Lines that name a crypto engine's internals: debug output there can carry
// raw session state.
bool mentionsCryptoInternals(const QString &line)
{
    return line.contains(QLatin1String("matrix_sdk_crypto"))
        || line.contains(QLatin1String("vodozemac"));
}

// "<iso time> <level> <category>: <message>" as AsyncLogSink writes it.
const QRegularExpression &categoryOfLine()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\S+\s+(?:debug|info|warning|critical|fatal)\s+([^\s:]+):)"));
    return re;
}

// A message-body field: never exported, however it came to be logged.
const QRegularExpression &bodyField()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\b(?:formatted_body|plaintext|message_body|text_body|body)["']?\s*[=:]\s*["']?\S)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// !room:server, @user:server, #alias:server and legacy $event:server.
const QRegularExpression &matrixId()
{
    static const QRegularExpression re(QStringLiteral(
        R"((?<![A-Za-z0-9_])[@!#$][^\s:@!#$"'<>(){}\[\],;|\\]+:[A-Za-z0-9][A-Za-z0-9.-]*(?::\d+)?)"));
    return re;
}

// Event ids since room version 3 have no server part.
const QRegularExpression &bareEventId()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?<![A-Za-z0-9_$])\$[A-Za-z0-9_+/=-]{16,})"));
    return re;
}

const QRegularExpression &urlWithPath()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\b([a-z][a-z0-9+.-]*://)(?:[^\s/@"'<>]*@)?([^\s/?#"'<>]+)[/?#][^\s"'<>]*)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression &urlUserinfo()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\b([a-z][a-z0-9+.-]*://)[^\s/@"'<>]*@)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// "Authorization: Bearer <token>" and "Basic <b64>" carry no `=` or `:` after
// the scheme word, which the shared credential rule requires; JWT-shaped
// strings are caught wherever they appear.
const QRegularExpression &authorizationScheme()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\b(?:(?i:Bearer)\s+[^\s"',;]+|Basic\s+[A-Za-z0-9+/=]{16,}))"));
    return re;
}

const QRegularExpression &jwtShape()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\beyJ[A-Za-z0-9_-]{4,}(?:\.[A-Za-z0-9_-]*){1,2})"));
    return re;
}

const QRegularExpression &emailAddress()
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z0-9.-]+\b)"));
    return re;
}

QString maskWith(const QRegularExpression &re, const QString &input,
                 const QByteArray &salt, int *count)
{
    QString out;
    out.reserve(input.size());
    qsizetype cursor = 0;
    auto it = re.globalMatch(input);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(input).mid(cursor, m.capturedStart() - cursor);
        out += maskMatrixId(m.captured(), salt);
        if (count)
            ++*count;
        cursor = m.capturedEnd();
    }
    out += QStringView(input).mid(cursor);
    return out;
}

} // namespace

bool isSensitiveCategory(QStringView category)
{
    for (const char *prefix : kSensitiveCategories) {
        const QLatin1String p{prefix};
        if (category == p
            || (category.startsWith(p) && category.size() > p.size()
                && category.at(p.size()) == QLatin1Char('.'))) {
            return true;
        }
    }
    return false;
}

QString maskMatrixId(const QString &id, const QByteArray &salt)
{
    if (salt.isEmpty())
        return matrix::e2ee::redactId(id);
    // redactId() keeps the sigil and hashes the rest; the salt rides in front
    // of the rest so the result keeps that shape.
    QString salted;
    qsizetype restFrom = 0;
    if (!id.isEmpty()
        && (id.at(0) == QLatin1Char('!') || id.at(0) == QLatin1Char('$')
            || id.at(0) == QLatin1Char('@') || id.at(0) == QLatin1Char('#'))) {
        salted = id.left(1);
        restFrom = 1;
    }
    salted += QString::fromLatin1(salt.toHex());
    salted += QStringView(id).mid(restFrom);
    return matrix::e2ee::redactId(salted);
}

std::optional<QString> sanitizeLine(const QString &line,
                                    const SanitizeOptions &options,
                                    SanitizeStats *stats)
{
    if (stats)
        ++stats->linesIn;

    QString s = line;
    // A control character can hide text from a reader or forge a line.
    for (QChar &ch : s) {
        if (ch.unicode() < 0x20 && ch != QLatin1Char('\t'))
            ch = QLatin1Char(' ');
    }

    const QRegularExpressionMatch cat = categoryOfLine().match(s);
    if (cat.hasMatch() && isSensitiveCategory(cat.capturedView(1))) {
        if (stats)
            ++stats->droppedSensitiveCategory;
        return std::nullopt;
    }
    if (mentionsCryptoInternals(s)) {
        if (stats)
            ++stats->droppedSensitiveCategory;
        return std::nullopt;
    }
    if (bodyField().match(s).hasMatch()) {
        if (stats)
            ++stats->droppedMessageBody;
        return std::nullopt;
    }

    int ids = 0;
    s = maskWith(matrixId(), s, options.salt, &ids);
    s = maskWith(bareEventId(), s, options.salt, &ids);
    if (stats)
        stats->idsReplaced += ids;

    // Host stays (it says which server is unreachable); path, query,
    // fragment and credentials go.
    s.replace(urlWithPath(), QStringLiteral("\\1\\2/<path>"));
    s.replace(urlUserinfo(), QStringLiteral("\\1"));
    s.replace(emailAddress(), QStringLiteral("<email>"));
    s.replace(authorizationScheme(), QStringLiteral("<credential>"));
    s.replace(jwtShape(), QStringLiteral("<credential>"));

    // Longest first so a name inside a longer one cannot leave a tail.
    QStringList literals;
    for (const QString &lit : options.literals) {
        if (lit.size() >= 3)
            literals.append(lit);
    }
    std::sort(literals.begin(), literals.end(),
              [](const QString &a, const QString &b) {
                  return a.size() > b.size();
              });
    for (const QString &lit : std::as_const(literals))
        s.replace(lit, QStringLiteral("<name>"), Qt::CaseInsensitive);

    // Filesystem paths (whole, so the localpart in a store path goes with
    // them), credential assignments, token shapes, recovery-key groups and
    // long opaque runs.
    s = matrix::app_diagnostics::redactSensitive(s);

    if (s.size() > kMaxLineChars) {
        s.truncate(kMaxLineChars);
        s += QStringLiteral(" ...[truncated]");
        if (stats)
            ++stats->truncatedLines;
    }
    if (stats)
        ++stats->linesKept;
    return s;
}

QString sanitizeLog(const QString &raw, const SanitizeOptions &options,
                    SanitizeStats *stats)
{
    QString out;
    out.reserve(raw.size());
    qsizetype pos = 0;
    while (pos < raw.size()) {
        qsizetype nl = raw.indexOf(QLatin1Char('\n'), pos);
        if (nl < 0)
            nl = raw.size();
        const QString line = raw.mid(pos, nl - pos);
        pos = nl + 1;
        if (line.isEmpty())
            continue;
        if (const auto kept = sanitizeLine(line, options, stats)) {
            out += *kept;
            out += QLatin1Char('\n');
        }
    }
    return out;
}

QString sanitizeText(const QString &text, const SanitizeOptions &options)
{
    SanitizeOptions noDrops = options;
    QString out;
    qsizetype pos = 0;
    while (pos < text.size()) {
        qsizetype nl = text.indexOf(QLatin1Char('\n'), pos);
        if (nl < 0)
            nl = text.size();
        const QString line = text.mid(pos, nl - pos);
        pos = nl + 1;
        // Header lines are never dropped for their shape, only scrubbed.
        if (line.isEmpty()) {
            out += QLatin1Char('\n');
            continue;
        }
        QString s = line;
        int ids = 0;
        s = maskWith(matrixId(), s, noDrops.salt, &ids);
        s = maskWith(bareEventId(), s, noDrops.salt, &ids);
        s.replace(urlWithPath(), QStringLiteral("\\1\\2/<path>"));
        s.replace(urlUserinfo(), QStringLiteral("\\1"));
        s.replace(emailAddress(), QStringLiteral("<email>"));
        s.replace(authorizationScheme(), QStringLiteral("<credential>"));
        s.replace(jwtShape(), QStringLiteral("<credential>"));
        for (const QString &lit : std::as_const(noDrops.literals)) {
            if (lit.size() >= 3)
                s.replace(lit, QStringLiteral("<name>"), Qt::CaseInsensitive);
        }
        out += matrix::app_diagnostics::redactSensitive(s);
        out += QLatin1Char('\n');
    }
    return out;
}

QString lastLines(const QString &text, int count)
{
    if (count <= 0 || text.isEmpty())
        return {};
    qsizetype end = text.size();
    if (text.endsWith(QLatin1Char('\n')))
        --end;
    qsizetype start = end;
    int seen = 0;
    while (start > 0) {
        const qsizetype nl = text.lastIndexOf(QLatin1Char('\n'), start - 1);
        if (nl < 0) {
            start = 0;
            break;
        }
        if (++seen == count) {
            start = nl + 1;
            break;
        }
        start = nl;
    }
    return text.mid(start, end - start);
}

// =========================================================================
// rolling log
// =========================================================================

struct RollingLogDevice::Impl {
    QString dir;
    RollingLimits limits;
    QFile file;
    qint64 size = 0;
    int seq = 0;
    QString sessionStamp;
};

RollingLogDevice::RollingLogDevice(const QString &directory,
                                   RollingLimits limits, QObject *parent)
    : QIODevice(parent), m_impl(std::make_unique<Impl>())
{
    m_impl->dir = directory;
    m_impl->limits = limits;
}

RollingLogDevice::~RollingLogDevice()
{
    close();
}

QStringList RollingLogDevice::logFiles(const QString &directory)
{
    const QDir dir(directory);
    QStringList out;
    const QStringList names = dir.entryList(
        {QStringLiteral("lightning-*.log")}, QDir::Files, QDir::Name);
    for (const QString &name : names)
        out.append(dir.absoluteFilePath(name));
    return out;
}

void RollingLogDevice::prune(const QString &directory,
                             const RollingLimits &limits, const QString &keep)
{
    QStringList files = logFiles(directory);
    qint64 total = 0;
    for (const QString &f : std::as_const(files))
        total += QFileInfo(f).size();
    while (!files.isEmpty()
           && (files.size() > limits.maxFiles || total > limits.maxTotalBytes)) {
        const QString oldest = files.first();
        if (!keep.isEmpty() && QFileInfo(oldest) == QFileInfo(keep))
            break; // only the live file is left to remove
        const qint64 sz = QFileInfo(oldest).size();
        files.removeFirst();
        if (QFile::remove(oldest))
            total -= sz;
    }
}

bool RollingLogDevice::open(OpenMode mode)
{
    Q_UNUSED(mode)
    if (isOpen())
        return true;
    QDir dir(m_impl->dir);
    if (!dir.mkpath(QStringLiteral(".")))
        return false;
    QFile::setPermissions(m_impl->dir, QFile::ReadOwner | QFile::WriteOwner
                                           | QFile::ExeOwner);
    if (m_impl->sessionStamp.isEmpty()) {
        m_impl->sessionStamp = QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMdd-HHmmss"));
    }
    prune(m_impl->dir, m_impl->limits, QString());
    if (!openNextFile())
        return false;
    return QIODevice::open(QIODevice::WriteOnly);
}

void RollingLogDevice::close()
{
    if (m_impl->file.isOpen()) {
        m_impl->file.flush();
        m_impl->file.close();
    }
    if (isOpen())
        QIODevice::close();
}

bool RollingLogDevice::openNextFile()
{
    if (m_impl->file.isOpen()) {
        m_impl->file.flush();
        m_impl->file.close();
    }
    QString path;
    do {
        path = QStringLiteral("%1/lightning-%2-%3.log")
                   .arg(m_impl->dir, m_impl->sessionStamp,
                        QString::number(m_impl->seq++).rightJustified(3, QLatin1Char('0')));
    } while (QFileInfo::exists(path));
    m_impl->file.setFileName(path);
    if (!m_impl->file.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    m_impl->file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    m_impl->size = 0;
    prune(m_impl->dir, m_impl->limits, path);
    return true;
}

qint64 RollingLogDevice::writeData(const char *data, qint64 len)
{
    if (len <= 0)
        return 0;
    if (!m_impl->file.isOpen())
        return -1;
    if (m_impl->size >= m_impl->limits.maxFileBytes && !openNextFile())
        return -1;
    const qint64 written = m_impl->file.write(data, len);
    // One write(2) per batch: QFile buffers, and the sink does not flush a
    // device that is not a QFileDevice.
    m_impl->file.flush();
    if (written > 0)
        m_impl->size += written;
    return written;
}

QByteArray readRecentLog(const QString &directory, qint64 maxBytes,
                         int *fileCount)
{
    const QStringList files = RollingLogDevice::logFiles(directory);
    QList<QByteArray> parts;
    qint64 have = 0;
    int used = 0;
    for (auto it = files.crbegin(); it != files.crend() && have < maxBytes; ++it) {
        QFile f(*it);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const qint64 room = maxBytes - have;
        QByteArray chunk;
        if (f.size() > room) {
            f.seek(f.size() - room);
            chunk = f.readAll();
            // Start at a line, not in the middle of one.
            const qsizetype nl = chunk.indexOf('\n');
            chunk = nl >= 0 ? chunk.mid(nl + 1) : QByteArray();
        } else {
            chunk = f.readAll();
        }
        if (chunk.isEmpty())
            continue;
        have += chunk.size();
        ++used;
        parts.prepend(chunk);
    }
    if (fileCount)
        *fileCount = used;
    QByteArray out;
    out.reserve(qsizetype(have));
    for (const QByteArray &p : std::as_const(parts))
        out += p;
    return out;
}

namespace {
std::atomic<lightning::logging::AsyncLogSink *> g_rollingSink{nullptr};
QString g_rollingDir;
} // namespace

lightning::logging::AsyncLogSink *rollingLogSink()
{
    return g_rollingSink.load();
}

void setRollingLogSink(lightning::logging::AsyncLogSink *sink)
{
    g_rollingSink.store(sink);
}

QString rollingLogDirectory()
{
    return g_rollingDir;
}

void setRollingLogDirectory(const QString &directory)
{
    g_rollingDir = directory;
}

namespace {
QString g_buildInfo;
} // namespace
void setBuildInfo(const QString &text) { g_buildInfo = text; }
QString buildInfo() { return g_buildInfo; }

// =========================================================================
// zip
// =========================================================================

namespace {

quint32 crc32Of(const QByteArray &data)
{
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data)
        c = table[(c ^ quint8(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(QByteArray &out, quint32 v)
{
    out += char(v & 0xFF);
    out += char((v >> 8) & 0xFF);
}

void put32(QByteArray &out, quint32 v)
{
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

quint32 get16(const QByteArray &b, qsizetype at)
{
    return quint8(b[at]) | (quint32(quint8(b[at + 1])) << 8);
}

quint32 get32(const QByteArray &b, qsizetype at)
{
    return get16(b, at) | (get16(b, at + 2) << 16);
}

// Raw deflate, taken out of Qt's zlib framing: 4 bytes of length, a 2-byte
// zlib header, the deflate stream, a 4-byte adler32.
QByteArray rawDeflate(const QByteArray &data)
{
    const QByteArray framed = qCompress(data, 6);
    if (framed.size() <= 10)
        return {};
    return framed.mid(6, framed.size() - 10);
}

// ---- inflate (RFC 1951), after zlib's puff.c ------------------------------

struct Inflater {
    const quint8 *in;
    qsizetype inLen;
    qsizetype inPos = 0;
    quint32 bitBuf = 0;
    int bitCnt = 0;
    QByteArray out;
    bool err = false;

    int bits(int need)
    {
        quint32 val = bitBuf;
        while (bitCnt < need) {
            if (inPos >= inLen) {
                err = true;
                return 0;
            }
            val |= quint32(in[inPos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = val >> need;
        bitCnt -= need;
        return int(val & ((1u << need) - 1));
    }
};

struct Huffman {
    short count[16];
    short symbol[288];
};

int decodeSymbol(Inflater &s, const Huffman &h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= s.bits(1);
        if (s.err)
            return -1;
        const int count = h.count[len];
        if (code - count < first)
            return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

int construct(Huffman &h, const short *length, int n)
{
    for (int len = 0; len <= 15; ++len)
        h.count[len] = 0;
    for (int sym = 0; sym < n; ++sym)
        ++h.count[length[sym]];
    if (h.count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0)
            return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len)
        offs[len + 1] = short(offs[len] + h.count[len]);
    for (int sym = 0; sym < n; ++sym) {
        if (length[sym] != 0)
            h.symbol[offs[length[sym]]++] = short(sym);
    }
    return left;
}

bool inflateCodes(Inflater &s, const Huffman &lencode, const Huffman &distcode)
{
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195,
                                   227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97,
                                    129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
                                    3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = decodeSymbol(s, lencode);
        if (symbol < 0)
            return false;
        if (symbol < 256) {
            s.out += char(symbol);
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29)
                return false;
            const int len = lens[symbol] + s.bits(lext[symbol]);
            const int dsym = decodeSymbol(s, distcode);
            if (dsym < 0 || dsym >= 30)
                return false;
            const int dist = dists[dsym] + s.bits(dext[dsym]);
            if (s.err || dist > s.out.size())
                return false;
            for (int i = 0; i < len; ++i)
                s.out += s.out[s.out.size() - dist];
        }
    }
}

bool inflateRaw(const QByteArray &data, QByteArray *result)
{
    Inflater s{reinterpret_cast<const quint8 *>(data.constData()), data.size()};
    int last;
    do {
        last = s.bits(1);
        const int type = s.bits(2);
        if (s.err)
            return false;
        if (type == 0) {
            s.bitBuf = 0;
            s.bitCnt = 0;
            if (s.inPos + 4 > s.inLen)
                return false;
            const int len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
            const int nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
            s.inPos += 4;
            if (len != (~nlen & 0xFFFF) || s.inPos + len > s.inLen)
                return false;
            s.out.append(reinterpret_cast<const char *>(s.in + s.inPos), len);
            s.inPos += len;
        } else if (type == 1) {
            static Huffman lencode, distcode;
            static bool built = false;
            if (!built) {
                short lengths[288];
                int sym = 0;
                for (; sym < 144; ++sym) lengths[sym] = 8;
                for (; sym < 256; ++sym) lengths[sym] = 9;
                for (; sym < 280; ++sym) lengths[sym] = 7;
                for (; sym < 288; ++sym) lengths[sym] = 8;
                construct(lencode, lengths, 288);
                for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
                construct(distcode, lengths, 30);
                built = true;
            }
            if (!inflateCodes(s, lencode, distcode))
                return false;
        } else if (type == 2) {
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                            11, 4, 12, 3, 13, 2, 14, 1, 15};
            const int nlen = s.bits(5) + 257;
            const int ndist = s.bits(5) + 1;
            const int ncode = s.bits(4) + 4;
            if (s.err || nlen > 286 || ndist > 30)
                return false;
            short lengths[320] = {};
            for (int i = 0; i < ncode; ++i)
                lengths[order[i]] = short(s.bits(3));
            Huffman lencode, distcode;
            if (construct(lencode, lengths, 19) != 0)
                return false;
            int index = 0;
            while (index < nlen + ndist) {
                int symbol = decodeSymbol(s, lencode);
                if (symbol < 0)
                    return false;
                if (symbol < 16) {
                    lengths[index++] = short(symbol);
                } else {
                    int len = 0, rep;
                    if (symbol == 16) {
                        if (index == 0)
                            return false;
                        len = lengths[index - 1];
                        rep = 3 + s.bits(2);
                    } else if (symbol == 17) {
                        rep = 3 + s.bits(3);
                    } else {
                        rep = 11 + s.bits(7);
                    }
                    if (index + rep > nlen + ndist)
                        return false;
                    while (rep--)
                        lengths[index++] = short(len);
                }
            }
            if (lengths[256] == 0)
                return false;
            if (construct(lencode, lengths, nlen) < 0)
                return false;
            if (construct(distcode, lengths + nlen, ndist) < 0)
                return false;
            if (!inflateCodes(s, lencode, distcode))
                return false;
        } else {
            return false;
        }
    } while (!last);
    *result = s.out;
    return !s.err;
}

void dosTime(const QDateTime &when, quint32 *time, quint32 *date)
{
    const QDateTime t = when.isValid() ? when.toUTC()
                                       : QDateTime::currentDateTimeUtc();
    const int year = std::clamp(t.date().year(), 1980, 2107);
    *time = quint32((t.time().hour() << 11) | (t.time().minute() << 5)
                    | (t.time().second() / 2));
    *date = quint32(((year - 1980) << 9) | (t.date().month() << 5)
                    | t.date().day());
}

} // namespace

QByteArray writeZip(const QList<ZipEntry> &entries, const QDateTime &when)
{
    quint32 dtime = 0, ddate = 0;
    dosTime(when, &dtime, &ddate);

    QByteArray out;
    QByteArray central;
    for (const ZipEntry &e : entries) {
        const QByteArray name = e.name.toUtf8();
        const quint32 crc = crc32Of(e.data);
        QByteArray packed = e.data.isEmpty() ? QByteArray() : rawDeflate(e.data);
        quint32 method = 8;
        if (packed.isEmpty() || packed.size() >= e.data.size()) {
            packed = e.data;
            method = 0;
        }
        const quint32 offset = quint32(out.size());

        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0x0800); // UTF-8 names
        put16(out, method);
        put16(out, dtime);
        put16(out, ddate);
        put32(out, crc);
        put32(out, quint32(packed.size()));
        put32(out, quint32(e.data.size()));
        put16(out, quint32(name.size()));
        put16(out, 0);
        out += name;
        out += packed;

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, method);
        put16(central, dtime);
        put16(central, ddate);
        put32(central, crc);
        put32(central, quint32(packed.size()));
        put32(central, quint32(e.data.size()));
        put16(central, quint32(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central += name;
    }
    const quint32 cdOffset = quint32(out.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, quint32(entries.size()));
    put16(out, quint32(entries.size()));
    put32(out, quint32(central.size()));
    put32(out, cdOffset);
    put16(out, 0);
    return out;
}

QList<ZipEntry> readZip(const QByteArray &zip, bool *ok)
{
    const auto fail = [ok]() -> QList<ZipEntry> {
        if (ok)
            *ok = false;
        return {};
    };
    if (zip.size() < 22)
        return fail();
    qsizetype eocd = -1;
    for (qsizetype i = zip.size() - 22; i >= 0 && i >= zip.size() - 22 - 65535; --i) {
        if (get32(zip, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return fail();
    const int count = int(get16(zip, eocd + 10));
    qsizetype at = qsizetype(get32(zip, eocd + 16));
    QList<ZipEntry> result;
    for (int n = 0; n < count; ++n) {
        if (at + 46 > zip.size() || get32(zip, at) != 0x02014b50)
            return fail();
        const quint32 method = get16(zip, at + 10);
        const quint32 crc = get32(zip, at + 16);
        const qsizetype csize = qsizetype(get32(zip, at + 20));
        const qsizetype usize = qsizetype(get32(zip, at + 24));
        const qsizetype nameLen = qsizetype(get16(zip, at + 28));
        const qsizetype extraLen = qsizetype(get16(zip, at + 30));
        const qsizetype commentLen = qsizetype(get16(zip, at + 32));
        const qsizetype local = qsizetype(get32(zip, at + 42));
        if (at + 46 + nameLen > zip.size() || local + 30 > zip.size()
            || get32(zip, local) != 0x04034b50) {
            return fail();
        }
        const QString name = QString::fromUtf8(zip.mid(at + 46, nameLen));
        const qsizetype dataAt = local + 30 + qsizetype(get16(zip, local + 26))
            + qsizetype(get16(zip, local + 28));
        if (dataAt + csize > zip.size())
            return fail();
        const QByteArray packed = zip.mid(dataAt, csize);
        QByteArray data;
        if (method == 0) {
            data = packed;
        } else if (method == 8) {
            if (!inflateRaw(packed, &data))
                return fail();
        } else {
            return fail();
        }
        if (data.size() != usize || crc32Of(data) != crc)
            return fail();
        result.append({name, data});
        at += 46 + nameLen + extraLen + commentLen;
    }
    if (ok)
        *ok = true;
    return result;
}

// =========================================================================
// the export
// =========================================================================

QString exportPromise()
{
    return QStringLiteral(
        "This file has no messages, passwords or keys. User ids, room ids and "
        "event ids are replaced with short codes, file paths and account names "
        "are removed, and lines from encryption and credential handling are "
        "left out.");
}

ExportResult buildExport(const ExportInputs &inputs)
{
    ExportResult r;
    r.rawBytes = inputs.rawLog.size();
    r.sessionFiles = inputs.sessionFiles;
    r.sanitizedDiagnostics = sanitizeText(inputs.diagnostics, inputs.options);
    const QString body = sanitizeLog(QString::fromUtf8(inputs.rawLog),
                                     inputs.options, &r.stats);

    const int removed = r.stats.droppedSensitiveCategory
        + r.stats.droppedMessageBody;
    QString log;
    log += QStringLiteral("# Lightning log, sanitized for sharing.\n");
    log += QStringLiteral("# %1 of %2 lines kept; %3 removed (encryption, "
                          "credential or message-body lines); %4 ids replaced "
                          "by codes.\n")
               .arg(r.stats.linesKept).arg(r.stats.linesIn).arg(removed)
               .arg(r.stats.idsReplaced);
    log += body;
    r.sanitizedLog = log;

    QString readme;
    readme += QStringLiteral("Lightning log export\n\n");
    readme += exportPromise();
    readme += QStringLiteral(
        "\n\n%1  basic facts about the build and the machine\n"
        "%2  the application's recent log (sanitized)\n\n"
        "Ids are replaced by codes that match inside this file only.\n")
        .arg(QLatin1String(kDiagnosticsName), QLatin1String(kLogName));

    r.zip = writeZip({{QLatin1String(kReadmeName), readme.toUtf8()},
                      {QLatin1String(kDiagnosticsName),
                       r.sanitizedDiagnostics.toUtf8()},
                      {QLatin1String(kLogName), r.sanitizedLog.toUtf8()}},
                     inputs.when);
    return r;
}

} // namespace lightning::logexport
