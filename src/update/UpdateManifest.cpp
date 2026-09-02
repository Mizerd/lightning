#include "update/UpdateManifest.h"

#include "update/SignatureVerifier.h"
#include "update/UpdateEndpoints.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>

namespace lightning::update {
namespace {

ManifestError manifestErrorForSignature(SignatureError error)
{
    switch (error) {
    case SignatureError::None:
        return ManifestError::None;
    case SignatureError::EnvelopeTooLarge:
    case SignatureError::EnvelopeNotJson:
    case SignatureError::EnvelopeMalformed:
    case SignatureError::MalformedSignature:
    case SignatureError::MalformedKey:
        return ManifestError::SignatureMalformed;
    case SignatureError::UnsupportedAlgorithm:
        return ManifestError::SignatureUnsupportedAlgorithm;
    case SignatureError::UnknownKeyId:
        return ManifestError::SignatureUnknownKey;
    case SignatureError::EmptyPayload:
        return ManifestError::ManifestNotJson;
    case SignatureError::VerificationFailed:
    case SignatureError::CryptoUnavailable:
        return ManifestError::SignatureInvalid;
    }
    return ManifestError::SignatureInvalid;
}

UpdateManifest::Result failure(ManifestError error, const QString &detail = QString())
{
    UpdateManifest::Result result;
    result.ok = false;
    result.error = error;
    result.message = detail.isEmpty() ? manifestErrorText(error)
                                      : manifestErrorText(error) + QStringLiteral(": ") + detail;
    return result;
}

// A directly downloadable install type; the manifest may only carry
// artifacts for these.
bool isDownloadableInstallType(InstallType type)
{
    return canInstallAutomatically(type);
}

} // namespace

QString manifestErrorText(ManifestError error)
{
    switch (error) {
    case ManifestError::None:
        return QStringLiteral("No error");
    case ManifestError::SignatureMissing:
        return QStringLiteral("The update information was not signed");
    case ManifestError::SignatureMalformed:
        return QStringLiteral("The update signature is malformed");
    case ManifestError::SignatureUnsupportedAlgorithm:
        return QStringLiteral("The update signature uses an unsupported algorithm");
    case ManifestError::SignatureUnknownKey:
        return QStringLiteral("The update was signed with a key this build does not trust");
    case ManifestError::SignatureInvalid:
        return QStringLiteral("The update signature is not valid for this update information");
    case ManifestError::ManifestTooLarge:
        return QStringLiteral("The update information is larger than the allowed limit");
    case ManifestError::ManifestNotJson:
        return QStringLiteral("The update information is not valid JSON");
    case ManifestError::ManifestNotObject:
        return QStringLiteral("The update information is not a JSON object");
    case ManifestError::UnsupportedSchema:
        return QStringLiteral("This update information needs a newer Lightning; "
                              "please update manually");
    case ManifestError::UnsupportedUpdaterVersion:
        return QStringLiteral("This update needs a newer updater; please update manually");
    case ManifestError::MissingVersion:
        return QStringLiteral("The update information has no version");
    case ManifestError::MalformedVersion:
        return QStringLiteral("The update version is not a valid version number");
    case ManifestError::MissingChannel:
        return QStringLiteral("The update information has no release channel");
    case ManifestError::MissingExpiry:
        return QStringLiteral("The update information carries no expiry");
    case ManifestError::MalformedExpiry:
        return QStringLiteral("The update information has a malformed expiry");
    case ManifestError::ReleaseNotesUrlRejected:
        return QStringLiteral("The release notes link is not an accepted address");
    case ManifestError::ArtifactMalformed:
        return QStringLiteral("A download entry is malformed");
    case ManifestError::ArtifactBadFilename:
        return QStringLiteral("A download entry has an unsafe file name");
    case ManifestError::ArtifactBadHash:
        return QStringLiteral("A download entry has a malformed checksum");
    case ManifestError::ArtifactBadSize:
        return QStringLiteral("A download entry has an unacceptable size");
    case ManifestError::ArtifactBadUrl:
        return QStringLiteral("A download entry has an unacceptable address");
    case ManifestError::ArtifactForeignHost:
        return QStringLiteral("A download entry points at an untrusted host");
    case ManifestError::ArtifactFilenameMismatch:
        return QStringLiteral("A download entry's address does not match its file name");
    case ManifestError::ArtifactBadMirrorUrl:
        return QStringLiteral("A download entry has a malformed mirror address");
    case ManifestError::ChannelMalformed:
        return QStringLiteral("A channel entry is malformed");
    }
    return QStringLiteral("Unknown update information error");
}

bool isValidSha256Hex(const QString &value)
{
    if (value.size() != 64)
        return false;
    for (const QChar c : value) {
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        const bool lower = c >= QLatin1Char('a') && c <= QLatin1Char('f');
        if (!digit && !lower)
            return false; // uppercase is rejected: the format is fixed
    }
    return true;
}

bool isSafeArtifactFilename(const QString &filename)
{
    if (filename.isEmpty() || filename.size() > 255)
        return false;
    if (filename.contains(QLatin1Char('/')) || filename.contains(QLatin1Char('\\')))
        return false;
    if (filename.contains(QLatin1String("..")))
        return false;
    if (filename == QLatin1String(".") || filename == QLatin1String(".."))
        return false;
    // A leading dash would be read as an option by anything taking argv.
    if (filename.startsWith(QLatin1Char('-')))
        return false;
    if (filename.contains(QLatin1Char(':')))
        return false;
    for (const QChar c : filename) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return false;
        if (c.isSpace() && c != QLatin1Char(' '))
            return false;
    }
    // The verified artifact is renamed to this name inside the update staging
    // directory, which already holds two files of its own. A manifest naming
    // one of them would have the artifact delete the single-instance lock, or
    // be deleted by the next launch's status-file cleanup. Only an operator
    // mistake can produce this -- the manifest is signed -- but the cost of
    // refusing it is nothing.
    if (filename == QLatin1String("update.lock")
        || filename == QLatin1String("update-status.json")
        || filename.endsWith(QLatin1String(".lock"))) {
        return false;
    }
    return true;
}

QString UpdateManifest::channelIdForInstallType(InstallType type)
{
    switch (type) {
    case InstallType::LinuxFlatpak:
        return QStringLiteral("linux-flatpak");
    case InstallType::LinuxSnap:
        return QStringLiteral("linux-snap");
    case InstallType::LinuxDeb:
        return QStringLiteral("linux-deb-repo");
    case InstallType::LinuxRpm:
        return QStringLiteral("linux-rpm-repo");
    default:
        break;
    }
    return {};
}

UpdateManifest::Result UpdateManifest::parseVerified(const QByteArray &manifestBytes,
                                                     const QByteArray &sigBytes,
                                                     const TrustStore &trust)
{
    if (sigBytes.isEmpty())
        return failure(ManifestError::SignatureMissing);
    if (manifestBytes.isEmpty())
        return failure(ManifestError::ManifestNotJson);
    if (manifestBytes.size() > kMaxManifestBytes)
        return failure(ManifestError::ManifestTooLarge);

    // STEP 1 — signature over the raw bytes. Nothing below runs otherwise.
    const SignatureResult signature = verifyDetachedSignature(manifestBytes, sigBytes, trust);
    if (!signature.ok)
        return failure(manifestErrorForSignature(signature.error));

    // STEP 2 — only now is the document parsed.
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return failure(ManifestError::ManifestNotJson);
    if (!document.isObject())
        return failure(ManifestError::ManifestNotObject);

    const QJsonObject root = document.object();
    UpdateManifest manifest;
    manifest.m_signingKeyId = signature.keyId;

    const QJsonValue schemaValue = root.value(QLatin1String("schema"));
    if (!schemaValue.isDouble())
        return failure(ManifestError::UnsupportedSchema);
    manifest.m_schema = schemaValue.toInt(-1);
    if (manifest.m_schema != kSupportedSchema)
        return failure(ManifestError::UnsupportedSchema);

    const QJsonValue minUpdater = root.value(QLatin1String("min_updater_version"));
    if (!minUpdater.isUndefined()) {
        if (!minUpdater.isDouble())
            return failure(ManifestError::UnsupportedUpdaterVersion);
        manifest.m_minUpdaterVersion = minUpdater.toInt(-1);
    }
    if (manifest.m_minUpdaterVersion < 1 || manifest.m_minUpdaterVersion > kUpdaterVersion)
        return failure(ManifestError::UnsupportedUpdaterVersion);

    const QJsonValue versionValue = root.value(QLatin1String("version"));
    if (!versionValue.isString() || versionValue.toString().isEmpty())
        return failure(ManifestError::MissingVersion);
    manifest.m_versionString = versionValue.toString();
    const std::optional<Version> parsedVersion = Version::parse(manifest.m_versionString);
    if (!parsedVersion)
        return failure(ManifestError::MalformedVersion);
    manifest.m_version = *parsedVersion;

    const QJsonValue channelValue = root.value(QLatin1String("channel"));
    if (!channelValue.isString() || channelValue.toString().isEmpty())
        return failure(ManifestError::MissingChannel);
    manifest.m_channel = channelValue.toString();

    const QJsonValue tagValue = root.value(QLatin1String("tag"));
    if (tagValue.isString())
        manifest.m_tag = tagValue.toString();

    const QJsonValue releasedValue = root.value(QLatin1String("released"));
    if (releasedValue.isString()) {
        // Presentation only: an unparseable timestamp is left null rather
        // than failing an otherwise valid, signed release.
        manifest.m_released = QDateTime::fromString(releasedValue.toString(), Qt::ISODate);
    }

    // REQUIRED, unlike `released`: it is the one field that says whether a
    // correctly signed document is still the current one. Absent or
    // unparseable is a failure, never "no expiry".
    const QJsonValue expiresValue = root.value(QLatin1String("expires"));
    if (!expiresValue.isString() || expiresValue.toString().isEmpty())
        return failure(ManifestError::MissingExpiry);
    manifest.m_expires = QDateTime::fromString(expiresValue.toString(), Qt::ISODate);
    if (!manifest.m_expires.isValid())
        return failure(ManifestError::MalformedExpiry);
    // A timestamp with no zone designator is a local time on whatever
    // machine reads it; the generator always writes a "Z" instant.
    if (manifest.m_expires.timeSpec() == Qt::LocalTime)
        return failure(ManifestError::MalformedExpiry);

    const QJsonValue notesValue = root.value(QLatin1String("release_notes"));
    if (notesValue.isString())
        manifest.m_releaseNotes = notesValue.toString();

    const QJsonValue notesUrlValue = root.value(QLatin1String("release_notes_url"));
    if (notesUrlValue.isString() && !notesUrlValue.toString().isEmpty()) {
        const QUrl url(notesUrlValue.toString(), QUrl::StrictMode);
        // METADATA: canonical host only, never a mirror. This link is opened
        // in the user's browser and it describes the release; a bandwidth
        // mirror has no business serving either.
        if (!isAllowedManifestUrl(url))
            return failure(ManifestError::ReleaseNotesUrlRejected);
        manifest.m_releaseNotesUrl = url;
    }

    const QJsonValue artifactsValue = root.value(QLatin1String("artifacts"));
    if (!artifactsValue.isUndefined() && !artifactsValue.isNull()) {
        if (!artifactsValue.isObject())
            return failure(ManifestError::ArtifactMalformed);
        const QJsonObject artifacts = artifactsValue.toObject();
        for (auto it = artifacts.constBegin(); it != artifacts.constEnd(); ++it) {
            const std::optional<InstallType> type = installTypeFromId(it.key());
            // Forward compatibility: an artifact key this build does not
            // know, or one for an install type Lightning never installs
            // itself, is ignored rather than treated as an error.
            if (!type || !isDownloadableInstallType(*type))
                continue;
            if (!it.value().isObject())
                return failure(ManifestError::ArtifactMalformed, it.key());
            const QJsonObject entry = it.value().toObject();

            ManifestArtifact artifact;
            artifact.installTypeId = it.key();

            const QJsonValue filenameValue = entry.value(QLatin1String("filename"));
            if (!filenameValue.isString())
                return failure(ManifestError::ArtifactMalformed, it.key());
            artifact.filename = filenameValue.toString();
            if (!isSafeArtifactFilename(artifact.filename))
                return failure(ManifestError::ArtifactBadFilename, it.key());

            const QJsonValue sizeValue = entry.value(QLatin1String("size"));
            if (!sizeValue.isDouble())
                return failure(ManifestError::ArtifactMalformed, it.key());
            artifact.size = static_cast<qint64>(sizeValue.toDouble());
            if (artifact.size <= 0 || artifact.size > kMaxArtifactBytes)
                return failure(ManifestError::ArtifactBadSize, it.key());

            const QJsonValue hashValue = entry.value(QLatin1String("sha256"));
            if (!hashValue.isString())
                return failure(ManifestError::ArtifactMalformed, it.key());
            artifact.sha256 = hashValue.toString();
            if (!isValidSha256Hex(artifact.sha256))
                return failure(ManifestError::ArtifactBadHash, it.key());

            const QJsonValue urlValue = entry.value(QLatin1String("url"));
            if (!urlValue.isString())
                return failure(ManifestError::ArtifactMalformed, it.key());
            const QUrl url(urlValue.toString(), QUrl::StrictMode);
            if (!url.isValid() || url.scheme() != QLatin1String("https"))
                return failure(ManifestError::ArtifactBadUrl, it.key());
            // The CANONICAL address is held to the metadata host policy, not
            // the artifact one. It is the fallback the mirror falls back TO,
            // so it must be the release authority's own host: if this were
            // allowed to name a mirror as well, a manifest could put both
            // addresses on the same third party and one outage there would
            // leave no working source at all. Hash verification would still
            // hold, but the availability guarantee would be gone.
            if (!isAllowedManifestUrl(url))
                return failure(ManifestError::ArtifactForeignHost, it.key());
            // The URL must actually name the file the manifest describes,
            // so the hash, the size and the stored name all refer to one
            // thing.
            if (url.fileName() != artifact.filename)
                return failure(ManifestError::ArtifactFilenameMismatch, it.key());
            artifact.url = url;

            // OPTIONAL mirror. Absent or JSON null means "no mirror for this
            // artifact" and everything downstream behaves exactly as it did
            // before mirrors existed.
            //
            // A mirror this build does not trust is DROPPED, not fatal. The
            // field is consumed as nothing but a download address, and the
            // downloader re-checks the host at transfer time anyway, so
            // ignoring it is fail-closed on trust and merely slower: the
            // artifact keeps its canonical address and downloads exactly as it
            // used to. Failing the document instead would be a fleet hazard --
            // the day the project moves the mirror, every already-installed
            // client with an older compiled-in host list would reject EVERY
            // later manifest outright, including the perfectly good GitLab
            // address inside it, and could never be updated again.
            //
            // A MALFORMED value is still fatal: a non-string, a non-https or
            // unparseable URL, or a name that disagrees with the entry cannot
            // be interpreted at all, and that is a broken document rather than
            // one describing a host we happen not to know.
            const QJsonValue mirrorValue = entry.value(QLatin1String("mirror_url"));
            if (!mirrorValue.isUndefined() && !mirrorValue.isNull()) {
                if (!mirrorValue.isString())
                    return failure(ManifestError::ArtifactBadMirrorUrl, it.key());
                const QUrl mirror(mirrorValue.toString(), QUrl::StrictMode);
                if (!mirror.isValid() || mirror.scheme() != QLatin1String("https"))
                    return failure(ManifestError::ArtifactBadMirrorUrl, it.key());
                // The same agreement the canonical address must keep: the
                // mirror has to name the very file this entry describes.
                if (mirror.fileName() != artifact.filename)
                    return failure(ManifestError::ArtifactFilenameMismatch, it.key());
                if (isAllowedArtifactUrl(mirror))
                    artifact.mirrorUrl = mirror;
                else
                    ++manifest.m_untrustedMirrorCount;
            }

            manifest.m_artifacts.insert(artifact.installTypeId, artifact);
        }
    }

    const QJsonValue channelsValue = root.value(QLatin1String("channels"));
    if (!channelsValue.isUndefined() && !channelsValue.isNull()) {
        if (!channelsValue.isObject())
            return failure(ManifestError::ChannelMalformed);
        const QJsonObject channels = channelsValue.toObject();
        for (auto it = channels.constBegin(); it != channels.constEnd(); ++it) {
            if (!it.value().isObject())
                return failure(ManifestError::ChannelMalformed, it.key());
            const QJsonObject entry = it.value().toObject();
            ManifestChannel channel;
            channel.id = it.key();
            const QJsonValue availableValue = entry.value(QLatin1String("available"));
            if (!availableValue.isBool())
                return failure(ManifestError::ChannelMalformed, it.key());
            channel.available = availableValue.toBool();
            const QJsonValue channelVersion = entry.value(QLatin1String("version"));
            if (channelVersion.isString()) {
                const std::optional<Version> parsed = Version::parse(channelVersion.toString());
                if (!parsed)
                    return failure(ManifestError::ChannelMalformed, it.key());
                channel.version = parsed;
            } else if (!channelVersion.isNull() && !channelVersion.isUndefined()) {
                return failure(ManifestError::ChannelMalformed, it.key());
            }
            const QJsonValue noteValue = entry.value(QLatin1String("note"));
            // Bounded at the parse: this string reaches a status label, and
            // a signed document is still an unbounded one.
            if (noteValue.isString())
                channel.note = noteValue.toString().left(kMaxChannelNoteChars);
            manifest.m_channels.insert(channel.id, channel);
        }
    }

    // Every field above is data. There is deliberately no place to put a
    // command, argument vector, script, or interpreter: unknown fields
    // (including "command" / "install_command") were never read.
    manifest.m_valid = true;

    Result result;
    result.ok = true;
    result.error = ManifestError::None;
    result.manifest = manifest;
    return result;
}

std::optional<ManifestArtifact> UpdateManifest::artifactFor(InstallType type) const
{
    return artifactForId(installTypeId(type));
}

std::optional<ManifestArtifact> UpdateManifest::artifactForId(const QString &installTypeId) const
{
    const auto it = m_artifacts.constFind(installTypeId);
    if (it == m_artifacts.constEnd())
        return std::nullopt;
    return *it;
}

QStringList UpdateManifest::artifactIds() const
{
    QStringList ids = m_artifacts.keys();
    ids.sort();
    return ids;
}

std::optional<ManifestChannel> UpdateManifest::channelFor(const QString &channelId) const
{
    const auto it = m_channels.constFind(channelId);
    if (it == m_channels.constEnd())
        return std::nullopt;
    return *it;
}

bool UpdateManifest::channelOffersUpdate(const QString &channelId, const Version &installed) const
{
    const std::optional<ManifestChannel> channel = channelFor(channelId);
    if (!channel || !channel->available || !channel->version)
        return false;
    return *channel->version > installed;
}

} // namespace lightning::update
