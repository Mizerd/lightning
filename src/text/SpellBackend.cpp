#include "text/SpellBackend.h"

#include <QByteArray>
#include <QLibrary>
#include <QLocale>
#include <QtGlobal>

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

    void addToPersonalDictionary(const QString &word) override
    {
        if (!m_checker)
            return;
        const std::wstring wide = word.toStdWString();
        // The user's Windows custom dictionary, shared with other apps.
        m_checker->Add(wide.c_str());
    }

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

#else // no <spellcheck.h>

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &, SpellBackendFailure *failure, QStringList *)
{
    setFailure(failure, "no-platform");
    return {};
}

#endif // LIGHTNING_HAVE_WIN_SPELLCHECK

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
    using DictAdd = void (*)(void *, const char *, ssize_t);

    BrokerInit brokerInit = nullptr;
    BrokerFree brokerFree = nullptr;
    BrokerRequestDict requestDict = nullptr;
    BrokerFreeDict freeDict = nullptr;
    BrokerDictExists dictExists = nullptr;
    BrokerListDicts listDicts = nullptr; // optional: the picker degrades
    DictCheck check = nullptr;
    DictSuggest suggest = nullptr;
    DictFreeStringList freeStringList = nullptr;
    DictAdd add = nullptr;

    bool complete() const
    {
        return brokerInit && brokerFree && requestDict && freeDict
            && dictExists && check && suggest && freeStringList && add;
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
        m_api.add =
            reinterpret_cast<EnchantApi::DictAdd>(resolve("enchant_dict_add"));
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

    void addToPersonalDictionary(const QString &word) override
    {
        if (!m_dict)
            return;
        const QByteArray utf8 = word.toUtf8();
        // Writes the user's ~/.config/enchant word list.
        m_api.add(m_dict, utf8.constData(),
                  static_cast<ssize_t>(utf8.size()));
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

} // namespace

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage, SpellBackendFailure *failure,
    QStringList *availableLanguages)
{
    auto backend = std::make_unique<EnchantSpellBackend>();
    const bool opened = backend->open(candidateTags(preferredLanguage), failure);
    if (availableLanguages)
        *availableLanguages = backend->availableLanguages();
    if (!opened)
        return {};
    return backend;
}

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

#endif
