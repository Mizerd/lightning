#pragma once

#include "matrix/TimelineEvent.h"

#include <QList>
#include <QString>

namespace roomexport {

enum class Format {
    PlainText,
    Json,
    Html,
};

struct Options {
    QString roomName;
    QString roomId;
    QString exportedBy;
    bool encrypted = false;
    /// Required before any decrypted body of an encrypted room is written. When
    /// false, bodies render as a withheld marker so the user still gets the
    /// conversation's shape.
    bool allowEncryptedPlaintext = false;
    bool use24HourClock = false;

    QString scope = QStringLiteral("loaded-timeline");
    QString rangeDescription;
    bool reachesStartOfRoom = false;
    /// Messages present but unreadable (no key). -1 when not counted.
    int undecryptableCount = -1;
    bool includeAttachments = false;
    qint64 attachmentBudgetBytes = 0;
};

bool isExportable(const TimelineEvent &event);

bool isAttachment(const TimelineEvent &event);

QString withheldBody();

int exportableCount(const QList<TimelineEvent> &events);

QString renderPlainText(const QList<TimelineEvent> &events,
                        const Options &options);
QString renderJson(const QList<TimelineEvent> &events, const Options &options);

QString render(const QList<TimelineEvent> &events, const Options &options,
               Format format);

QString placeholderBody(const TimelineEvent &event);

QString stateOrBody(const TimelineEvent &event);

QString suggestedFileName(const Options &options, Format format);

}
