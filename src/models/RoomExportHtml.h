#pragma once

#include "matrix/TimelineEvent.h"
#include "models/RoomExport.h"

#include <QByteArray>
#include <QDate>
#include <QHash>
#include <QList>
#include <QString>

class QIODevice;

/// The HTML room export: ONE self-contained page, readable offline in any
/// browser, with attachments embedded as `data:` URIs.
///
/// The page cannot reach the network or run code even if sanitising missed
/// something: the first element of <head> is a Content-Security-Policy that
/// allows images and media from `data:` only, inline styles, and nothing
/// else, and the writer emits no <script> at all.
///
/// Message text: a formatted body goes through MessageHtml::sanitize (the
/// timeline's own sanitizer) and is then reduced further for a page that is
/// not the timeline: every <img> becomes its alt text and every link that is
/// not http(s) is unwrapped to its text. A plain body is HTML-escaped and its
/// http(s) URLs become links. Raw event HTML never reaches the file.
///
/// Attachments: the bytes decide what is embedded, never the sender's
/// mimetype. An image is embedded only when its bytes are a raster format the
/// writer recognises, a video or audio file only when its container is
/// recognised; anything else becomes a download link typed
/// application/octet-stream. SVG is never embedded, by declared type, name or
/// content (CLAUDE.md §6).
///
/// Encrypted rooms: as for the other formats, `allowEncryptedPlaintext` false
/// writes a withheld marker in place of every body AND every attachment.
namespace roomexport {

/// What the writer has for one attachment row.
enum class AttachmentState {
    NotIncluded,  ///< Attachments were not asked for (or not this format).
    Embedded,     ///< `bytes` holds the payload.
    OverLimit,    ///< Would not fit in the size limit.
    Svg,          ///< SVG (or markup posing as an image): never embedded.
    Failed,       ///< The download failed.
    Unavailable,  ///< No source to fetch (an unregistered or bare row).
};

struct Attachment {
    AttachmentState state = AttachmentState::NotIncluded;
    QByteArray bytes;
};

/// How embedded bytes are presented.
enum class EmbedElement { Image, Video, Audio, Download };

struct EmbedPlan {
    EmbedElement element = EmbedElement::Download;
    /// The `data:` URI's type: sniffed from the bytes, or octet-stream.
    QByteArray mime = QByteArrayLiteral("application/octet-stream");
};

/// True when the sender declared the file SVG by mimetype or name. Checked
/// before any download, so an SVG attachment is never fetched for embedding.
bool declaredSvg(const TimelineEvent &event);

/// True when the payload opens as markup (after a BOM and whitespace) or as
/// gzip: what an SVG or SVGZ looks like whatever it is called. Same rule as
/// MediaBridge::looksLikeMarkupOrCompressed.
bool looksLikeMarkupOrCompressed(const QByteArray &bytes);

/// True when the writer will refuse to embed `bytes` for `event` as SVG: a
/// declared SVG, or an image/sticker whose bytes open as markup or gzip. The
/// controller asks the same question so a refused payload is not charged to
/// the size limit.
bool refusedAsSvg(const TimelineEvent &event, const QByteArray &bytes);

/// Decide how `bytes` for `event` are embedded, from the bytes themselves.
EmbedPlan planEmbed(const TimelineEvent &event, const QByteArray &bytes);

/// A leaf filename safe for a `download` attribute.
QString safeDownloadName(const QString &raw);

/// Streams the page into `out`. Usage: writeHeader, then for each exportable
/// event in time order writeEvent (or the begin/chunk/end trio, which lets a
/// caller spread a large payload over several event-loop turns), then
/// writeFooter. Every write reports false once the device has failed.
class HtmlWriter
{
public:
    HtmlWriter(QIODevice *out, Options options);

    /// User id -> display name, for mention links in formatted bodies.
    void setDisplayNames(const QHash<QString, QString> &names);

    bool writeHeader(int messageCount);
    bool writeEvent(const TimelineEvent &event, const Attachment &attachment);
    bool writeFooter();

    /// Writes the row up to its embedded payload, or the whole row when there
    /// is none. Returns the payload bytes still to write (0 = row finished).
    qsizetype beginEvent(const TimelineEvent &event,
                         const Attachment &attachment);
    /// Writes up to `maxBytes` of the pending payload as base64. Returns
    /// true while more remains.
    bool writePayloadChunk(qsizetype maxBytes);
    /// Closes the row after its payload.
    bool endEvent();

    bool ok() const { return m_ok; }

private:
    bool write(const QByteArray &bytes);
    bool write(const QString &text) { return write(text.toUtf8()); }
    QString bodyHtml(const TimelineEvent &event) const;
    QString formattedHtml(const QString &formatted) const;

    QIODevice *m_out = nullptr;
    Options m_options;
    QHash<QString, QString> m_names;
    QDate m_lastDate;
    bool m_ok = true;
    bool m_dayOpen = false;
    // The row being written by begin/chunk/end.
    QByteArray m_payload;
    qsizetype m_payloadOffset = 0;
    QByteArray m_rowSuffix;
};

/// Convenience: the whole page in memory, attachments looked up by event id
/// (absent = NotIncluded). For tests and small exports.
QByteArray renderHtml(const QList<TimelineEvent> &events,
                      const Options &options,
                      const QHash<QString, Attachment> &attachments = {});

/// The Content-Security-Policy the page carries, for tests.
QString contentSecurityPolicy();

} // namespace roomexport
