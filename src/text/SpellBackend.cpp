#include "text/SpellBackend.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QLocale>
#include <QRegularExpression>
#include <QStringConverter>
#include <QtGlobal>

#include <utility>

QString spellTagToBcp47(const QString &tag)
{
    QString out = tag.trimmed();
    out.replace(QLatin1Char('_'), QLatin1Char('-'));
    return out;
}

QString spellTagToPosix(const QString &tag)
{
    QString out = tag.trimmed();
    out.replace(QLatin1Char('-'), QLatin1Char('_'));
    return out;
}

namespace {

// Tags to try in order: "en_GB", then "en", since distributions commonly ship
// only one of the two.
[[maybe_unused]] QStringList candidateTags(const QString &preferred)
{
    QString wanted = spellTagToPosix(preferred);
    if (wanted.isEmpty())
        wanted = QLocale::system().name(); // "en_GB"

    QStringList tags;
    if (!wanted.isEmpty() && wanted != QLatin1String("C")) {
        tags << wanted;
        const int sep = wanted.indexOf(QLatin1Char('_'));
        if (sep > 0)
            tags << wanted.left(sep);
    }
    // English as a last resort for the system preference only; an explicit
    // choice is never silently checked against another language.
    if (preferred.isEmpty()) {
        if (!tags.contains(QStringLiteral("en_US")))
            tags << QStringLiteral("en_US");
        if (!tags.contains(QStringLiteral("en")))
            tags << QStringLiteral("en");
    }
    return tags;
}

[[maybe_unused]] void setFailure(SpellBackendFailure *failure, const char *reason)
{
    if (failure)
        *failure = QString::fromLatin1(reason);
}

} // namespace

// ---------------------------------------------------------------------------
// Windows: the operating system's own spell checker.
// ---------------------------------------------------------------------------
#if defined(Q_OS_WIN)

#include <windows.h>

// Guarded so a toolchain without <spellcheck.h> still builds; the backend is
// then absent and `--spell-status` reports it.
#if defined(__has_include)
#  if __has_include(<spellcheck.h>)
#    define LIGHTNING_HAVE_WIN_SPELLCHECK 1
#  endif
#endif

#if defined(LIGHTNING_HAVE_WIN_SPELLCHECK)
#include <spellcheck.h>

namespace {

// CLSID_SpellCheckerFactory, written out (it matches mingw-w64's
// spellcheck.h): DEFINE_GUID only declares the symbol without INITGUID, and
// INITGUID would emit every GUID in every included header. The interface ids
// come from the header through IID_PPV_ARGS.
const CLSID kSpellCheckerFactoryClsid = {
    0x7ab36653, 0x1796, 0x484b,
    { 0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc }
};

// ISpellChecker is apartment-threaded; it is only used on the thread that
// created it. Per-word checks are cheap and cached by SpellChecker.
class WindowsSpellBackend final : public SpellBackend
{
public:
    ~WindowsSpellBackend() override
    {
        if (m_checker)
            m_checker->Release();
        if (m_factory)
            m_factory->Release();
        if (m_ownsComInit)
            CoUninitialize();
    }

    // False when no candidate tag has a checker, which is normal without an
    // installed proofing language.
    bool open(const QStringList &tags, SpellBackendFailure *failure)
    {
        // Initialise COM ourselves: in the app the QPA plugin already has
        // (S_FALSE), but `--spell-status` runs under a bare QCoreApplication
        // where nothing has. S_OK and S_FALSE both need a matching
        // CoUninitialize; RPC_E_CHANGED_MODE (other apartment model) does not.
        const HRESULT comInit =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_ownsComInit = SUCCEEDED(comInit);

        HRESULT hr = CoCreateInstance(kSpellCheckerFactoryClsid, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&m_factory));
        if (FAILED(hr) || !m_factory) {
            setFailure(failure, "no-library");
            return false;
        }
        readSupportedLanguages();
        for (const QString &tag : tags) {
            // Windows wants a BCP-47 tag ("en-GB"), not the POSIX spelling.
            const QString bcp47 = spellTagToBcp47(tag);
            const std::wstring wide = bcp47.toStdWString();
            BOOL supported = FALSE;
            if (FAILED(m_factory->IsSupported(wide.c_str(), &supported))
                || !supported) {
                continue;
            }
            if (SUCCEEDED(m_factory->CreateSpellChecker(wide.c_str(),
                                                        &m_checker))
                && m_checker) {
                m_language = bcp47;
                return true;
            }
        }
        setFailure(failure, "no-dictionary");
        return false;
    }

    bool isCorrect(const QString &word) const override
    {
        if (!m_checker)
            return true;
        const std::wstring wide = word.toStdWString();
        IEnumSpellingError *errors = nullptr;
        if (FAILED(m_checker->Check(wide.c_str(), &errors)) || !errors)
            return true;
        ISpellingError *error = nullptr;
        // One error is enough; release both interfaces either way.
        const bool wrong = (errors->Next(&error) == S_OK) && error != nullptr;
        if (error)
            error->Release();
        errors->Release();
        return !wrong;
    }

    QStringList suggest(const QString &word) const override
    {
        QStringList out;
        if (!m_checker)
            return out;
        const std::wstring wide = word.toStdWString();
        IEnumString *suggestions = nullptr;
        if (FAILED(m_checker->Suggest(wide.c_str(), &suggestions))
            || !suggestions) {
            return out;
        }
        LPOLESTR item = nullptr;
        while (suggestions->Next(1, &item, nullptr) == S_OK && item) {
            out << QString::fromWCharArray(item);
            CoTaskMemFree(item);
            item = nullptr;
            if (out.size() >= 16) // bounded: this feeds a context menu
                break;
        }
        suggestions->Release();
        return out;
    }

    // Windows' own autocorrect list: an error whose corrective action is
    // REPLACE carries the one replacement Windows itself would apply
    // ("teh" -> "the"). GET_SUGGESTIONS errors are not corrections.
    QString autoCorrection(const QString &word) const override
    {
        if (!m_checker)
            return {};
        const std::wstring wide = word.toStdWString();
        IEnumSpellingError *errors = nullptr;
        if (FAILED(m_checker->Check(wide.c_str(), &errors)) || !errors)
            return {};
        QString out;
        ISpellingError *error = nullptr;
        if (errors->Next(&error) == S_OK && error) {
            CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
            ULONG start = 0;
            ULONG length = 0;
            // The error must cover the whole word, or the replacement is for
            // a part of it.
            if (SUCCEEDED(error->get_CorrectiveAction(&action))
                && action == CORRECTIVE_ACTION_REPLACE
                && SUCCEEDED(error->get_StartIndex(&start))
                && SUCCEEDED(error->get_Length(&length)) && start == 0
                && length == static_cast<ULONG>(wide.size())) {
                LPWSTR replacement = nullptr;
                if (SUCCEEDED(error->get_Replacement(&replacement))
                    && replacement) {
                    out = QString::fromWCharArray(replacement);
                    CoTaskMemFree(replacement);
                }
            }
            error->Release();
        }
        errors->Release();
        return out;
    }
    bool hasNativeAutoCorrection() const override { return true; }

    QString language() const override { return m_language; }
    QStringList availableLanguages() const override { return m_languages; }
    QString name() const override { return QStringLiteral("windows"); }

private:
    void readSupportedLanguages()
    {
        IEnumString *languages = nullptr;
        if (FAILED(m_factory->get_SupportedLanguages(&languages)) || !languages)
            return;
        LPOLESTR item = nullptr;
        while (languages->Next(1, &item, nullptr) == S_OK && item) {
            m_languages << QString::fromWCharArray(item);
            CoTaskMemFree(item);
            item = nullptr;
            if (m_languages.size() >= 256)
                break;
        }
        languages->Release();
    }

    ISpellCheckerFactory *m_factory = nullptr;
    ISpellChecker *m_checker = nullptr;
    QString m_language;
    QStringList m_languages;
    bool m_ownsComInit = false;
};

} // namespace

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage, SpellBackendFailure *failure,
    QStringList *availableLanguages)
{
    auto backend = std::make_unique<WindowsSpellBackend>();
    const bool opened = backend->open(candidateTags(preferredLanguage), failure);
    if (availableLanguages)
        *availableLanguages = backend->availableLanguages();
    if (!opened)
        return {};
    return backend;
}

bool platformSpellEngineCompiledIn() { return true; }

#else // no <spellcheck.h>

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &, SpellBackendFailure *failure, QStringList *)
{
    setFailure(failure, "no-platform");
    return {};
}

bool platformSpellEngineCompiledIn() { return false; }

#endif // LIGHTNING_HAVE_WIN_SPELLCHECK

// Windows autocorrects a hardware keyboard only when the user turns it on
// (Settings > Time & language > Typing), and offers no API to read that
// switch, so Lightning's default is off.
bool platformAutocorrectDefault() { return false; }

// ---------------------------------------------------------------------------
// Linux / BSD: enchant-2, resolved at runtime.
// ---------------------------------------------------------------------------
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)

namespace {

// Resolved at runtime through QLibrary, so enchant is not a build dependency.
struct EnchantApi
{
    using BrokerInit = void *(*)();
    using BrokerFree = void (*)(void *);
    using BrokerRequestDict = void *(*)(void *, const char *);
    using BrokerFreeDict = void (*)(void *, void *);
    using BrokerDictExists = int (*)(void *, const char *);
    using DictDescribeFn = void (*)(const char *, const char *, const char *,
                                    const char *, void *);
    using BrokerListDicts = void (*)(void *, DictDescribeFn, void *);
    using DictCheck = int (*)(void *, const char *, ssize_t);
    using DictSuggest = char **(*)(void *, const char *, ssize_t, size_t *);
    using DictFreeStringList = void (*)(void *, char **);

    BrokerInit brokerInit = nullptr;
    BrokerFree brokerFree = nullptr;
    BrokerRequestDict requestDict = nullptr;
    BrokerFreeDict freeDict = nullptr;
    BrokerDictExists dictExists = nullptr;
    BrokerListDicts listDicts = nullptr; // optional: the picker degrades
    DictCheck check = nullptr;
    DictSuggest suggest = nullptr;
    DictFreeStringList freeStringList = nullptr;

    bool complete() const
    {
        return brokerInit && brokerFree && requestDict && freeDict
            && dictExists && check && suggest && freeStringList;
    }
};

class EnchantSpellBackend final : public SpellBackend
{
public:
    ~EnchantSpellBackend() override
    {
        if (m_broker && m_dict && m_api.freeDict)
            m_api.freeDict(m_broker, m_dict);
        if (m_broker && m_api.brokerFree)
            m_api.brokerFree(m_broker);
        // The QLibrary is not unloaded: enchant's providers are dlopened
        // plugins of their own.
    }

    bool open(const QStringList &tags, SpellBackendFailure *failure)
    {
        // Without a -dev package only the versioned soname exists.
        static const char *const kNames[] = {
            "libenchant-2.so.2",
            "libenchant-2.so",
        };
        for (const char *name : kNames) {
            m_library.setFileName(QString::fromLatin1(name));
            if (m_library.load())
                break;
        }
        if (!m_library.isLoaded()) {
            setFailure(failure, "no-library");
            return false;
        }
        auto resolve = [this](const char *symbol) {
            return m_library.resolve(symbol);
        };
        m_api.brokerInit =
            reinterpret_cast<EnchantApi::BrokerInit>(resolve("enchant_broker_init"));
        m_api.brokerFree =
            reinterpret_cast<EnchantApi::BrokerFree>(resolve("enchant_broker_free"));
        m_api.requestDict = reinterpret_cast<EnchantApi::BrokerRequestDict>(
            resolve("enchant_broker_request_dict"));
        m_api.freeDict = reinterpret_cast<EnchantApi::BrokerFreeDict>(
            resolve("enchant_broker_free_dict"));
        m_api.dictExists = reinterpret_cast<EnchantApi::BrokerDictExists>(
            resolve("enchant_broker_dict_exists"));
        m_api.listDicts = reinterpret_cast<EnchantApi::BrokerListDicts>(
            resolve("enchant_broker_list_dicts"));
        m_api.check =
            reinterpret_cast<EnchantApi::DictCheck>(resolve("enchant_dict_check"));
        m_api.suggest =
            reinterpret_cast<EnchantApi::DictSuggest>(resolve("enchant_dict_suggest"));
        m_api.freeStringList = reinterpret_cast<EnchantApi::DictFreeStringList>(
            resolve("enchant_dict_free_string_list"));
        if (!m_api.complete()) {
            setFailure(failure, "no-library");
            return false;
        }
        m_broker = m_api.brokerInit();
        if (!m_broker) {
            setFailure(failure, "no-library");
            return false;
        }
        readAvailableLanguages();
        for (const QString &tag : tags) {
            const QByteArray utf8 = spellTagToPosix(tag).toUtf8();
            if (m_api.dictExists(m_broker, utf8.constData()) != 1)
                continue;
            m_dict = m_api.requestDict(m_broker, utf8.constData());
            if (m_dict) {
                m_language = spellTagToBcp47(tag);
                return true;
            }
        }
        setFailure(failure, "no-dictionary");
        return false;
    }

    bool isCorrect(const QString &word) const override
    {
        if (!m_dict)
            return true;
        const QByteArray utf8 = word.toUtf8();
        // 0 = correct, positive = misspelled, negative = provider error. An
        // error is not a misspelling.
        return m_api.check(m_dict, utf8.constData(),
                           static_cast<ssize_t>(utf8.size())) <= 0;
    }

    QStringList suggest(const QString &word) const override
    {
        QStringList out;
        if (!m_dict)
            return out;
        const QByteArray utf8 = word.toUtf8();
        size_t count = 0;
        char **items = m_api.suggest(m_dict, utf8.constData(),
                                     static_cast<ssize_t>(utf8.size()), &count);
        if (!items)
            return out;
        for (size_t i = 0; i < count && i < 16; ++i) {
            if (items[i])
                out << QString::fromUtf8(items[i]);
        }
        m_api.freeStringList(m_dict, items);
        return out;
    }

    QString language() const override { return m_language; }
    QStringList availableLanguages() const override { return m_languages; }
    QString name() const override { return QStringLiteral("enchant"); }

private:
    static void describeDict(const char *tag, const char *, const char *,
                             const char *, void *userData)
    {
        auto *out = static_cast<QStringList *>(userData);
        if (!tag || out->size() >= 256)
            return;
        const QString bcp47 = spellTagToBcp47(QString::fromUtf8(tag));
        if (!out->contains(bcp47))
            out->append(bcp47);
    }

    void readAvailableLanguages()
    {
        if (!m_api.listDicts)
            return;
        // Provider order, deduplicated by tag.
        m_api.listDicts(m_broker, &EnchantSpellBackend::describeDict,
                        &m_languages);
    }

    QLibrary m_library;
    EnchantApi m_api;
    void *m_broker = nullptr;
    void *m_dict = nullptr;
    QString m_language;
    QStringList m_languages;
};

// ---------------------------------------------------------------------------
// Hunspell, directly. The fallback when enchant is absent (the Flatpak KDE
// runtime, the Snap, NixOS, a minimal AppImage host) or has no dictionary for
// the language: libhunspell is resolved at runtime like enchant, and the
// dictionaries come from the system or from the package's own bundle.
// ---------------------------------------------------------------------------

struct HunspellApi
{
    using Create = void *(*)(const char *, const char *);
    using Destroy = void (*)(void *);
    using Spell = int (*)(void *, const char *);
    using Suggest = int (*)(void *, char ***, const char *);
    using FreeList = void (*)(void *, char ***, int);
    using Encoding = char *(*)(void *);

    Create create = nullptr;
    Destroy destroy = nullptr;
    Spell spell = nullptr;
    Suggest suggest = nullptr;
    FreeList freeList = nullptr;
    Encoding encoding = nullptr;

    bool complete() const
    {
        return create && destroy && spell && suggest && freeList && encoding;
    }
};

QString applicationDir()
{
    // applicationDirPath() warns without an instance.
    return QCoreApplication::instance() ? QCoreApplication::applicationDirPath()
                                        : QString();
}

// Loaded once per process and never unloaded. Null when no candidate loads.
const HunspellApi *hunspellApi()
{
    static const HunspellApi *api = [] () -> const HunspellApi * {
        static QLibrary library;
        QStringList candidates;
        // The package's own copy first (AppImage and Snap stage it in
        // usr/lib next to usr/bin), so the host's version never matters.
        const QString app = applicationDir();
        if (!app.isEmpty())
            candidates << app + QStringLiteral("/../lib/libhunspell-1.7.so.0");
#if defined(LIGHTNING_HUNSPELL_LIBRARY)
        // Nix: an absolute store path, since NixOS has no global library path.
        // A plain char literal, not QStringLiteral: Nix finds runtime
        // dependencies by scanning the binary for the store path in ASCII, and
        // QStringLiteral stores UTF-16, which would drop Hunspell from the
        // package's closure.
        static const char kHunspellLibrary[] = LIGHTNING_HUNSPELL_LIBRARY;
        candidates << QString::fromUtf8(kHunspellLibrary);
#endif
        // Without a -dev package only the versioned soname exists.
        candidates << QStringLiteral("libhunspell-1.7.so.0")
                   << QStringLiteral("libhunspell-1.7.so")
                   << QStringLiteral("libhunspell-1.6.so.0")
                   << QStringLiteral("libhunspell.so");
        for (const QString &candidate : std::as_const(candidates)) {
            if (candidate.startsWith(QLatin1Char('/'))
                && !QFileInfo::exists(candidate)) {
                continue;
            }
            library.setFileName(candidate);
            if (library.load())
                break;
        }
        if (!library.isLoaded())
            return nullptr;
        static HunspellApi resolved;
        resolved.create = reinterpret_cast<HunspellApi::Create>(
            library.resolve("Hunspell_create"));
        resolved.destroy = reinterpret_cast<HunspellApi::Destroy>(
            library.resolve("Hunspell_destroy"));
        resolved.spell = reinterpret_cast<HunspellApi::Spell>(
            library.resolve("Hunspell_spell"));
        resolved.suggest = reinterpret_cast<HunspellApi::Suggest>(
            library.resolve("Hunspell_suggest"));
        resolved.freeList = reinterpret_cast<HunspellApi::FreeList>(
            library.resolve("Hunspell_free_list"));
        resolved.encoding = reinterpret_cast<HunspellApi::Encoding>(
            library.resolve("Hunspell_get_dic_encoding"));
        return resolved.complete() ? &resolved : nullptr;
    }();
    return api;
}

struct DictionaryFile
{
    QString name; // file stem: "en_US", "de_DE_frami", "lt"
    QString tag;  // BCP-47: "en-US", "de-DE", "lt"
    QString aff;
    QString dic;
};

// "en_US" -> "en-US", "de_DE_frami" -> "de-DE", "en_GB-ise" -> "en-GB",
// "lt" -> "lt"; "" for a stem that is not a language (hyph_*, th_*, ...).
QString tagForDictionaryStem(const QString &stem)
{
    static const QRegularExpression rx(
        QStringLiteral("^([a-z]{2,3})(?:[_-]([A-Z]{2}))?(?:[_-].*)?$"));
    const QRegularExpressionMatch m = rx.match(stem);
    if (!m.hasMatch())
        return {};
    const QString region = m.captured(2);
    return region.isEmpty() ? m.captured(1)
                            : m.captured(1) + QLatin1Char('-') + region;
}

// Every usable .aff/.dic pair, by directory priority; the first file for a
// stem wins. Distributions ship regional aliases as symlinks (Debian: es_AR
// ... es_VE -> es_ES, 19 for Arabic), so within a directory real files come
// first and a link to a dictionary already listed is dropped: the picker shows
// each dictionary once, and "es-MX" still resolves through its language. A
// directory of nothing but links (a Nix profile) keeps every distinct target.
QList<DictionaryFile> scanDictionaries(const QStringList &directories)
{
    QList<DictionaryFile> out;
    QStringList seenStems;
    QStringList seenTargets;
    for (const QString &directory : directories) {
        const QDir dir(directory);
        const QFileInfoList dics = dir.entryInfoList(
            { QStringLiteral("*.dic") }, QDir::Files | QDir::Readable,
            QDir::Name);
        for (int pass = 0; pass < 2; ++pass) {
            for (const QFileInfo &info : dics) {
                if (info.isSymLink() != (pass == 1))
                    continue;
                const QString stem = info.completeBaseName();
                const QString target = info.canonicalFilePath();
                if (seenStems.contains(stem) || seenTargets.contains(target))
                    continue;
                const QString tag = tagForDictionaryStem(stem);
                const QString aff = dir.filePath(stem + QStringLiteral(".aff"));
                if (tag.isEmpty() || !QFileInfo::exists(aff))
                    continue;
                seenStems << stem;
                seenTargets << target;
                out.append({ stem, tag, aff, info.filePath() });
                if (out.size() >= 512)
                    return out;
            }
        }
    }
    return out;
}

// The dictionary for one candidate ("en_GB" or "en"). Exact stem first, then
// a variant with the same tag (de_DE_frami for de_DE); for a bare language,
// the variant whose region repeats it (lt_LT, de_DE), then en_US for "en",
// then the first.
const DictionaryFile *findDictionary(const QList<DictionaryFile> &dicts,
                                     const QString &posixTag)
{
    const QString bcp47 = spellTagToBcp47(posixTag);
    for (const DictionaryFile &d : dicts)
        if (d.name == posixTag)
            return &d;
    for (const DictionaryFile &d : dicts)
        if (d.tag == bcp47)
            return &d;
    if (bcp47.contains(QLatin1Char('-')))
        return nullptr;
    const QString preferred = bcp47 + QLatin1Char('-')
        + (bcp47 == QLatin1String("en") ? QStringLiteral("US") : bcp47.toUpper());
    for (const DictionaryFile &d : dicts)
        if (d.tag == preferred)
            return &d;
    for (const DictionaryFile &d : dicts)
        if (d.tag.section(QLatin1Char('-'), 0, 0) == bcp47)
            return &d;
    return nullptr;
}

// Hunspell spells encodings its own way ("ISO8859-13", "microsoft-cp1251").
QByteArray normalizedEncodingName(QByteArray name)
{
    name = name.trimmed();
    const QByteArray upper = name.toUpper();
    if (upper.startsWith("ISO8859-"))
        return "ISO-8859-" + name.mid(8);
    if (upper.startsWith("MICROSOFT-CP"))
        return "windows-" + name.mid(12);
    if (upper == "UTF8")
        return "UTF-8";
    return name;
}

class HunspellSpellBackend final : public SpellBackend
{
public:
    ~HunspellSpellBackend() override
    {
        if (m_handle && m_api)
            m_api->destroy(m_handle);
    }

    bool open(const HunspellApi *api, const DictionaryFile &dict,
              const QStringList &languages)
    {
        m_api = api;
        m_languages = languages;
        const QByteArray aff = QFile::encodeName(dict.aff);
        const QByteArray dic = QFile::encodeName(dict.dic);
        m_handle = m_api->create(aff.constData(), dic.constData());
        if (!m_handle)
            return false;
        const char *encoding = m_api->encoding(m_handle);
        const QByteArray name =
            normalizedEncodingName(encoding ? QByteArray(encoding) : QByteArray());
        // A dictionary in an encoding this Qt cannot convert is skipped
        // rather than fed mojibake.
        m_encodingName = name.isEmpty() ? QByteArray("ISO-8859-1") : name;
        m_encoder = makeEncoder();
        m_decoder = makeDecoder();
        if (!m_encoder.isValid() || !m_decoder.isValid())
            return false;
        m_language = dict.tag;
        return true;
    }

    bool isCorrect(const QString &word) const override
    {
        bool ok = false;
        const QByteArray bytes = encode(word, &ok);
        // A word this dictionary's encoding cannot even represent is not
        // its to judge.
        if (!ok)
            return true;
        return m_api->spell(m_handle, bytes.constData()) != 0;
    }

    QStringList suggest(const QString &word) const override
    {
        QStringList out;
        bool ok = false;
        const QByteArray bytes = encode(word, &ok);
        if (!ok)
            return out;
        char **list = nullptr;
        const int count = m_api->suggest(m_handle, &list, bytes.constData());
        for (int i = 0; i < count && list && out.size() < 16; ++i) {
            if (!list[i])
                continue;
            const QString item = m_decoder.decode(QByteArrayView(list[i]));
            if (m_decoder.hasError())
                m_decoder = makeDecoder(); // see encode()
            else if (!item.isEmpty())
                out << item;
        }
        if (list)
            m_api->freeList(m_handle, &list, count);
        return out;
    }

    QString language() const override { return m_language; }
    QStringList availableLanguages() const override { return m_languages; }
    QString name() const override { return QStringLiteral("hunspell"); }

private:
    QByteArray encode(const QString &word, bool *ok) const
    {
        QByteArray bytes = m_encoder.encode(word);
        *ok = !bytes.isEmpty();
        if (m_encoder.hasError()) {
            // Measured on Qt 6.11 with ICU: resetState() leaves hasError()
            // set, so one unrepresentable word ("日本" against an
            // ISO-8859-13 dictionary) would make every later word look
            // unrepresentable. A fresh converter is the only reset.
            *ok = false;
            m_encoder = makeEncoder();
        }
        return bytes;
    }
    QStringEncoder makeEncoder() const
    {
        return QStringEncoder(m_encodingName.constData(),
                              QStringConverter::Flag::Stateless);
    }
    QStringDecoder makeDecoder() const
    {
        return QStringDecoder(m_encodingName.constData(),
                              QStringConverter::Flag::Stateless);
    }

    const HunspellApi *m_api = nullptr;
    void *m_handle = nullptr;
    QByteArray m_encodingName;
    mutable QStringEncoder m_encoder;
    mutable QStringDecoder m_decoder;
    QString m_language;
    QStringList m_languages;
};

void appendDirectory(QStringList *out, const QString &path)
{
    // A relative entry would resolve against whatever the working directory
    // happens to be (the XDG spec says to ignore them).
    if (path.isEmpty() || !QDir::isAbsolutePath(path))
        return;
    const QFileInfo info(path);
    if (!info.isDir())
        return;
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty() && !out->contains(canonical))
        out->append(canonical);
}

QStringList languagesIn(const QList<DictionaryFile> &dicts)
{
    QStringList out;
    for (const DictionaryFile &d : dicts)
        if (!out.contains(d.tag))
            out << d.tag;
    return out;
}

} // namespace

QStringList bundledSpellDictionaryDirectories()
{
    QStringList out;
    const QString app = applicationDir();
    if (!app.isEmpty())
        appendDirectory(&out, app + QStringLiteral("/../share/hunspell"));
    // The Flatpak module installs into the app prefix.
    appendDirectory(&out, QStringLiteral("/app/share/hunspell"));
#if defined(LIGHTNING_SPELL_DICTIONARY_DIRS)
    // ASCII on purpose, as kHunspellLibrary: the Nix reference scanner must
    // see the store path.
    static const char kDictionaryDirs[] = LIGHTNING_SPELL_DICTIONARY_DIRS;
    const QStringList built = QString::fromUtf8(kDictionaryDirs)
                                  .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &dir : built)
        appendDirectory(&out, dir);
#endif
    return out;
}

QStringList spellDictionaryDirectories()
{
    QStringList out;
    // The user's explicit choice first; Hunspell's own tools honour it too.
    const QStringList dicPath = qEnvironmentVariable("DICPATH")
                                    .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &dir : dicPath)
        appendDirectory(&out, dir);
    // The system's dictionaries before the bundled ones: a distribution's
    // newer word list wins, the bundle only fills what the host lacks.
    const QString home = QDir::homePath();
    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (dataHome.isEmpty() || !QDir::isAbsolutePath(dataHome))
        dataHome = home + QStringLiteral("/.local/share");
    appendDirectory(&out, dataHome + QStringLiteral("/hunspell"));
    QStringList dataDirs = qEnvironmentVariable("XDG_DATA_DIRS")
                               .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    dataDirs << QStringLiteral("/usr/local/share") << QStringLiteral("/usr/share");
    for (const QString &dir : std::as_const(dataDirs)) {
        appendDirectory(&out, dir + QStringLiteral("/hunspell"));
        appendDirectory(&out, dir + QStringLiteral("/myspell"));
        appendDirectory(&out, dir + QStringLiteral("/myspell/dicts"));
    }
    // NixOS: dictionaries installed system-wide or per user.
    const QString user = qEnvironmentVariable("USER");
    appendDirectory(&out, QStringLiteral("/run/current-system/sw/share/hunspell"));
    if (!user.isEmpty())
        appendDirectory(&out, QStringLiteral("/etc/profiles/per-user/") + user
                                  + QStringLiteral("/share/hunspell"));
    appendDirectory(&out, home + QStringLiteral("/.nix-profile/share/hunspell"));
    appendDirectory(&out, QStringLiteral("/nix/var/nix/profiles/default/share/hunspell"));
    for (const QString &dir : bundledSpellDictionaryDirectories())
        appendDirectory(&out, dir);
    return out;
}

std::unique_ptr<SpellBackend> createHunspellSpellBackend(
    const QString &preferredLanguage, const QStringList &dictionaryDirectories,
    SpellBackendFailure *failure, QStringList *availableLanguages)
{
    const HunspellApi *api = hunspellApi();
    if (!api) {
        setFailure(failure, "no-library");
        return {};
    }
    const QList<DictionaryFile> dicts = scanDictionaries(
        dictionaryDirectories.isEmpty() ? spellDictionaryDirectories()
                                        : dictionaryDirectories);
    const QStringList languages = languagesIn(dicts);
    if (availableLanguages)
        *availableLanguages = languages;
    for (const QString &tag : candidateTags(preferredLanguage)) {
        const DictionaryFile *dict = findDictionary(dicts, tag);
        if (!dict)
            continue;
        auto backend = std::make_unique<HunspellSpellBackend>();
        if (backend->open(api, *dict, languages))
            return backend;
    }
    setFailure(failure, "no-dictionary");
    return {};
}

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage, SpellBackendFailure *failure,
    QStringList *availableLanguages)
{
    // LIGHTNING_SPELL_ENGINE=enchant|hunspell pins one engine, for
    // diagnosis; anything else means enchant, then Hunspell.
    const QString engine =
        qEnvironmentVariable("LIGHTNING_SPELL_ENGINE").trimmed().toLower();
    QStringList offered;
    QString enchantFailure;
    if (engine != QLatin1String("hunspell")) {
        auto backend = std::make_unique<EnchantSpellBackend>();
        const bool opened =
            backend->open(candidateTags(preferredLanguage), &enchantFailure);
        offered = backend->availableLanguages();
        if (opened) {
            if (availableLanguages)
                *availableLanguages = offered;
            return backend;
        }
    }
    QString hunspellFailure;
    if (engine != QLatin1String("enchant")) {
        QStringList hunspellOffered;
        auto backend = createHunspellSpellBackend(
            preferredLanguage, {}, &hunspellFailure, &hunspellOffered);
        for (const QString &tag : std::as_const(hunspellOffered))
            if (!offered.contains(tag))
                offered << tag;
        if (backend) {
            if (availableLanguages)
                *availableLanguages = offered;
            return backend;
        }
    }
    if (availableLanguages)
        *availableLanguages = offered;
    // "no-library" only when no engine loaded at all: a loaded engine without
    // the dictionary is the more useful thing to say.
    const bool anyLoaded = (!enchantFailure.isEmpty()
                            && enchantFailure != QLatin1String("no-library"))
        || (!hunspellFailure.isEmpty()
            && hunspellFailure != QLatin1String("no-library"));
    setFailure(failure, anyLoaded ? "no-dictionary" : "no-library");
    return {};
}

bool platformSpellEngineCompiledIn() { return true; }
bool platformAutocorrectDefault() { return false; }

// ---------------------------------------------------------------------------
// macOS is SpellBackendMac.mm (NSSpellChecker). Everything else: no backend.
// ---------------------------------------------------------------------------
#elif !defined(Q_OS_MACOS)

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &, SpellBackendFailure *failure, QStringList *)
{
    setFailure(failure, "no-platform");
    return {};
}

bool platformSpellEngineCompiledIn() { return false; }
bool platformAutocorrectDefault() { return false; }

#endif
