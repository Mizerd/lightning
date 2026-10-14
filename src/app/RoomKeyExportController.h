#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class MatrixClient;

// Settings → Privacy & security → Export room keys, the counterpart of the
// import beside it.
//
// The export itself is matrix-sdk's own Encryption::export_room_keys (see
// rust/src/keyexport.rs): the standard encrypted key-export format Element
// and other clients import. This class is policy only: it checks the two
// passphrase fields and the destination, starts the export, follows its
// progress and reports the outcome in words.
//
// The passphrase enters C++ only as a parameter of exportKeys() (and of the
// stateless strength() hint): it is checked, handed to the backend and
// dropped. No member, property, signal or log line ever holds it, nor the
// destination path. Sign-out and account switches drop the pending export,
// so a late answer never lands on the next account.
class RoomKeyExportController : public QObject
{
    Q_OBJECT
    // The backend can export at all (Rust backend only).
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // Sessions the SDK has gathered so far while busy (0 = not known yet).
    Q_PROPERTY(int collected READ collected NOTIFY stateChanged)
    // Outcome of the last attempt, shown in place.
    Q_PROPERTY(QString resultMessage READ resultMessage NOTIFY stateChanged)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY stateChanged)
    Q_PROPERTY(int minimumLength READ minimumLength CONSTANT)
    // The strength() rung an export needs (Element's zxcvbn score 3).
    Q_PROPERTY(int requiredStrength READ requiredStrength CONSTANT)
    Q_PROPERTY(QString suggestedFileName READ suggestedFileName CONSTANT)

public:
    // The file can be attacked offline for as long as the attacker likes;
    // below this the 500,000 PBKDF2 rounds do not buy much.
    static constexpr int kMinimumLength = 8;
    // Below this strength() rung the export is refused, as Element refuses
    // below zxcvbn score 3 (see strengthOf for the mapping).
    static constexpr int kRequiredStrength = 3;
    // Fewer distinct characters than this is weak at any length.
    static constexpr int kMinimumDistinct = 5;

    explicit RoomKeyExportController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const;
    bool busy() const { return m_op != 0; }
    int collected() const { return m_collected; }
    QString resultMessage() const { return m_resultMessage; }
    bool resultOk() const { return m_resultOk; }
    int minimumLength() const { return kMinimumLength; }
    int requiredStrength() const { return kRequiredStrength; }
    QString suggestedFileName() const;

    // Checks the fields and the destination, then starts the export.
    // Returns true when it was started; false with the reason in
    // resultMessage when refused locally. The QML fields must wipe
    // themselves right after calling this.
    Q_INVOKABLE bool exportKeys(const QUrl &fileUrl,
                                const QString &passphrase,
                                const QString &confirmation);
    Q_INVOKABLE void clearResult();
    // 0 empty, 1 too short, 2 weak, 3 fair, 4 strong. Below
    // requiredStrength the export is refused.
    Q_INVOKABLE int strength(const QString &passphrase) const
    {
        return strengthOf(passphrase);
    }

    // "" when the fields may be sent, else "empty", "mismatch",
    // "too_short" or "too_weak".
    static QString localProblem(const QString &passphrase,
                                const QString &confirmation);
    static int strengthOf(const QString &passphrase);
    // The sentence shown for a category ("" is success, with `exported`).
    static QString describe(const QString &category, int exported);

Q_SIGNALS:
    void stateChanged();
    // Terminal outcome of an export that was started.
    void finished(bool ok, int exported);

private Q_SLOTS:
    void onProgress(quint64 opId, int collected);
    void onFinished(quint64 opId, bool ok, int exported,
                    const QString &category);
    void onSessionChanged();

private:
    void setResult(bool ok, const QString &message);

    MatrixClient *m_client = nullptr;
    quint64 m_op = 0;
    int m_collected = 0;
    QString m_resultMessage;
    bool m_resultOk = false;
};
