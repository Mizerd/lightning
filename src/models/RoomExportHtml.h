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

enum class AttachmentState {
    NotIncluded,
    Embedded,
    OverLimit,
    Svg,
    Failed,
    Unavailable,
};

struct Attachment {
    AttachmentState state = AttachmentState::NotIncluded;
    QByteArray bytes;
};

enum class EmbedElement { Image, Video, Audio, Download };

struct EmbedPlan {
    EmbedElement element = EmbedElement::Download;
    QByteArray mime = QByteArrayLiteral("application/octet-stream");
};

bool declaredSvg(const TimelineEvent &event);

bool looksLikeMarkupOrCompressed(const QByteArray &bytes);

/// True when the writer will refuse to embed `bytes` for `event` as SVG: a
/// declared SVG, or an image/sticker whose bytes open as markup or gzip. The
/// controller asks the same question so a refused payload is not charged to
/// the size limit.
bool refusedAsSvg(const TimelineEvent &event, const QByteArray &bytes);

EmbedPlan planEmbed(const TimelineEvent &event, const QByteArray &bytes);

QString safeDownloadName(const QString &raw);

class HtmlWriter
{
public:
    HtmlWriter(QIODevice *out, Options options);

    void setDisplayNames(const QHash<QString, QString> &names);

    bool writeHeader(int messageCount);
    bool writeEvent(const TimelineEvent &event, const Attachment &attachment);
    bool writeFooter();

    qsizetype beginEvent(const TimelineEvent &event,
                         const Attachment &attachment);
    bool writePayloadChunk(qsizetype maxBytes);
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
    QByteArray m_payload;
    qsizetype m_payloadOffset = 0;
    QByteArray m_rowSuffix;
};

QByteArray renderHtml(const QList<TimelineEvent> &events,
                      const Options &options,
                      const QHash<QString, Attachment> &attachments = {});

QString contentSecurityPolicy();

}
