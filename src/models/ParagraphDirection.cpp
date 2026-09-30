#include "models/ParagraphDirection.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

ParagraphDirection::ParagraphDirection(QObject *parent)
    : QObject(parent)
{
}

void ParagraphDirection::setDocument(QQuickTextDocument *document)
{
    if (m_quickDocument == document)
        return;
    QObject::disconnect(m_contentsConnection);
    m_quickDocument = document;
    m_textDocument = document ? document->textDocument() : nullptr;
    // Every setText replaces the whole document (formats included), and
    // contentsChanged follows each replacement once it is complete.
    if (m_textDocument) {
        m_contentsConnection = connect(m_textDocument, &QTextDocument::contentsChanged,
                                       this, &ParagraphDirection::update);
    }
    Q_EMIT documentChanged();
    update();
}

void ParagraphDirection::setLeftToRight(bool value)
{
    if (m_leftToRight == value)
        return;
    m_leftToRight = value;
    Q_EMIT leftToRightChanged();
    update();
}

Qt::LayoutDirection ParagraphDirection::paragraphDirection(QStringView text)
{
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        char32_t cp = text[i].unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < n
            && QChar::isLowSurrogate(text[i + 1].unicode())) {
            cp = QChar::surrogateToUcs4(text[i].unicode(), text[i + 1].unicode());
            ++i;
        }
        switch (QChar::direction(cp)) {
        case QChar::DirL:
            return Qt::LeftToRight;
        case QChar::DirR:
        case QChar::DirAL:
            return Qt::RightToLeft;
        default:
            break;
        }
    }
    return Qt::LayoutDirectionAuto;
}

bool ParagraphDirection::apply(QTextDocument *document, bool leftToRight)
{
    if (!document)
        return false;
    bool rightToLeft = false;
    for (QTextBlock b = document->begin(); b.isValid() && !rightToLeft; b = b.next())
        rightToLeft = paragraphDirection(b.text()) == Qt::RightToLeft;
    if (!rightToLeft && !leftToRight)
        return false;

    // Explicit on every paragraph with a strong character, not only the ones
    // that differ from the document's: QQuickTextEdit's own choice is not ours
    // (it reads an emoji as left to right). Paragraphs with none (digits,
    // emoji) keep the document's.
    // One edit block: one relayout and one contentsChanged for all of them.
    QTextCursor cursor(document);
    bool editing = false;
    for (QTextBlock b = document->begin(); b.isValid(); b = b.next()) {
        const Qt::LayoutDirection wanted =
            leftToRight ? Qt::LeftToRight : paragraphDirection(b.text());
        if (wanted == Qt::LayoutDirectionAuto
            || b.blockFormat().layoutDirection() == wanted) {
            continue;
        }
        if (!editing) {
            cursor.beginEditBlock();
            editing = true;
        }
        QTextBlockFormat format;
        format.setLayoutDirection(wanted);
        cursor.setPosition(b.position());
        cursor.mergeBlockFormat(format);
    }
    if (editing)
        cursor.endEditBlock();
    return rightToLeft && !leftToRight;
}

void ParagraphDirection::update()
{
    // The format changes below report contentsChanged themselves.
    if (m_updating)
        return;
    m_updating = true;
    const bool rightToLeft = apply(m_textDocument, m_leftToRight);
    m_updating = false;
    if (rightToLeft != m_rightToLeft) {
        m_rightToLeft = rightToLeft;
        Q_EMIT rightToLeftChanged();
    }
}
