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

// The signed release manifest (spec §4).
//
// Signature first: the only way to obtain an UpdateManifest is
// parseVerified(), which verifies the detached signature over the raw bytes
// before any field is trusted. No other parse entry point exists.
//
// The manifest says what to fetch, never how to install: there is no command
// or script concept, and unknown fields (e.g. "install_command") are ignored.
namespace lightning::update {

enum class ManifestError {
    None,
    // Signature stage.
    SignatureMissing,
    SignatureMalformed,
    SignatureUnsupportedAlgorithm,
    SignatureUnknownKey,
    SignatureInvalid,
    // Document stage.
    ManifestTooLarge,
    ManifestNotJson,
    ManifestNotObject,
    UnsupportedSchema,
    UnsupportedUpdaterVersion,
    MissingVersion,
    MalformedVersion,
    MissingChannel,
    ReleaseNotesUrlRejected,
    // Artifact stage.
    ArtifactMalformed,
    ArtifactBadFilename,
    ArtifactBadHash,
    ArtifactBadSize,
    ArtifactBadUrl,
    ArtifactForeignHost,
    ArtifactFilenameMismatch,
    // The mirror address is malformed (not an https string, unparseable, or a
    // different file). A well-formed mirror on an untrusted host is not an
    // error: it is dropped and counted in untrustedMirrorCount(). Keep it that
    // way, or moving the mirror would make older clients reject every manifest.
    ArtifactBadMirrorUrl,
    // Channels stage.
    ChannelMalformed,
};

QString manifestErrorText(ManifestError error);

struct ManifestArtifact {
    QString installTypeId;
    QString filename;
    qint64 size = 0;
    QString sha256; // 64 lowercase hex characters
    // The canonical (release-authority) address. Always present.
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
    // Bounded at parse time (kMaxChannelNoteChars); rendered as plain text.
    QString note;
};

// Defined after the class because it holds a manifest by value.
struct ManifestParseResult;

class UpdateManifest
{
public:
    using Result = ManifestParseResult;

    // Spec bounds; the caller must also bound the transport.
    static constexpr qint64 kMaxManifestBytes = 1024 * 1024;
    static constexpr int kSupportedSchema = 1;
    // The updater helper contract version compiled into this build.
    static constexpr int kUpdaterVersion = 1;
    // Client-side sanity ceiling on artifact size (our limit, not the
    // server's).
    static constexpr qint64 kMaxArtifactBytes = qint64(1024) * 1024 * 1024;
    // A channel note is one or two sentences for a status line.
    static constexpr int kMaxChannelNoteChars = 512;

    // Verifies `sigBytes` over `manifestBytes`, then parses.
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
    // An `expires` was present but unreadable. UpdateManager treats that as
    // stale so a generator defect cannot hide the freshness signal.
    bool expiresMalformed() const { return m_expiresMalformed; }
    int minUpdaterVersion() const { return m_minUpdaterVersion; }
    QString releaseNotes() const { return m_releaseNotes; }
    QUrl releaseNotesUrl() const { return m_releaseNotesUrl; }
    // Mirrors offered on untrusted hosts; those artifacts use the canonical
    // address.
    int untrustedMirrorCount() const { return m_untrustedMirrorCount; }
    QString signingKeyId() const { return m_signingKeyId; }

    // Direct-download artifact for an install type, if the release publishes
    // one.
    std::optional<ManifestArtifact> artifactFor(InstallType type) const;
    std::optional<ManifestArtifact> artifactForId(const QString &installTypeId) const;
    QStringList artifactIds() const;

    // Ecosystem-managed installs (flatpak/snap/apt/dnf).
    std::optional<ManifestChannel> channelFor(const QString &channelId) const;

    // True only when the channel says available and names a strictly newer
    // version.
    bool channelOffersUpdate(const QString &channelId, const Version &installed) const;

    // Canonical channel ids for the ecosystems Lightning reports on.
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
    // Diagnostic only; see untrustedMirrorCount().
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

// Exposed for tests and for the downloader's own re-validation.
bool isValidSha256Hex(const QString &value);
bool isSafeArtifactFilename(const QString &filename);

} // namespace lightning::update
