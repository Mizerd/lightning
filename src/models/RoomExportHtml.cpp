#include "models/RoomExportHtml.h"

#include "models/MessageHtml.h"

#include <QBuffer>
#include <QDateTime>
#include <QIODevice>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>

#include <cstring>
#include <utility>

namespace roomexport {
namespace {

// Nothing may load from anywhere but this file, and nothing may run. The
// base-uri and form-action directives are not covered by default-src.
const QString kCsp = QStringLiteral(
    "default-src 'none'; img-src data:; media-src data:; "
    "style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'");

// Readable in both schemes, no web fonts (they would be a request).
const char kStyle[] = R"CSS(
:root { color-scheme: light dark; --bg: #ffffff; --fg: #1d1f23; --muted: #5f6672;
  --line: #e3e5e8; --card: #f5f6f8; --link: #0b62c4; --note: #8a5a00; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #15171a; --fg: #e6e8eb; --muted: #9aa1ab; --line: #2b2f35;
    --card: #1e2125; --link: #7db4ff; --note: #e0b25c; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg);
  font: 15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
main, header.export, footer { max-width: 860px; margin: 0 auto; padding: 0 16px; }
header.export { padding-top: 24px; padding-bottom: 8px; border-bottom: 1px solid var(--line); }
header.export h1 { margin: 0 0 4px; font-size: 22px; overflow-wrap: anywhere; }
header.export p { margin: 2px 0; color: var(--muted); font-size: 13px; }
header.export p.warn { color: var(--note); }
section.day h2 { position: sticky; top: 0; margin: 20px 0 8px; padding: 6px 0;
  background: var(--bg); color: var(--muted); font-size: 13px; font-weight: 600;
  border-bottom: 1px solid var(--line); }
article.msg { padding: 6px 0; overflow-wrap: anywhere; }
.meta { font-size: 13px; color: var(--muted); }
.sender { color: var(--fg); font-weight: 600; }
.tag { display: inline-block; margin-left: 6px; padding: 0 6px; border-radius: 8px;
  background: var(--card); font-size: 11px; }
.reply { font-size: 12px; color: var(--muted); border-left: 3px solid var(--line);
  padding-left: 8px; margin: 2px 0; }
.body { margin: 2px 0; }
.body.plain { white-space: pre-wrap; }
.note { color: var(--note); font-style: italic; margin: 2px 0; }
.state { color: var(--muted); font-size: 13px; font-style: italic; }
a { color: var(--link); }
figure { margin: 6px 0; }
img, video { max-width: 100%; max-height: 640px; border-radius: 6px; }
audio { width: 100%; max-width: 480px; }
.file { margin: 4px 0; padding: 6px 10px; border-radius: 6px; background: var(--card);
  display: inline-block; max-width: 100%; }
pre, code { background: var(--card); border-radius: 4px; }
pre { padding: 8px; overflow-x: auto; white-space: pre-wrap; }
blockquote { margin: 4px 0; padding-left: 10px; border-left: 3px solid var(--line); }
footer { padding: 24px 16px; color: var(--muted); font-size: 12px; }
)CSS";

QString esc(const QString &text)
{
    return text.toHtmlEscaped();
}

QString senderName(const TimelineEvent &event)
{
    const QString name = event.senderDisplayName.trimmed();
    return name.isEmpty() ? event.sender : name;
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0,
                                       'f', 1);
}

QString attachmentLabel(const TimelineEvent &event)
{
    switch (event.type) {
    case TimelineEvent::Image:   return QStringLiteral("image");
    case TimelineEvent::Video:   return QStringLiteral("video");
    case TimelineEvent::Audio:
        return event.mediaIsVoice ? QStringLiteral("voice message")
                                  : QStringLiteral("audio");
    case TimelineEvent::Sticker: return QStringLiteral("sticker");
    default:                     return QStringLiteral("file");
    }
}

QString attachmentName(const TimelineEvent &event)
{
    const QString name = event.mediaFilename.trimmed().isEmpty()
        ? event.body.trimmed() : event.mediaFilename.trimmed();
    return name.isEmpty() ? QStringLiteral("attachment") : name;
}

bool isVisual(const TimelineEvent &event)
{
    return event.type == TimelineEvent::Image
        || event.type == TimelineEvent::Sticker;
}

bool at(const QByteArray &b, qsizetype offset, const char *magic)
{
    const qsizetype n = qsizetype(qstrlen(magic));
    return b.size() >= offset + n
        && std::memcmp(b.constData() + offset, magic, size_t(n)) == 0;
}

QByteArray sniffImage(const QByteArray &b)
{
    if (at(b, 0, "\x89PNG\r\n\x1a\n"))
        return QByteArrayLiteral("image/png");
    if (b.size() >= 3 && static_cast<unsigned char>(b.at(0)) == 0xFF
        && static_cast<unsigned char>(b.at(1)) == 0xD8
        && static_cast<unsigned char>(b.at(2)) == 0xFF)
        return QByteArrayLiteral("image/jpeg");
    if (at(b, 0, "GIF87a") || at(b, 0, "GIF89a"))
        return QByteArrayLiteral("image/gif");
    if (at(b, 0, "RIFF") && at(b, 8, "WEBP"))
        return QByteArrayLiteral("image/webp");
    if (at(b, 0, "BM") && b.size() > 26)
        return QByteArrayLiteral("image/bmp");
    if (at(b, 4, "ftypavif") || at(b, 4, "ftypavis"))
        return QByteArrayLiteral("image/avif");
    return {};
}

QByteArray sniffVideo(const QByteArray &b)
{
    if (at(b, 4, "ftyp"))
        return QByteArrayLiteral("video/mp4");
    if (at(b, 0, "\x1a\x45\xdf\xa3"))
        return QByteArrayLiteral("video/webm");
    if (at(b, 0, "OggS"))
        return QByteArrayLiteral("video/ogg");
    return {};
}

QByteArray sniffAudio(const QByteArray &b)
{
    if (at(b, 0, "OggS"))
        return QByteArrayLiteral("audio/ogg");
    if (at(b, 0, "ID3"))
        return QByteArrayLiteral("audio/mpeg");
    if (b.size() >= 2 && static_cast<unsigned char>(b.at(0)) == 0xFF
        && (static_cast<unsigned char>(b.at(1)) & 0xE0) == 0xE0)
        return QByteArrayLiteral("audio/mpeg");
    if (at(b, 0, "fLaC"))
        return QByteArrayLiteral("audio/flac");
    if (at(b, 0, "RIFF") && at(b, 8, "WAVE"))
        return QByteArrayLiteral("audio/wav");
    if (at(b, 4, "ftyp"))
        return QByteArrayLiteral("audio/mp4");
    if (at(b, 0, "\x1a\x45\xdf\xa3"))
        return QByteArrayLiteral("audio/webm");
    return {};
}

// http(s) URLs in a plain body become links; everything else is escaped text.
QString linkifyPlain(const QString &text)
{
    static const QRegularExpression urlPattern(
        QStringLiteral("https?://[^\\s<>\"]+"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype cursor = 0;
    auto it = urlPattern.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        QString candidate = match.captured(0);
        // Sentence punctuation after a URL is not part of it.
        while (!candidate.isEmpty()
               && QStringLiteral(".,;:!?)]}'").contains(candidate.back()))
            candidate.chop(1);
        const QUrl url(candidate, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        out += esc(text.mid(cursor, match.capturedStart() - cursor));
        if (url.isValid() && !url.host().isEmpty()
            && (scheme == QLatin1String("http")
                || scheme == QLatin1String("https"))) {
            out += QStringLiteral("<a href=\"") + esc(candidate)
                + QStringLiteral("\" rel=\"noopener noreferrer\">")
                + esc(candidate) + QStringLiteral("</a>");
        } else {
            out += esc(candidate);
        }
        cursor = match.capturedStart() + candidate.size();
    }
    out += esc(text.mid(cursor));
    return out;
}

} // namespace

QString contentSecurityPolicy()
{
    return kCsp;
}

bool declaredSvg(const TimelineEvent &event)
{
    const QString mime = event.mediaMimetype.trimmed().toLower();
    if (mime.startsWith(QLatin1String("image/svg")))
        return true;
    for (const QString &name : { event.mediaFilename, event.body }) {
        const QString lower = name.trimmed().toLower();
        if (lower.endsWith(QLatin1String(".svg"))
            || lower.endsWith(QLatin1String(".svgz")))
            return true;
    }
    return false;
}

bool looksLikeMarkupOrCompressed(const QByteArray &bytes)
{
    qsizetype i = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes.at(0)) == 0xEF
        && static_cast<unsigned char>(bytes.at(1)) == 0xBB
        && static_cast<unsigned char>(bytes.at(2)) == 0xBF)
        i = 3;
    const auto isXmlSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (i < bytes.size() && isXmlSpace(bytes.at(i)))
        ++i;
    if (i < bytes.size() && bytes.at(i) == '<')
        return true;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes.at(0)) == 0x1F
        && static_cast<unsigned char>(bytes.at(1)) == 0x8B)
        return true;
    return false;
}

bool refusedAsSvg(const TimelineEvent &event, const QByteArray &bytes)
{
    return declaredSvg(event)
        || (isVisual(event) && looksLikeMarkupOrCompressed(bytes));
}

EmbedPlan planEmbed(const TimelineEvent &event, const QByteArray &bytes)
{
    EmbedPlan plan;
    QByteArray mime;
    switch (event.type) {
    case TimelineEvent::Image:
    case TimelineEvent::Sticker:
        mime = sniffImage(bytes);
        if (!mime.isEmpty())
            plan.element = EmbedElement::Image;
        break;
    case TimelineEvent::Video:
        mime = sniffVideo(bytes);
        if (!mime.isEmpty())
            plan.element = EmbedElement::Video;
        break;
    case TimelineEvent::Audio:
        mime = sniffAudio(bytes);
        if (!mime.isEmpty())
            plan.element = EmbedElement::Audio;
        break;
    default:
        break;
    }
    // Anything unrecognised is offered as an inert download, never rendered.
    if (plan.element != EmbedElement::Download)
        plan.mime = mime;
    return plan;
}

QString safeDownloadName(const QString &raw)
{
    QString name = raw.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                 QStringLiteral("-"));
    for (QChar &c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char('-');
    }
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    name = name.trimmed();
    if (name.size() > 120)
        name = name.right(120);
    return name.isEmpty() ? QStringLiteral("attachment") : name;
}

HtmlWriter::HtmlWriter(QIODevice *out, Options options)
    : m_out(out), m_options(std::move(options))
{
}

void HtmlWriter::setDisplayNames(const QHash<QString, QString> &names)
{
    m_names = names;
}

bool HtmlWriter::write(const QByteArray &bytes)
{
    if (!m_ok || !m_out)
        return false;
    if (m_out->write(bytes) != bytes.size())
        m_ok = false;
    return m_ok;
}

bool HtmlWriter::writeHeader(int messageCount)
{
    const QString title = m_options.roomName.isEmpty() ? m_options.roomId
                                                       : m_options.roomName;
    QString h;
    h += QStringLiteral("<!DOCTYPE html>\n<html>\n<head>\n");
    // First in <head>, so no element parsed before it can act.
    h += QStringLiteral("<meta http-equiv=\"Content-Security-Policy\" "
                        "content=\"") + esc(kCsp) + QStringLiteral("\">\n");
    h += QStringLiteral("<meta charset=\"utf-8\">\n"
                        "<meta name=\"referrer\" content=\"no-referrer\">\n"
                        "<meta name=\"viewport\" "
                        "content=\"width=device-width, initial-scale=1\">\n"
                        "<meta name=\"generator\" content=\"Lightning\">\n");
    h += QStringLiteral("<title>") + esc(title) + QStringLiteral("</title>\n");
    h += QStringLiteral("<style>") + QString::fromUtf8(kStyle)
        + QStringLiteral("</style>\n</head>\n<body>\n");
    h += QStringLiteral("<header class=\"export\">\n<h1>") + esc(title)
        + QStringLiteral("</h1>\n");
    if (!m_options.roomName.isEmpty())
        h += QStringLiteral("<p>") + esc(m_options.roomId)
            + QStringLiteral("</p>\n");
    const QString when = QLocale().toString(QDateTime::currentDateTime(),
                                            QLocale::LongFormat);
    h += QStringLiteral("<p>Exported from Lightning on ") + esc(when);
    if (!m_options.exportedBy.isEmpty())
        h += QStringLiteral(" by ") + esc(m_options.exportedBy);
    h += QStringLiteral("</p>\n");
    h += QStringLiteral("<p>This export carries ")
        + esc(m_options.rangeDescription.isEmpty()
                  ? QStringLiteral("the messages Lightning had loaded")
                  : m_options.rangeDescription)
        + QStringLiteral(".");
    if (m_options.reachesStartOfRoom)
        h += QStringLiteral(" It reaches the start of the room's history this "
                            "account can read.");
    h += QStringLiteral("</p>\n");
    h += QStringLiteral("<p>Messages in this file: %1</p>\n").arg(messageCount);
    if (m_options.undecryptableCount > 0)
        h += QStringLiteral("<p class=\"warn\">Messages that could not be "
                            "decrypted: %1</p>\n")
                 .arg(m_options.undecryptableCount);
    const bool textAllowed =
        !m_options.encrypted || m_options.allowEncryptedPlaintext;
    if (m_options.encrypted) {
        h += textAllowed
            ? QStringLiteral("<p class=\"warn\">This room is ENCRYPTED. The "
                             "messages below are its decrypted text, written "
                             "in the clear.</p>\n")
            : QStringLiteral("<p class=\"warn\">This room is ENCRYPTED and the "
                             "export was made without its text or "
                             "attachments, so only the shape of the "
                             "conversation is here.</p>\n");
    }
    if (m_options.includeAttachments && textAllowed) {
        h += QStringLiteral("<p>Attachments are embedded in this file, up to "
                            "%1 in total. Any that did not fit or could not "
                            "be downloaded are listed by name.</p>\n")
                 .arg(formatBytes(m_options.attachmentBudgetBytes));
    } else {
        h += QStringLiteral("<p>Attachments are not included; each is listed "
                            "by name.</p>\n");
    }
    h += QStringLiteral("</header>\n<main>\n");
    return write(h);
}

QString HtmlWriter::formattedHtml(const QString &formatted) const
{
    const QHash<QString, QString> &names = m_names;
    QString safe = MessageHtml::sanitize(
        formatted,
        [&names](const QString &userId) { return names.value(userId); },
        m_options.exportedBy, MessageHtml::MentionStyle{},
        /*revealSpoilers=*/true);

    // The sanitizer's only <img> is a custom emoji addressed by mxc:, which
    // this page could not load anyway: keep its alt text.
    static const QRegularExpression imgTag(
        QStringLiteral("<img\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression altAttr(QStringLiteral("alt=\"([^\"]*)\""));
    QString noImages;
    qsizetype cursor = 0;
    auto images = imgTag.globalMatch(safe);
    while (images.hasNext()) {
        const QRegularExpressionMatch m = images.next();
        noImages += safe.mid(cursor, m.capturedStart() - cursor);
        // Already escaped by the sanitizer, and still text here.
        noImages += altAttr.match(m.captured(0)).captured(1);
        cursor = m.capturedEnd();
    }
    noImages += safe.mid(cursor);

    // Internal anchors (mention:, spoiler:toggle) mean nothing in a file:
    // unwrap every link that is not http(s), keeping its text.
    static const QRegularExpression anchorTag(
        QStringLiteral("<a\\b[^>]*>|</a\\s*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hrefAttr(
        QStringLiteral("href=\"([^\"]*)\""));
    QString out;
    QList<bool> kept;
    cursor = 0;
    auto anchors = anchorTag.globalMatch(noImages);
    while (anchors.hasNext()) {
        const QRegularExpressionMatch m = anchors.next();
        out += noImages.mid(cursor, m.capturedStart() - cursor);
        cursor = m.capturedEnd();
        const QString tag = m.captured(0);
        if (tag.startsWith(QLatin1String("</"))) {
            if (!kept.isEmpty() && kept.takeLast())
                out += QStringLiteral("</a>");
            continue;
        }
        const QString href = hrefAttr.match(tag).captured(1);
        const QString lower = href.toLower();
        const bool external = lower.startsWith(QLatin1String("https://"))
            || lower.startsWith(QLatin1String("http://"));
        kept.append(external);
        if (external)
            out += QStringLiteral("<a href=\"") + href
                + QStringLiteral("\" rel=\"noopener noreferrer\">");
    }
    out += noImages.mid(cursor);
    return out;
}

QString HtmlWriter::bodyHtml(const TimelineEvent &event) const
{
    if (m_options.encrypted && !m_options.allowEncryptedPlaintext)
        return QStringLiteral("<p class=\"note\">") + esc(withheldBody())
            + QStringLiteral("</p>");
    const QString placeholder = placeholderBody(event);
    if (!placeholder.isEmpty())
        return QStringLiteral("<p class=\"note\">") + esc(placeholder)
            + QStringLiteral("</p>");
    if (event.type == TimelineEvent::StateChange)
        return QStringLiteral("<div class=\"state\">") + esc(event.body)
            + QStringLiteral("</div>");
    if (isAttachment(event)) {
        // The caption, when the sender wrote one beyond the file's name.
        const QString caption = event.body.trimmed();
        if (caption.isEmpty() || caption == attachmentName(event))
            return QString();
        return QStringLiteral("<div class=\"body plain\">")
            + linkifyPlain(caption) + QStringLiteral("</div>");
    }
    const QString prefix = event.type == TimelineEvent::Emote
        ? QStringLiteral("* ") + esc(senderName(event)) + QLatin1Char(' ')
        : QString();
    if (!event.formattedBody.trimmed().isEmpty())
        return QStringLiteral("<div class=\"body\">") + prefix
            + formattedHtml(event.formattedBody) + QStringLiteral("</div>");
    return QStringLiteral("<div class=\"body plain\">") + prefix
        + linkifyPlain(event.body) + QStringLiteral("</div>");
}

qsizetype HtmlWriter::beginEvent(const TimelineEvent &event,
                                 const Attachment &attachment)
{
    m_payload.clear();
    m_payloadOffset = 0;
    m_rowSuffix.clear();

    QString h;
    const QDateTime when = event.timestamp.toLocalTime();
    if (when.isValid() && when.date() != m_lastDate) {
        if (m_dayOpen)
            h += QStringLiteral("</section>\n");
        m_lastDate = when.date();
        m_dayOpen = true;
        h += QStringLiteral("<section class=\"day\">\n<h2>")
            + esc(QLocale().toString(m_lastDate, QLocale::LongFormat))
            + QStringLiteral("</h2>\n");
    }
    const QString timeFormat = m_options.use24HourClock
        ? QStringLiteral("HH:mm")
        : QLocale().timeFormat(QLocale::ShortFormat);
    const QString stamp = when.isValid() ? when.time().toString(timeFormat)
                                         : QStringLiteral("--:--");

    h += QStringLiteral("<article class=\"msg\" id=\"e-") + esc(event.eventId)
        + QStringLiteral("\">\n<div class=\"meta\">");
    // State rows have no speaker: a name would attribute words to someone.
    if (event.type != TimelineEvent::StateChange)
        h += QStringLiteral("<span class=\"sender\" title=\"")
            + esc(event.sender) + QStringLiteral("\">")
            + esc(senderName(event)) + QStringLiteral("</span> ");
    h += QStringLiteral("<time datetime=\"")
        + esc(event.timestamp.toUTC().toString(Qt::ISODate))
        + QStringLiteral("\">") + esc(stamp) + QStringLiteral("</time>");
    if (!event.threadRootId.isEmpty())
        h += QStringLiteral("<span class=\"tag\">in thread</span>");
    if (event.edited)
        h += QStringLiteral("<span class=\"tag\">edited</span>");
    h += QStringLiteral("</div>\n");
    if (!event.replyToEventId.isEmpty()) {
        h += QStringLiteral("<div class=\"reply\">In reply to <a href=\"#e-")
            + esc(QString::fromLatin1(
                  QUrl::toPercentEncoding(event.replyToEventId)))
            + QStringLiteral("\">an earlier message</a></div>\n");
    }
    h += bodyHtml(event);

    const bool textAllowed =
        !m_options.encrypted || m_options.allowEncryptedPlaintext;
    QString after = QStringLiteral("\n</article>\n");
    if (isAttachment(event) && textAllowed && !event.redacted) {
        const QString name = attachmentName(event);
        const QString label = attachmentLabel(event);
        AttachmentState state = attachment.state;
        if (state == AttachmentState::Embedded
            && refusedAsSvg(event, attachment.bytes))
            state = AttachmentState::Svg;
        if (state == AttachmentState::Embedded && attachment.bytes.isEmpty())
            state = AttachmentState::Failed;

        const auto placeholderLine = [&](const QString &why) -> QString {
            return QStringLiteral("\n<p class=\"file\">[")
                + esc(label) + QStringLiteral(": ") + esc(name)
                + QStringLiteral(" — ") + esc(why) + QStringLiteral("]</p>");
        };
        switch (state) {
        case AttachmentState::NotIncluded:
            h += placeholderLine(QStringLiteral("not included in this export"));
            break;
        case AttachmentState::OverLimit:
            h += placeholderLine(
                QStringLiteral("not included: over the size limit"));
            break;
        case AttachmentState::Svg:
            h += placeholderLine(QStringLiteral(
                "not included: SVG images are never embedded"));
            break;
        case AttachmentState::Failed:
            h += placeholderLine(
                QStringLiteral("not included: it could not be downloaded"));
            break;
        case AttachmentState::Unavailable:
            h += placeholderLine(
                QStringLiteral("not included: it is not available"));
            break;
        case AttachmentState::Embedded: {
            const EmbedPlan plan = planEmbed(event, attachment.bytes);
            const QString dataPrefix = QStringLiteral("data:")
                + QString::fromLatin1(plan.mime) + QStringLiteral(";base64,");
            switch (plan.element) {
            case EmbedElement::Image:
                h += QStringLiteral("\n<figure><img alt=\"") + esc(name)
                    + QStringLiteral("\" src=\"") + dataPrefix;
                after = QStringLiteral("\"></figure>") + after;
                break;
            case EmbedElement::Video:
                h += QStringLiteral("\n<figure><video controls "
                                    "preload=\"metadata\" title=\"")
                    + esc(name) + QStringLiteral("\" src=\"") + dataPrefix;
                after = QStringLiteral("\"></video></figure>") + after;
                break;
            case EmbedElement::Audio:
                h += QStringLiteral("\n<figure><audio controls "
                                    "preload=\"metadata\" title=\"")
                    + esc(name) + QStringLiteral("\" src=\"") + dataPrefix;
                after = QStringLiteral("\"></audio></figure>") + after;
                break;
            case EmbedElement::Download:
                h += QStringLiteral("\n<p class=\"file\"><a download=\"")
                    + esc(safeDownloadName(name)) + QStringLiteral("\" href=\"")
                    + dataPrefix;
                after = QStringLiteral("\">") + esc(name)
                    + QStringLiteral("</a> (")
                    + esc(formatBytes(attachment.bytes.size()))
                    + QStringLiteral(")</p>") + after;
                break;
            }
            m_payload = attachment.bytes;
            break;
        }
        }
    }
    m_rowSuffix = after.toUtf8();
    write(h);
    if (m_payload.isEmpty()) {
        endEvent();
        return 0;
    }
    return m_payload.size();
}

bool HtmlWriter::writePayloadChunk(qsizetype maxBytes)
{
    if (m_payloadOffset >= m_payload.size())
        return false;
    // Whole 3-byte groups, so the chunks' base64 concatenates exactly.
    qsizetype take = qMax<qsizetype>(3, maxBytes - maxBytes % 3);
    take = qMin(take, m_payload.size() - m_payloadOffset);
    write(m_payload.mid(m_payloadOffset, take).toBase64());
    m_payloadOffset += take;
    return m_ok && m_payloadOffset < m_payload.size();
}

bool HtmlWriter::endEvent()
{
    const bool ok = write(m_rowSuffix);
    m_rowSuffix.clear();
    m_payload.clear();
    m_payloadOffset = 0;
    return ok;
}

bool HtmlWriter::writeEvent(const TimelineEvent &event,
                            const Attachment &attachment)
{
    if (beginEvent(event, attachment) == 0)
        return m_ok;
    while (writePayloadChunk(3 * 256 * 1024)) {
    }
    return endEvent();
}

bool HtmlWriter::writeFooter()
{
    QString h;
    if (m_dayOpen)
        h += QStringLiteral("</section>\n");
    m_dayOpen = false;
    h += QStringLiteral("</main>\n<footer>Exported from Lightning.</footer>\n"
                        "</body>\n</html>\n");
    return write(h);
}

QByteArray renderHtml(const QList<TimelineEvent> &events,
                      const Options &options,
                      const QHash<QString, Attachment> &attachments)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    HtmlWriter writer(&buffer, options);
    QHash<QString, QString> names;
    for (const TimelineEvent &event : events) {
        if (!event.senderDisplayName.trimmed().isEmpty())
            names.insert(event.sender, event.senderDisplayName.trimmed());
    }
    writer.setDisplayNames(names);
    writer.writeHeader(exportableCount(events));
    for (const TimelineEvent &event : events) {
        if (!isExportable(event))
            continue;
        writer.writeEvent(event, attachments.value(event.eventId));
    }
    writer.writeFooter();
    buffer.close();
    return bytes;
}

} // namespace roomexport
