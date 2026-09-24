// macOS SpellBackend over NSSpellChecker: system dictionaries, learned words
// and automatic language identification. Objective-C++ so the common C++
// never sees an AppKit header; compiled only on Apple.
//
// Used only from the thread that created it (the GUI thread).
//
// Written against the documented AppKit API; not yet exercised on macOS.

#include "text/SpellBackend.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <QString>
#include <QStringList>

namespace {

QString fromNSString(NSString *string)
{
    return string ? QString::fromNSString(string) : QString();
}

class MacSpellBackend final : public SpellBackend
{
public:
    explicit MacSpellBackend(const QString &preferredLanguage)
    {
        @autoreleasepool {
            NSSpellChecker *checker = [NSSpellChecker sharedSpellChecker];
            m_tag = [NSSpellChecker uniqueSpellDocumentTag];
            for (NSString *lang in [checker availableLanguages])
                m_languages << spellTagToBcp47(fromNSString(lang));
            if (preferredLanguage.isEmpty()) {
                // Automatic: AppKit identifies the language. Report the first
                // preferred language as the resolved one.
                m_automatic = true;
                NSArray<NSString *> *preferred = [checker userPreferredLanguages];
                m_language = preferred.count > 0
                    ? spellTagToBcp47(fromNSString(preferred.firstObject))
                    : QString();
            } else {
                // An explicit choice must be checkable, or it is "no
                // dictionary". Exact tag first; only a tag the picker never
                // offered (e.g. a hand-edited "en-XX") falls back to its
                // language.
                const QString posix = spellTagToPosix(preferredLanguage);
                const QString bcp47 = spellTagToBcp47(preferredLanguage);
                const QString languageOnly = bcp47.section(QLatin1Char('-'), 0, 0);
                for (NSString *lang in [checker availableLanguages]) {
                    const QString have = fromNSString(lang);
                    if (have == posix || spellTagToBcp47(have) == bcp47) {
                        m_language = spellTagToBcp47(have);
                        m_nsLanguage = [lang copy];
                        break;
                    }
                }
                if (m_nsLanguage == nil && !languageOnly.isEmpty()) {
                    for (NSString *lang in [checker availableLanguages]) {
                        const QString have = fromNSString(lang);
                        if (have == languageOnly) {
                            m_language = spellTagToBcp47(have);
                            m_nsLanguage = [lang copy];
                            break;
                        }
                    }
                }
            }
        }
    }

    ~MacSpellBackend() override
    {
        @autoreleasepool {
            [[NSSpellChecker sharedSpellChecker] closeSpellDocumentWithTag:m_tag];
            [m_nsLanguage release];
        }
    }

    bool resolved() const { return m_automatic || m_nsLanguage != nil; }

    bool isCorrect(const QString &word) const override
    {
        @autoreleasepool {
            NSString *text = word.toNSString();
            NSInteger count = 0;
            const NSRange range = [[NSSpellChecker sharedSpellChecker]
                checkSpellingOfString:text
                           startingAt:0
                             language:m_automatic ? nil : m_nsLanguage
                                 wrap:NO
               inSpellDocumentWithTag:m_tag
                            wordCount:&count];
            return range.location == NSNotFound;
        }
    }

    QStringList suggest(const QString &word) const override
    {
        QStringList out;
        @autoreleasepool {
            NSString *text = word.toNSString();
            NSArray<NSString *> *guesses = [[NSSpellChecker sharedSpellChecker]
                guessesForWordRange:NSMakeRange(0, text.length)
                           inString:text
                           language:m_automatic ? nil : m_nsLanguage
             inSpellDocumentWithTag:m_tag];
            for (NSString *guess in guesses) {
                out << fromNSString(guess);
                if (out.size() >= 16)
                    break;
            }
        }
        return out;
    }

    void addToPersonalDictionary(const QString &word) override
    {
        @autoreleasepool {
            // The user's own learned words, shared with every Mac app.
            [[NSSpellChecker sharedSpellChecker] learnWord:word.toNSString()];
        }
    }

    QString language() const override { return m_language; }
    QStringList availableLanguages() const override { return m_languages; }
    QString name() const override { return QStringLiteral("macos"); }

private:
    NSInteger m_tag = 0;
    NSString *m_nsLanguage = nil;
    bool m_automatic = false;
    QString m_language;
    QStringList m_languages;
};

} // namespace

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage, SpellBackendFailure *failure,
    QStringList *availableLanguages)
{
    auto backend = std::make_unique<MacSpellBackend>(preferredLanguage);
    if (availableLanguages)
        *availableLanguages = backend->availableLanguages();
    if (!backend->resolved()) {
        if (failure)
            *failure = QStringLiteral("no-dictionary");
        return {};
    }
    return backend;
}
