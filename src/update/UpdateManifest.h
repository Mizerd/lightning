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

// Lightning secure update system — the signed release manifest (spec §4).
//
// SIGNATURE FIRST, ALWAYS. The only way to obtain an UpdateManifest is
// parseVerified(), which verifies the detached signature over the RAW
// manifest bytes before a single field is parsed as trusted. There is no
// public "parse these bytes" entry point, so the wrong order is not
// expressible.
//
// The manifest describes WHAT to fetch. It can never describe HOW to
// install: there is no command, argv, script, or interpreter concept in
// this type, and unknown JSON fields (including anything named "command" or
// "install_command") are ignored on parse and reach no execution path.
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
    // `expires` is REQUIRED and must be an ISO-8601 instant. A signature
    // proves who produced the document, never that it is the current one;
    // the expiry is what bounds how long a captured `latest` pair can be
    // replayed to freeze installations on a vulnerable version.
    MissingExpiry,
    MalformedExpiry,
    ReleaseNotesUrlRejected,
    // Artifact stage.
    ArtifactMalformed,
    ArtifactBadFilename,
    ArtifactBadHash,
    ArtifactBadSize,
    ArtifactBadUrl,
    ArtifactForeignHost,
    ArtifactFilenameMismatch,
    // The optional mirror address is MALFORMED -- not a string, not https,
    // unparseable, or naming a different file than the entry describes. Only
    // those are fatal. A well-formed mirror on a host this build does not
    // trust is NOT an error: it is dropped, counted in untrustedMirrorCount(),
    // and the artifact keeps its canonical address. Do not "tighten" that back
    // into a failure -- the day the mirror moves, every client built before
    // the move would reject every later manifest, canonical address included,
    // and could never be updated again.
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
    // OPTIONAL bandwidth mirror for the SAME bytes, chosen by the release
    // authority inside the signed document — never discovered at runtime.
    // Empty means this release publishes no mirror for this artifact, and
    // the download behaves exactly as it did before mirrors existed. It is
    // tried FIRST when present, and both sources are verified against the
    // one `sha256` above.
    QUrl mirrorUrl;
};

struct ManifestChannel {
    QString id;
    bool available = false;
    std::optional<Version> version;
    // Presentation text from the signed document, bounded at parse time
    // (kMaxChannelNoteChars) and rendered as plain text only.
    QString note;
};

// Declared before the class and DEFINED after it: it carries a manifest by
// value, which a nested struct could not do while the enclosing class is
// still incomplete.
struct ManifestParseResult;

class UpdateManifest
{
public:
    using Result = ManifestParseResult;

    // Bounds from the spec. The caller must also bound the transport.
    static constexpr qint64 kMaxManifestBytes = 1024 * 1024;
    static constexpr int kSupportedSchema = 1;
    // The updater helper contract version compiled into this build.
    static constexpr int kUpdaterVersion = 1;
    // Client-side sanity ceiling on an artifact size. This is OUR limit,
    // not a server-advertised one, and it is described that way in the UI.
    static constexpr qint64 kMaxArtifactBytes = qint64(1024) * 1024 * 1024;
    // A channel note is one or two sentences for a status line.
    static constexpr int kMaxChannelNoteChars = 512;

    // Verify `sigBytes` over `manifestBytes`, then parse. Nothing from the
    // manifest is used before the signature verifies.
    static Result parseVerified(const QByteArray &manifestBytes, const QByteArray &sigBytes,
                                const TrustStore &trust);

    bool isValid() const { return m_valid; }

    int schema() const { return m_schema; }
    const Version &version() const { return m_version; }
    QString versionString() const { return m_versionString; }
    QString channel() const { return m_channel; }
    QString tag() const { return m_tag; }
    QDateTime released() const { return m_released; }
    // The signed instant after which this document must not be believed.
    // Always valid on a parsed manifest; UpdateManager compares it with the
    // clock, because the parser has none.
    QDateTime expires() const { return m_expires; }
    int minUpdaterVersion() const { return m_minUpdaterVersion; }
    QString releaseNotes() const { return m_releaseNotes; }
    QUrl releaseNotesUrl() const { return m_releaseNotesUrl; }
    // > 0 when the manifest offered a mirror on a host this build does not
    // trust. Downloads still work: those artifacts simply use the canonical
    // source, exactly as they did before mirrors existed.
    int untrustedMirrorCount() const { return m_untrustedMirrorCount; }
    QString signingKeyId() const { return m_signingKeyId; }

    // Direct-download artifact for an install type, when the release
    // publishes one. Absent = no direct download for that install type.
    std::optional<ManifestArtifact> artifactFor(InstallType type) const;
    std::optional<ManifestArtifact> artifactForId(const QString &installTypeId) const;
    QStringList artifactIds() const;

    // Ecosystem-managed installs (flatpak/snap/apt/dnf).
    std::optional<ManifestChannel> channelFor(const QString &channelId) const;

    // True only when the named ecosystem channel says it is available AND
    // carries a version strictly newer than `installed`. A Flatpak/Snap user
    // is never told an update is actionable on any weaker evidence.
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
    int m_minUpdaterVersion = 1;
    QString m_releaseNotes;
    QUrl m_releaseNotesUrl;
    // Entries that named a mirror host this build does not trust. Those
    // mirrors were ignored and the artifacts kept their canonical address;
    // this is a diagnostic, never a failure.
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
