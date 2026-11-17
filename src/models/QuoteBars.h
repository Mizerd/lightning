#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>
#include <qqmlintegration.h>

class QTextDocument;
class QTextFrame;

// Quick's text paints a table cell's background only behind its own text lines,
// so a bar drawn as a cell is one line tall. This reports the full-height
// rectangle of each quote table (MessageHtml's two-cell tables whose first cell
// carries a background) for QML to paint.
class QuoteBars : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument
                   NOTIFY documentChanged)
    Q_PROPERTY(QVariantList bars READ bars NOTIFY barsChanged)

public:
    explicit QuoteBars(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_quickDocument; }
    void setDocument(QQuickTextDocument *document);
    QVariantList bars() const { return m_bars; }

Q_SIGNALS:
    void documentChanged();
    void barsChanged();

private:
    void update();
    void collect(QTextFrame *frame, QVariantList &out) const;

    QPointer<QQuickTextDocument> m_quickDocument;
    QPointer<QTextDocument> m_textDocument;
    QVariantList m_bars;
};
