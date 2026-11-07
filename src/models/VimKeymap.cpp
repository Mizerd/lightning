#include "models/VimKeymap.h"

#include <QClipboard>
#include <QGuiApplication>

namespace {

constexpr int kMaxCount = 9999;

bool isBlank(QChar c)
{
    return c == u' ' || c == u'\t';
}

// 0 whitespace, 1 keyword, 2 other non-blank: Vim's "word" classes.
int charClass(QChar c)
{
    if (c.isSpace())
        return 0;
    if (c.isLetterOrNumber() || c == u'_')
        return 1;
    return 2;
}

int lineStart(const QString &buf, int pos)
{
    if (pos <= 0)
        return 0;
    const int i = buf.lastIndexOf(u'\n', qMin(pos, int(buf.size())) - 1);
    return i < 0 ? 0 : i + 1;
}

int lineEnd(const QString &buf, int pos)
{
    const int i = buf.indexOf(u'\n', qMax(0, pos));
    return i < 0 ? int(buf.size()) : i;
}

int lineNumber(const QString &buf, int pos)
{
    return int(QStringView(buf).left(qBound(0, pos, int(buf.size()))).count(u'\n'));
}

int lineCount(const QString &buf)
{
    return int(buf.count(u'\n')) + 1;
}

int startOfLineNumber(const QString &buf, int line)
{
    int pos = 0;
    for (int i = 0; i < line; ++i) {
        const int nl = buf.indexOf(u'\n', pos);
        if (nl < 0)
            break;
        pos = nl + 1;
    }
    return pos;
}

int firstNonBlank(const QString &buf, int pos)
{
    const int ls = lineStart(buf, pos);
    const int le = lineEnd(buf, ls);
    int p = ls;
    while (p < le && isBlank(buf.at(p)))
        ++p;
    return p;
}

bool emptyLineAt(const QString &buf, int p)
{
    if (p >= buf.size())
        return p == buf.size() && (p == 0 || buf.at(p - 1) == u'\n');
    return buf.at(p) == u'\n' && (p == 0 || buf.at(p - 1) == u'\n');
}

int clampToLine(const QString &buf, int pos)
{
    pos = qBound(0, pos, int(buf.size()));
    const int ls = lineStart(buf, pos);
    const int le = lineEnd(buf, ls);
    return le > ls ? qMin(pos, le - 1) : ls;
}

int wordForward(const QString &buf, int pos)
{
    const int n = int(buf.size());
    if (pos >= n)
        return n;
    int p = pos;
    const int cls = charClass(buf.at(p));
    if (cls != 0) {
        while (p < n && charClass(buf.at(p)) == cls)
            ++p;
    }
    while (p < n && charClass(buf.at(p)) == 0) {
        if (buf.at(p) == u'\n' && p + 1 < n && buf.at(p + 1) == u'\n')
            return p + 1;
        ++p;
    }
    return p;
}

int wordBackward(const QString &buf, int pos)
{
    int p = pos - 1;
    if (p <= 0)
        return 0;
    while (p > 0 && charClass(buf.at(p)) == 0) {
        if (emptyLineAt(buf, p) && p != pos)
            return p;
        --p;
    }
    if (charClass(buf.at(p)) == 0)
        return p;
    const int cls = charClass(buf.at(p));
    while (p > 0 && charClass(buf.at(p - 1)) == cls)
        --p;
    return p;
}

int wordEnd(const QString &buf, int pos)
{
    const int n = int(buf.size());
    int p = pos + 1;
    while (p < n && charClass(buf.at(p)) == 0)
        ++p;
    if (p >= n)
        return qMax(0, n - 1);
    const int cls = charClass(buf.at(p));
    while (p + 1 < n && charClass(buf.at(p + 1)) == cls)
        ++p;
    return p;
}

int endOfCurrentWord(const QString &buf, int pos)
{
    const int n = int(buf.size());
    const int cls = charClass(buf.at(pos));
    int p = pos;
    while (p + 1 < n && charClass(buf.at(p + 1)) == cls)
        ++p;
    return p;
}

QString repeated(const QString &text, int count)
{
    QString out;
    out.reserve(text.size() * count);
    for (int i = 0; i < count; ++i)
        out += text;
    return out;
}

} // namespace

VimKeymap::VimKeymap(QObject *parent)
    : QObject(parent)
{
}

QString VimKeymap::pending() const
{
    QString out;
    if (m_opCount > 0)
        out += QString::number(m_opCount);
    switch (m_op) {
    case Op::Delete: out += u'd'; break;
    case Op::Change: out += u'c'; break;
    case Op::Yank: out += u'y'; break;
    case Op::None: break;
    }
    if (m_count > 0)
        out += QString::number(m_count);
    if (m_gPending)
        out += u'g';
    return out;
}

QString VimKeymap::registerText() const
{
    return readRegister();
}

void VimKeymap::setNormal(bool normal)
{
    if (m_normal == normal)
        return;
    m_normal = normal;
    Q_EMIT modeChanged();
}

void VimKeymap::clearPending()
{
    const bool had = m_count || m_opCount || m_op != Op::None || m_gPending;
    m_count = 0;
    m_opCount = 0;
    m_op = Op::None;
    m_gPending = false;
    if (had)
        Q_EMIT pendingChanged();
}

int VimKeymap::takeCount()
{
    const int a = m_opCount > 0 ? m_opCount : 1;
    const int b = m_count > 0 ? m_count : 1;
    return qMin(kMaxCount, a * b);
}

void VimKeymap::setRegister(const QString &text)
{
    m_register = text;
    if (!m_useClipboard)
        return;
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        if (QClipboard *clipboard = app->clipboard())
            clipboard->setText(text);
    }
}

QString VimKeymap::readRegister() const
{
    if (m_useClipboard) {
        if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
            if (QClipboard *clipboard = app->clipboard()) {
                const QString external = clipboard->text();
                if (!external.isEmpty())
                    return external;
            }
        }
    }
    return m_register;
}

int VimKeymap::enterNormal(const QString &buffer, int cursor)
{
    clearPending();
    m_wantColumn = -1;
    setNormal(true);
    cursor = qBound(0, cursor, int(buffer.size()));
    if (cursor > lineStart(buffer, cursor))
        --cursor;
    return clampToLine(buffer, cursor);
}

void VimKeymap::enterInsert()
{
    clearPending();
    m_wantColumn = -1;
    setNormal(false);
}

int VimKeymap::clampNormal(const QString &buffer, int cursor) const
{
    return clampToLine(buffer, cursor);
}

QString VimKeymap::richInsertHtml(const QString &text)
{
    QString escaped = text.toHtmlEscaped();
    escaped.replace(u'\n', QStringLiteral("<br>"));
    return QStringLiteral("<span style=\"white-space:pre-wrap\">") + escaped
        + QStringLiteral("</span>");
}

QVariantMap VimKeymap::toVariant(const Result &r)
{
    QVariantMap map;
    map.insert(QStringLiteral("accepted"), r.accepted);
    map.insert(QStringLiteral("cursor"), r.cursor);
    map.insert(QStringLiteral("hasEdit"), r.hasEdit);
    map.insert(QStringLiteral("editStart"), r.editStart);
    map.insert(QStringLiteral("editEnd"), r.editEnd);
    map.insert(QStringLiteral("editText"), r.editText);
    map.insert(QStringLiteral("undo"), r.undo);
    map.insert(QStringLiteral("redo"), r.redo);
    map.insert(QStringLiteral("send"), r.send);
    map.insert(QStringLiteral("clampAfter"), r.clampAfter);
    return map;
}

QVariantMap VimKeymap::handleKey(int key, int modifiers, const QString &text,
                                 const QString &buffer, int cursor)
{
    return toVariant(handle(key, Qt::KeyboardModifiers(modifiers), text, buffer,
                            cursor));
}

VimKeymap::Result VimKeymap::handle(int key, Qt::KeyboardModifiers modifiers,
                                    const QString &text, const QString &buffer,
                                    int cursor)
{
    Result r;
    if (!m_normal)
        return r;
    const int pos = clampToLine(buffer, cursor);
    const Qt::KeyboardModifiers chord =
        modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);

    if (key == Qt::Key_Escape) {
        const bool hadPending = !pending().isEmpty();
        clearPending();
        r.accepted = hadPending;
        return r;
    }
    if (chord == Qt::ControlModifier && key == Qt::Key_R) {
        r.accepted = true;
        r.redo = takeCount();
        r.clampAfter = true;
        clearPending();
        return r;
    }
    if (chord != Qt::NoModifier)
        return r;

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        clearPending();
        setNormal(false);
        r.accepted = true;
        r.send = true;
        return r;
    case Qt::Key_Left: return command(u'h', buffer, pos);
    case Qt::Key_Right: return command(u'l', buffer, pos);
    case Qt::Key_Up: return command(u'k', buffer, pos);
    case Qt::Key_Down: return command(u'j', buffer, pos);
    case Qt::Key_Home: return command(u'0', buffer, pos);
    case Qt::Key_End: return command(u'$', buffer, pos);
    case Qt::Key_Backspace: return command(u'h', buffer, pos);
    case Qt::Key_Delete: return command(u'x', buffer, pos);
    case Qt::Key_Space: return command(u'l', buffer, pos);
    default: break;
    }

    if (text.size() == 1 && text.at(0).isPrint())
        return command(text.at(0), buffer, pos);
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab
        || (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        || key == Qt::Key_PageUp || key == Qt::Key_PageDown
        || key == Qt::Key_Menu)
        return r;
    // Anything else (dead keys, unknown) is swallowed so Normal mode never types.
    r.accepted = text.size() > 0;
    return r;
}

namespace {

bool isMotionKey(QChar c)
{
    return QStringView(u"hjklwbe0^$Gg").contains(c);
}

} // namespace

int VimKeymap::motionTarget(QChar motion, int count, bool hasCount,
                            const QString &buf, int pos)
{
    const int ls = lineStart(buf, pos);
    const int le = lineEnd(buf, ls);
    const int line = lineNumber(buf, pos);
    const int last = lineCount(buf) - 1;
    const bool vertical = motion == u'j' || motion == u'k';
    if (!vertical && motion != u'$')
        m_wantColumn = -1;

    switch (motion.unicode()) {
    case u'h': return qMax(ls, pos - count);
    case u'l': return le > ls ? qMin(le - 1, pos + count) : ls;
    case u'0': return ls;
    case u'^': return firstNonBlank(buf, pos);
    case u'$': {
        m_wantColumn = kMaxCount * 100;
        const int s = startOfLineNumber(buf, qMin(last, line + count - 1));
        return lineEnd(buf, s);
    }
    case u'w': {
        int p = pos;
        for (int i = 0; i < count; ++i)
            p = wordForward(buf, p);
        return p;
    }
    case u'b': {
        int p = pos;
        for (int i = 0; i < count; ++i)
            p = wordBackward(buf, p);
        return p;
    }
    case u'e': {
        int p = pos;
        for (int i = 0; i < count; ++i)
            p = wordEnd(buf, p);
        return p;
    }
    case u'G':
    case u'g': {
        const int target = hasCount ? qBound(0, count - 1, last)
                                    : (motion == u'G' ? last : 0);
        return firstNonBlank(buf, startOfLineNumber(buf, target));
    }
    case u'j':
    case u'k': {
        if (m_wantColumn < 0)
            m_wantColumn = pos - ls;
        const int target = motion == u'j' ? qMin(last, line + count)
                                          : qMax(0, line - count);
        const int s = startOfLineNumber(buf, target);
        const int e = lineEnd(buf, s);
        return e > s ? qMin(s + m_wantColumn, e - 1) : s;
    }
    default: return pos;
    }
}

VimKeymap::Range VimKeymap::motionRange(QChar motion, int count, bool hasCount,
                                        const QString &buf, int pos, Op op) const
{
    Range rg;
    const int n = int(buf.size());
    const int ls = lineStart(buf, pos);
    const int le = lineEnd(buf, ls);
    const int line = lineNumber(buf, pos);
    const int last = lineCount(buf) - 1;
    auto charwise = [&](int a, int b) {
        rg.valid = true;
        rg.start = qMin(a, b);
        rg.end = qMax(a, b);
        rg.anchor = rg.start;
    };
    auto lines = [&](int l1, int l2) {
        rg.valid = true;
        rg.linewise = true;
        rg.start = startOfLineNumber(buf, l1);
        rg.end = lineEnd(buf, startOfLineNumber(buf, l2));
        if (l1 == line) {
            rg.anchor = pos;
        } else {
            const int e = lineEnd(buf, rg.start);
            rg.anchor = e > rg.start ? qMin(rg.start + (pos - ls), e - 1) : rg.start;
        }
    };

    switch (motion.unicode()) {
    case u'h': charwise(qMax(ls, pos - count), pos); break;
    case u'l': charwise(pos, qMin(le, pos + count)); break;
    case u'0': charwise(ls, pos); break;
    case u'^': charwise(firstNonBlank(buf, pos), pos); break;
    case u'$': {
        const int s = startOfLineNumber(buf, qMin(last, line + count - 1));
        charwise(pos, lineEnd(buf, s));
        break;
    }
    case u'w': {
        if (op == Op::Change && pos < n && charClass(buf.at(pos)) != 0) {
            int p = endOfCurrentWord(buf, pos);
            for (int i = 1; i < count; ++i)
                p = wordEnd(buf, p);
            charwise(pos, qMin(n, p + 1));
            break;
        }
        int p = pos;
        int prev = pos;
        for (int i = 0; i < count; ++i) {
            prev = p;
            p = wordForward(buf, p);
        }
        // An operator ends a word motion at the end of the last word's line,
        // never on the next line.
        if (lineNumber(buf, p) > lineNumber(buf, prev))
            p = lineEnd(buf, prev);
        charwise(pos, p);
        break;
    }
    case u'b': {
        int p = pos;
        for (int i = 0; i < count; ++i)
            p = wordBackward(buf, p);
        charwise(p, pos);
        break;
    }
    case u'e': {
        int p = pos;
        for (int i = 0; i < count; ++i)
            p = wordEnd(buf, p);
        charwise(pos, n == 0 ? 0 : qMin(n, p + 1));
        break;
    }
    case u'j':
        if (line < last)
            lines(line, qMin(last, line + count));
        break;
    case u'k':
        if (line > 0)
            lines(qMax(0, line - count), line);
        break;
    case u'G':
    case u'g': {
        const int target = hasCount ? qBound(0, count - 1, last)
                                    : (motion == u'G' ? last : 0);
        lines(qMin(line, target), qMax(line, target));
        break;
    }
    case u'L':
        lines(line, qMin(last, line + count - 1));
        break;
    default: break;
    }
    return rg;
}

VimKeymap::Result VimKeymap::applyOperator(Op op, const Range &range,
                                           const QString &buf, int pos)
{
    Q_UNUSED(pos);
    Result r;
    r.accepted = true;
    const int n = int(buf.size());
    const QString slice = buf.mid(range.start, range.end - range.start);

    if (range.linewise) {
        const QString lines = slice + u'\n';
        setRegister(lines);
        if (op == Op::Yank) {
            r.cursor = range.anchor;
            return r;
        }
        if (op == Op::Change) {
            r.hasEdit = range.end > range.start;
            r.editStart = range.start;
            r.editEnd = range.end;
            r.cursor = range.start;
            setNormal(false);
            return r;
        }
        int s = range.start;
        int e = range.end;
        if (e < n)
            ++e;
        else if (s > 0)
            --s;
        r.hasEdit = e > s;
        r.editStart = s;
        r.editEnd = e;
        const QString after = buf.left(s) + buf.mid(e);
        const int at = qMin(s, int(after.size()));
        r.cursor = clampToLine(after, firstNonBlank(after, lineStart(after, at)));
        return r;
    }

    if (op == Op::Yank) {
        if (!slice.isEmpty())
            setRegister(slice);
        r.cursor = range.anchor;
        return r;
    }
    if (!slice.isEmpty())
        setRegister(slice);
    r.hasEdit = !slice.isEmpty();
    r.editStart = range.start;
    r.editEnd = range.end;
    if (op == Op::Change) {
        r.cursor = range.start;
        setNormal(false);
        return r;
    }
    const QString after = buf.left(range.start) + buf.mid(range.end);
    r.cursor = clampToLine(after, range.start);
    return r;
}

VimKeymap::Result VimKeymap::paste(bool after, int count, const QString &buf,
                                   int pos)
{
    Result r;
    r.accepted = true;
    const QString reg = readRegister();
    if (reg.isEmpty())
        return r;
    r.hasEdit = true;
    if (reg.endsWith(u'\n')) {
        const QString content = repeated(reg, count);
        int lineAt = 0;
        if (after) {
            const int le = lineEnd(buf, pos);
            r.editStart = r.editEnd = le;
            r.editText = u'\n' + content.chopped(1);
            lineAt = le + 1;
        } else {
            const int ls = lineStart(buf, pos);
            r.editStart = r.editEnd = ls;
            r.editText = content;
            lineAt = ls;
        }
        const QString result = buf.left(r.editStart) + r.editText + buf.mid(r.editStart);
        r.cursor = clampToLine(result, firstNonBlank(result, lineAt));
        return r;
    }
    const QString content = repeated(reg, count);
    int at = pos;
    if (after && pos < lineEnd(buf, pos))
        ++at;
    r.editStart = r.editEnd = at;
    r.editText = content;
    const QString result = buf.left(at) + content + buf.mid(at);
    r.cursor = clampToLine(result, at + int(content.size()) - 1);
    return r;
}

VimKeymap::Result VimKeymap::command(QChar c, const QString &buf, int pos)
{
    Result r;
    r.accepted = true;

    if (c.isDigit() && (c != u'0' || m_count > 0)) {
        m_count = qMin(kMaxCount, m_count * 10 + c.digitValue());
        Q_EMIT pendingChanged();
        return r;
    }

    QChar key = c;
    if (m_gPending) {
        m_gPending = false;
        Q_EMIT pendingChanged();
        if (c != u'g') {
            clearPending();
            return r;
        }
    } else if (c == u'g') {
        m_gPending = true;
        Q_EMIT pendingChanged();
        return r;
    }

    const bool hasCount = m_count > 0 || m_opCount > 0;
    const int count = takeCount();

    if (m_op != Op::None) {
        const Op op = m_op;
        const QChar self = op == Op::Delete ? u'd' : op == Op::Change ? u'c' : u'y';
        Range range;
        if (key == self)
            range = motionRange(u'L', count, hasCount, buf, pos, op);
        else if (isMotionKey(key))
            range = motionRange(key, count, hasCount, buf, pos, op);
        clearPending();
        m_wantColumn = -1;
        if (!range.valid)
            return r;
        return applyOperator(op, range, buf, pos);
    }

    if (isMotionKey(key)) {
        const int target = motionTarget(key, count, hasCount, buf, pos);
        clearPending();
        r.cursor = clampToLine(buf, target);
        return r;
    }

    clearPending();
    m_wantColumn = -1;
    const int ls = lineStart(buf, pos);
    const int le = lineEnd(buf, ls);

    switch (key.unicode()) {
    case u'd':
    case u'c':
    case u'y':
        m_op = key == u'd' ? Op::Delete : key == u'c' ? Op::Change : Op::Yank;
        m_opCount = hasCount ? count : 0;
        Q_EMIT pendingChanged();
        return r;
    case u'x':
        if (le > ls) {
            Range rg;
            rg.valid = true;
            rg.start = pos;
            rg.end = qMin(le, pos + count);
            return applyOperator(Op::Delete, rg, buf, pos);
        }
        return r;
    case u'X':
        if (pos > ls) {
            Range rg;
            rg.valid = true;
            rg.start = qMax(ls, pos - count);
            rg.end = pos;
            return applyOperator(Op::Delete, rg, buf, pos);
        }
        return r;
    case u'D':
        return applyOperator(Op::Delete,
                             motionRange(u'$', count, hasCount, buf, pos, Op::Delete),
                             buf, pos);
    case u'C':
        return applyOperator(Op::Change,
                             motionRange(u'$', count, hasCount, buf, pos, Op::Change),
                             buf, pos);
    case u'i':
        r.cursor = pos;
        setNormal(false);
        return r;
    case u'a':
        r.cursor = pos < le ? pos + 1 : pos;
        setNormal(false);
        return r;
    case u'I':
        r.cursor = firstNonBlank(buf, pos);
        setNormal(false);
        return r;
    case u'A':
        r.cursor = le;
        setNormal(false);
        return r;
    case u'o':
        r.hasEdit = true;
        r.editStart = r.editEnd = le;
        r.editText = QStringLiteral("\n");
        r.cursor = le + 1;
        setNormal(false);
        return r;
    case u'O':
        r.hasEdit = true;
        r.editStart = r.editEnd = ls;
        r.editText = QStringLiteral("\n");
        r.cursor = ls;
        setNormal(false);
        return r;
    case u'p':
    case u'P':
        return paste(key == u'p', count, buf, pos);
    case u'u':
        r.undo = count;
        r.clampAfter = true;
        return r;
    default:
        return r;
    }
}
