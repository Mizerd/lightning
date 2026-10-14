#include "app/RoomKeyExportController.h"

#include "matrix/MatrixClient.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QSet>

#include <algorithm>

// Counts and categories only; never the passphrase or the destination path.
Q_LOGGING_CATEGORY(lcKeyExport, "lightning.e2ee.keyexport")

RoomKeyExportController::RoomKeyExportController(QObject *parent)
    : QObject(parent)
{
}

void RoomKeyExportController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    onSessionChanged(); // nothing of the previous client survives
    if (m_client) {
        connect(m_client, &MatrixClient::roomKeyExportProgress, this,
                &RoomKeyExportController::onProgress);
        connect(m_client, &MatrixClient::roomKeyExportFinished, this,
                &RoomKeyExportController::onFinished);
        connect(m_client, &MatrixClient::loggedOut, this,
                &RoomKeyExportController::onSessionChanged);
        connect(m_client, &MatrixClient::loginSucceeded, this,
                &RoomKeyExportController::onSessionChanged);
    }
}

bool RoomKeyExportController::available() const
{
    return m_client && m_client->supportsRoomKeyExport();
}

QString RoomKeyExportController::suggestedFileName() const
{
    // Element's own default is element-keys.txt; any name imports.
    return QStringLiteral("lightning-keys.txt");
}

bool RoomKeyExportController::exportKeys(const QUrl &fileUrl,
                                         const QString &passphrase,
                                         const QString &confirmation)
{
    if (!available()) {
        setResult(false, describe(QStringLiteral("unavailable"), 0));
        return false;
    }
    if (busy())
        return false;
    // Refused here, before anything reaches the backend.
    const QString problem = localProblem(passphrase, confirmation);
    if (!problem.isEmpty()) {
        setResult(false, describe(problem, 0));
        return false;
    }
    if (!fileUrl.isValid() || !fileUrl.isLocalFile()) {
        setResult(false, describe(QStringLiteral("invalid_path"), 0));
        return false;
    }
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty() || QFileInfo(path).isDir()) {
        setResult(false, describe(QStringLiteral("invalid_path"), 0));
        return false;
    }
    const quint64 opId = m_client->exportRoomKeys(path, passphrase);
    qCInfo(lcKeyExport) << "export sent=" << (opId != 0);
    if (opId == 0) {
        setResult(false, describe(QStringLiteral("not_signed_in"), 0));
        return false;
    }
    m_op = opId;
    m_collected = 0;
    m_resultMessage.clear();
    m_resultOk = false;
    Q_EMIT stateChanged();
    return true;
}

void RoomKeyExportController::clearResult()
{
    if (m_resultMessage.isEmpty() && !m_resultOk)
        return;
    m_resultMessage.clear();
    m_resultOk = false;
    Q_EMIT stateChanged();
}

namespace {
// Characters, not UTF-16 units, and without copying the passphrase.
qsizetype characterCount(const QString &text)
{
    return std::count_if(text.cbegin(), text.cend(),
                         [](QChar c) { return !c.isLowSurrogate(); });
}
} // namespace

namespace {
// Things a guesser tries first. Matched case-insensitively as substrings and
// each counted as nothing: "password123" is two common pieces, not 11
// characters of entropy.
const QStringList &commonWords()
{
    static const QStringList words{
        QStringLiteral("password"), QStringLiteral("passw0rd"),
        QStringLiteral("qwerty"),   QStringLiteral("qwertz"),
        QStringLiteral("azerty"),   QStringLiteral("asdfgh"),
        QStringLiteral("zxcvbn"),   QStringLiteral("letmein"),
        QStringLiteral("admin"),    QStringLiteral("welcome"),
        QStringLiteral("iloveyou"), QStringLiteral("matrix"),
        QStringLiteral("lightning"), QStringLiteral("element"),
        QStringLiteral("synapse"),  QStringLiteral("monkey"),
        QStringLiteral("dragon"),   QStringLiteral("football"),
        QStringLiteral("baseball"), QStringLiteral("sunshine"),
        QStringLiteral("princess"), QStringLiteral("changeme"),
        QStringLiteral("trustno1"), QStringLiteral("abc123"),
        QStringLiteral("123456"),
    };
    return words;
}

// The part of a passphrase that actually carries information: a passphrase
// that is a block repeated counts the block once, a common word counts for
// nothing, and a run of three or more consecutive letters or digits
// ("abc", "4321") counts for nothing. Deterministic and dictionary-light on
// purpose; it is a floor, not zxcvbn.
QString informativePart(const QString &passphrase)
{
    QString text = passphrase;
    const QString lower = text.toLower();
    const qsizetype n = lower.size();
    for (qsizetype p = 1; p <= n / 2; ++p) {
        bool periodic = true;
        for (qsizetype i = p; i < n && periodic; ++i)
            periodic = lower[i] == lower[i - p];
        if (periodic) {
            text = passphrase.left(p);
            break;
        }
    }
    QString low = text.toLower();
    if (low.size() != text.size())
        low = text; // case mapping changed the length; stay index-aligned
    QString kept;
    for (qsizetype i = 0; i < low.size();) {
        bool skipped = false;
        for (const QString &w : commonWords()) {
            if (low.mid(i, w.size()) == w) {
                i += w.size();
                skipped = true;
                break;
            }
        }
        if (skipped)
            continue;
        qsizetype run = 1;
        int step = 0;
        if (low[i].isLetterOrNumber() && i + 1 < low.size()) {
            const int d = int(low[i + 1].unicode()) - int(low[i].unicode());
            if ((d == 1 || d == -1)
                && low[i].isDigit() == low[i + 1].isDigit()
                && low[i].isLetter() == low[i + 1].isLetter()) {
                step = d;
                run = 2;
                while (i + run < low.size()
                       && int(low[i + run].unicode())
                              - int(low[i + run - 1].unicode()) == step
                       && low[i + run].isLetterOrNumber())
                    ++run;
            }
        }
        if (run >= 3) {
            i += run;
            continue;
        }
        kept.append(text[i]);
        ++i;
    }
    return kept;
}
} // namespace

QString RoomKeyExportController::localProblem(const QString &passphrase,
                                              const QString &confirmation)
{
    if (passphrase.isEmpty())
        return QStringLiteral("empty");
    if (passphrase != confirmation)
        return QStringLiteral("mismatch");
    if (characterCount(passphrase) < kMinimumLength)
        return QStringLiteral("too_short");
    // As Element: a weak passphrase blocks the export, it does not just warn.
    if (strengthOf(passphrase) < kRequiredStrength)
        return QStringLiteral("too_weak");
    return {};
}

int RoomKeyExportController::strengthOf(const QString &passphrase)
{
    // The change-password hint's scale (ChangePasswordSection): length and
    // character variety, 0 empty, 1 too short, 2 weak, 3 fair, 4 strong.
    // Element blocks a key export below zxcvbn score 3 ("safely
    // unguessable"); zxcvbn is not in this tree, so kRequiredStrength puts
    // that bar at rung 3 of this scale: 12 characters, or 10 mixing three of
    // lower case, upper case, digits and symbols. One guard zxcvbn has and
    // length alone does not: a passphrase of a few characters repeated
    // ("abababababab") is weak however long. What it cannot see is a
    // dictionary phrase or a keyboard run, which zxcvbn would mark down;
    // the hint says the bar is a minimum, not a guarantee.
    if (passphrase.isEmpty())
        return 0;
    if (characterCount(passphrase) < kMinimumLength)
        return 1;
    // Strength is judged on what is left once repetition, common words and
    // sequences are taken out ("password123password123" leaves nothing).
    const QString effective = informativePart(passphrase);
    const qsizetype chars = characterCount(effective);
    if (chars < kMinimumLength)
        return 2;
    bool lower = false, upper = false, digit = false, other = false;
    for (QChar c : effective) {
        const ushort u = c.unicode();
        if (u >= 'a' && u <= 'z')
            lower = true;
        else if (u >= 'A' && u <= 'Z')
            upper = true;
        else if (u >= '0' && u <= '9')
            digit = true;
        else
            other = true;
    }
    const int kinds = int(lower) + int(upper) + int(digit) + int(other);
    QSet<QChar> distinct;
    for (QChar c : effective) {
        distinct.insert(c);
        if (distinct.size() >= kMinimumDistinct)
            break;
    }
    if (distinct.size() < kMinimumDistinct)
        return 2;
    if (chars >= 16 || (chars >= 12 && kinds >= 3))
        return 4;
    if (chars >= 12 || (chars >= 10 && kinds >= 3))
        return 3;
    return 2;
}

QString RoomKeyExportController::describe(const QString &category,
                                          int exported)
{
    if (category.isEmpty()) {
        if (exported == 0)
            return tr("Export finished, but this session holds no room keys "
                      "yet, so the file contains none.");
        return tr("Exported %n room key(s). Keep the file and its passphrase "
                  "safe, and apart from each other.", nullptr, exported);
    }
    if (category == QLatin1String("empty"))
        return tr("Enter a passphrase to protect the file.");
    if (category == QLatin1String("mismatch"))
        return tr("The passphrases do not match.");
    if (category == QLatin1String("too_short"))
        return tr("Use at least %1 characters.").arg(kMinimumLength);
    if (category == QLatin1String("too_weak"))
        return tr("This passphrase is too weak to protect your keys. Use at "
                  "least 12 characters, or 10 that mix upper and lower case, "
                  "digits or symbols.");
    if (category == QLatin1String("invalid_path"))
        return tr("Lightning cannot save to that location. Choose another "
                  "folder or file name.");
    if (category == QLatin1String("permission_denied"))
        return tr("Lightning is not allowed to write to that location. "
                  "Choose another folder.");
    if (category == QLatin1String("disk_full"))
        return tr("There is not enough space to save the file. Nothing was "
                  "saved. Free some space or choose another location.");
    if (category == QLatin1String("write_failed"))
        return tr("Writing the file failed. Nothing was saved. Try again or "
                  "choose another location.");
    if (category == QLatin1String("partial_file"))
        return tr("Writing the file failed part-way. The file at that "
                  "location may be incomplete: delete it and export again.");
    if (category == QLatin1String("already_running"))
        return tr("An export is already running.");
    if (category == QLatin1String("not_signed_in"))
        return tr("Sign in to export room keys.");
    if (category == QLatin1String("no_encryption"))
        return tr("Encryption is not ready on this session yet. Try again in "
                  "a moment.");
    if (category == QLatin1String("unavailable"))
        return tr("Exporting room keys needs the Matrix SDK backend.");
    return tr("The export failed. Nothing was saved.");
}

void RoomKeyExportController::onProgress(quint64 opId, int collected)
{
    if (opId == 0 || opId != m_op || collected <= m_collected)
        return;
    m_collected = collected;
    Q_EMIT stateChanged();
}

void RoomKeyExportController::onFinished(quint64 opId, bool ok, int exported,
                                         const QString &category)
{
    if (opId == 0 || opId != m_op)
        return;
    m_op = 0;
    m_collected = 0;
    // A success the backend reports with a category is not a success.
    const bool success = ok && category.isEmpty();
    qCInfo(lcKeyExport) << "export finished ok=" << success
                        << "exported=" << (success ? exported : 0)
                        << "category=" << category;
    setResult(success,
              describe(success ? QString()
                               : (category.isEmpty() ? QStringLiteral("failed")
                                                     : category),
                       std::max(0, exported)));
    Q_EMIT finished(success, success ? exported : 0);
}

void RoomKeyExportController::onSessionChanged()
{
    const bool changed = m_op != 0 || m_collected != 0
                         || !m_resultMessage.isEmpty() || m_resultOk;
    m_op = 0;
    m_collected = 0;
    m_resultMessage.clear();
    m_resultOk = false;
    if (changed)
        Q_EMIT stateChanged();
}

void RoomKeyExportController::setResult(bool ok, const QString &message)
{
    m_resultOk = ok;
    m_resultMessage = message;
    Q_EMIT stateChanged();
}
