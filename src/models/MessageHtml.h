#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <functional>

namespace MessageHtml {

struct MentionStyle {
    QString accentColor;
    QString linkColor;
    QString codeBackground;
    bool underlineMentions = false;
};

QString sanitize(
    const QString &html,
    const std::function<QString(const QString &userId)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle = {},
    bool revealSpoilers = false);

enum class SegmentKind {
    RichText,
    CodeBlock,
};

struct Segment {
    SegmentKind kind = SegmentKind::RichText;
    QString text;
    // CodeBlock only. Empty unless the source class was `language-xxx` /
    // `lang-xxx` with xxx matching ^[A-Za-z0-9+#._-]{1,24}$. The class
    // attribute is attacker-chosen; only this validated token leaves the
    // parser.
    QString language;
};

QString markRoomMention(const QString &safeHtml, const QString &color,
                        const QString &fill = QString());

inline constexpr int kInlineEmojiHeight = 20;
inline constexpr int kInlineEmojiMinWidth = 6;
inline constexpr int kInlineEmojiMaxWidth = 60;
QString resolveInlineImages(
    const QString &safeHtml,
    const std::function<QString(const QString &mxcUri)> &resolve,
    const std::function<double(const QString &source)> &aspectOf = {});
int inlineEmojiWidth(double aspect);
std::function<double(const QString &source)> rememberingAspect(
    std::function<QSize(const QString &source)> sizeOf);

QString markEmoji(const QString &safeHtml);

QList<Segment> segments(
    const QString &html,
    const std::function<QString(const QString &userId)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle = {},
    bool revealSpoilers = false);

}
