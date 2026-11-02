#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class MatrixClient;

class RoomKeyExportController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(int collected READ collected NOTIFY stateChanged)
    Q_PROPERTY(QString resultMessage READ resultMessage NOTIFY stateChanged)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY stateChanged)
    Q_PROPERTY(int minimumLength READ minimumLength CONSTANT)
    Q_PROPERTY(int requiredStrength READ requiredStrength CONSTANT)
    Q_PROPERTY(QString suggestedFileName READ suggestedFileName CONSTANT)

public:
    // The file can be attacked offline for as long as the attacker likes;
    // below this the 500,000 PBKDF2 rounds do not buy much.
    static constexpr int kMinimumLength = 8;
    static constexpr int kRequiredStrength = 3;
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

    Q_INVOKABLE bool exportKeys(const QUrl &fileUrl,
                                const QString &passphrase,
                                const QString &confirmation);
    Q_INVOKABLE void clearResult();
    Q_INVOKABLE int strength(const QString &passphrase) const
    {
        return strengthOf(passphrase);
    }

    static QString localProblem(const QString &passphrase,
                                const QString &confirmation);
    static int strengthOf(const QString &passphrase);
    static QString describe(const QString &category, int exported);

Q_SIGNALS:
    void stateChanged();
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
