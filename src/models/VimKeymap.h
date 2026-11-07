#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// Vim-style modal editing for the composer, as a pure keymap over plain text,
// a caret and one register. The editor applies each Result itself; the engine
// never touches a document, so the same keymap drives the markdown and the
// rich editor and is testable without QML.
class VimKeymap : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool normal READ normal NOTIFY modeChanged)
    Q_PROPERTY(QString pending READ pending NOTIFY pendingChanged)

public:
    struct Result {
        bool accepted = false;
        // -1 leaves the caret where the editor put it.
        int cursor = -1;
        bool hasEdit = false;
        int editStart = 0;
        int editEnd = 0;
        QString editText;
        int undo = 0;
        int redo = 0;
        bool send = false;
        // After undo/redo the editor reports its own caret and asks
        // clampNormal() for the Normal-mode position.
        bool clampAfter = false;
    };

    explicit VimKeymap(QObject *parent = nullptr);

    bool normal() const { return m_normal; }
    QString pending() const;

    // The register is mirrored to the system clipboard through QClipboard,
    // and a clipboard that changed elsewhere is what p/P paste.
    void setUseSystemClipboard(bool use) { m_useClipboard = use; }
    QString registerText() const;

    Result handle(int key, Qt::KeyboardModifiers modifiers, const QString &text,
                  const QString &buffer, int cursor);

    Q_INVOKABLE QVariantMap handleKey(int key, int modifiers, const QString &text,
                                      const QString &buffer, int cursor);
    // Insert -> Normal. Returns the caret, one left as Vim does on Esc.
    Q_INVOKABLE int enterNormal(const QString &buffer, int cursor);
    Q_INVOKABLE void enterInsert();
    Q_INVOKABLE int clampNormal(const QString &buffer, int cursor) const;
    // Text for TextEdit.insert() on a RichText editor: escaped, spaces kept,
    // newlines as line breaks.
    Q_INVOKABLE static QString richInsertHtml(const QString &text);

    static QVariantMap toVariant(const Result &result);

Q_SIGNALS:
    void modeChanged();
    void pendingChanged();

private:
    enum class Op { None, Delete, Change, Yank };
    struct Range {
        bool valid = false;
        int start = 0;
        int end = 0;
        bool linewise = false;
        // Where the caret goes for a yank (Vim moves it to the range start).
        int anchor = -1;
    };

    Result command(QChar c, const QString &buf, int pos);
    Result applyOperator(Op op, const Range &range, const QString &buf, int pos);
    Range motionRange(QChar motion, int count, bool hasCount, const QString &buf,
                      int pos, Op op) const;
    int motionTarget(QChar motion, int count, bool hasCount, const QString &buf,
                     int pos);
    Result paste(bool after, int count, const QString &buf, int pos);
    void setRegister(const QString &text);
    QString readRegister() const;
    void setNormal(bool normal);
    void clearPending();
    int takeCount();

    bool m_normal = false;
    int m_count = 0;
    int m_opCount = 0;
    Op m_op = Op::None;
    bool m_gPending = false;
    int m_wantColumn = -1;
    QString m_register;
    bool m_useClipboard = true;
};
