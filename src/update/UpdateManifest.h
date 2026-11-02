#pragma once

#include "update/InstallType.h"
#include "update/UpdateTrustStore.h"
#include "update/Version.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace lightning::update {

enum class ManifestError {
    None,
    SignatureMissing,
    SignatureMalformed,
    SignatureUnsupportedAlgorithm,
    SignatureUnknownKey,
    SignatureInvalid,
    ManifestTooLarge,
    ManifestNotJson,
    ManifestNotObject,
    UnsupportedSchema,
    UnsupportedUpdaterVersion,
    MissingVersion,
    MalformedVersion,
    MissingChannel,
    ReleaseNotesUrlRejected,
    ArtifactMalformed,
    ArtifactBadFilename,
    ArtifactBadHash,
    ArtifactBadSize,
    ArtifactBadUrl,
    ArtifactForeignHost,
    ArtifactFilenameMismatch,
    ArtifactBadMirrorUrl,
    ChannelMalformed,
};

QString manifestErrorText(ManifestError error);

struct ManifestArtifact {
    QString installTypeId;
    QString filename;
    qint64 size = 0;
    QString sha256;
    QUrl url;
    // Optional mirror for the same bytes, chosen in the signed document, never
    // discovered at runtime. Tried first when present; both addresses verify
    // against the same `sha256`.
    QUrl mirrorUrl;
};

struct ManifestChannel {
    QString id;
    bool available = false;
    std::optional<Version> version;
    QString note;
};

struct ManifestParseResult;

class UpdateManifest
{
public:
    using Result = ManifestParseResult;

    static constexpr qint64 kMaxManifestBytes = 1024 * 1024;
    static constexpr int kSupportedSchema = 1;
    static constexpr int kUpdaterVersion = 1;
    static constexpr qint64 kMaxArtifactBytes = qint64(1024) * 1024 * 1024;
    static constexpr int kMaxChannelNoteChars = 512;

    static Result parseVerified(const QByteArray &manifestBytes, const QByteArray &sigBytes,
                                const TrustStore &trust);

    bool isValid() const { return m_valid; }

    int schema() const { return m_schema; }
    const Version &version() const { return m_version; }
    QString versionString() const { return m_versionString; }
    QString channel() const { return m_channel; }
    QString tag() const { return m_tag; }
    QDateTime released() const { return m_released; }
    // When the release authority expected to refresh this document.
    // Informational only: installed clients must keep updating from the mirror
    // even if the project's servers disappear, so the manifest is never refused
    // over it. Null means none (or unparseable). UpdateManager compares.
    QDateTime expires() const { return m_expires; }
    bool expiresMalformed() const { return m_expiresMalformed; }
    int minUpdaterVersion() const { return m_minUpdaterVersion; }
    QString releaseNotes() const { return m_releaseNotes; }
    QUrl releaseNotesUrl() const { return m_releaseNotesUrl; }
    int untrustedMirrorCount() const { return m_untrustedMirrorCount; }
    QString signingKeyId() const { return m_signingKeyId; }

    std::optional<ManifestArtifact> artifactFor(InstallType type) const;
    std::optional<ManifestArtifact> artifactForId(const QString &installTypeId) const;
    QStringList artifactIds() const;

    std::optional<ManifestChannel> channelFor(const QString &channelId) const;

    bool channelOffersUpdate(const QString &channelId, const Version &installed) const;

    static QString channelIdForInstallType(InstallType type);

private:
    bool m_valid = false;
    int m_schema = 0;
    Version m_version;
    QString m_versionString;
    QString m_channel;
    QString m_tag;
    QDateTime m_released;
    QDateTime m_expires;
    bool m_expiresMalformed = false;
    int m_minUpdaterVersion = 1;
    QString m_releaseNotes;
    QUrl m_releaseNotesUrl;
    int m_untrustedMirrorCount = 0;
    QString m_signingKeyId;
    QHash<QString, ManifestArtifact> m_artifacts;
    QHash<QString, ManifestChannel> m_channels;
};

struct ManifestParseResult {
    bool ok = false;
    ManifestError error = ManifestError::None;
    QString message;
    UpdateManifest manifest;
};

bool isValidSha256Hex(const QString &value);
bool isSafeArtifactFilename(const QString &filename);

}
