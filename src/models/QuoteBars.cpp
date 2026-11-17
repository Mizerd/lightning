#include "models/QuoteBars.h"

#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

QuoteBars::QuoteBars(QObject *parent)
    : QObject(parent)
{
}

void QuoteBars::setDocument(QQuickTextDocument *document)
{
    if (m_quickDocument == document)
        return;
    if (m_textDocument) {
        disconnect(m_textDocument, nullptr, this, nullptr);
        if (auto *layout = m_textDocument->documentLayout())
            disconnect(layout, nullptr, this, nullptr);
    }
    m_quickDocument = document;
    m_textDocument = document ? document->textDocument() : nullptr;
    if (m_textDocument) {
        connect(m_textDocument, &QTextDocument::contentsChanged, this,
                &QuoteBars::update);
        connect(m_textDocument->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged, this,
                &QuoteBars::update);
    }
    Q_EMIT documentChanged();
    update();
}

void QuoteBars::collect(QTextFrame *frame, QVariantList &out) const
{
    for (auto it = frame->begin(); !it.atEnd(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            const QTextTableFormat format = table->format();
            if (table->rows() == 1 && table->columns() == 1
                && format.border() == 0
                && format.borderBrush().style() == Qt::SolidPattern) {
                const QRectF r =
                    m_textDocument->documentLayout()->frameBoundingRect(table);
                out.append(QVariantMap{
                    {QStringLiteral("x"), r.x()},
                    {QStringLiteral("y"), r.y()},
                    {QStringLiteral("height"), r.height()},
                    {QStringLiteral("color"),
                     format.borderBrush().color()},
                });
            }
        }
        collect(child, out);
    }
}

void QuoteBars::update()
{
    QVariantList next;
    if (m_textDocument)
        collect(m_textDocument->rootFrame(), next);
    if (next != m_bars) {
        m_bars = next;
        Q_EMIT barsChanged();
    }
}
