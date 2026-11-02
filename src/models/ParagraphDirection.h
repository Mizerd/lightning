#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QStringView>
#include <qqmlintegration.h>

class QTextDocument;

// Gives every paragraph of a TextEdit its own direction.
//
// QQuickTextEdit takes ONE direction for the whole document, from the first
// character that has one, so an Arabic paragraph after an English one was laid
// out left to right and left-aligned (and an English one after Arabic, right
// to left). Element sets dir="auto"; this is the per-paragraph equivalent: in a
// document with any right-to-left paragraph, each paragraph gets the direction
// of its own first strong character (Unicode rule P2), and the TextEdit's
// implicit alignment follows it. Documents with no right-to-left paragraph are
// left untouched.
//
// `leftToRight` forces every paragraph left to right instead: code reads left
// to right in every language, and a block whose first line is an Arabic
// comment was laid out right to left, every line.
class ParagraphDirection : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument
                   NOTIFY documentChanged)
    Q_PROPERTY(bool leftToRight READ leftToRight WRITE setLeftToRight
                   NOTIFY leftToRightChanged)
    Q_PROPERTY(bool rightToLeft READ rightToLeft NOTIFY rightToLeftChanged)

public:
    explicit ParagraphDirection(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_quickDocument; }
    void setDocument(QQuickTextDocument *document);
    bool leftToRight() const { return m_leftToRight; }
    void setLeftToRight(bool value);
    bool rightToLeft() const { return m_rightToLeft; }

    static Qt::LayoutDirection paragraphDirection(QStringView text);
    static bool apply(QTextDocument *document, bool leftToRight);

Q_SIGNALS:
    void documentChanged();
    void leftToRightChanged();
    void rightToLeftChanged();

private:
    void update();

    QPointer<QQuickTextDocument> m_quickDocument;
    QPointer<QTextDocument> m_textDocument;
    QMetaObject::Connection m_contentsConnection;
    bool m_leftToRight = false;
    bool m_rightToLeft = false;
    bool m_updating = false;
};
