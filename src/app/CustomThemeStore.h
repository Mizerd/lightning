#pragma once

#include <QJsonObject>
#include <QList>
#include <functional>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class SettingsManager;

class CustomThemeStore : public QObject
{
    Q_OBJECT

    // [{ key, label, group, hint }], the editable roles in editor order.
    // Translated, so re-read on a language change rather than CONSTANT.
    Q_PROPERTY(QVariantList roles READ roles NOTIFY rolesChanged)
    Q_PROPERTY(QVariantList themes READ themes NOTIFY customThemeChanged)
    Q_PROPERTY(QString activeThemeId READ activeThemeId WRITE setActiveThemeId
                   NOTIFY customThemeChanged)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY customThemeChanged)
    Q_PROPERTY(int baseTheme READ baseTheme WRITE setBaseTheme
                   NOTIFY customThemeChanged)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY customThemeChanged)
    Q_PROPERTY(bool exists READ exists NOTIFY customThemeChanged)
    Q_PROPERTY(int overrideCount READ overrideCount NOTIFY customThemeChanged)
    Q_PROPERTY(QVariantMap gradients READ gradients NOTIFY customThemeChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY historyChanged)
    Q_PROPERTY(QVariantList colorGroups READ colorGroups NOTIFY customThemeChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY historyChanged)

public:
    explicit CustomThemeStore(SettingsManager *settings,
                              QObject *parent = nullptr);

    static constexpr int kCustomThemeId = 12;

    QVariantList roles() const;
    QVariantList themes() const;
    QString activeThemeId() const;
    void setActiveThemeId(const QString &id);
    QVariantMap colors() const;
    int baseTheme() const;
    QString name() const;
    bool exists() const;
    int overrideCount() const;
    QVariantMap gradients() const;

    void setBaseTheme(int themeId);
    void setName(const QString &name);

    Q_INVOKABLE bool setColor(const QString &role, const QString &hex);
    Q_INVOKABLE void resetColor(const QString &role);
    Q_INVOKABLE bool setGradient(const QString &role, const QVariantMap &spec);
    Q_INVOKABLE void resetGradient(const QString &role);
    Q_INVOKABLE bool isGradientRole(const QString &role) const;
    Q_INVOKABLE bool applyToGroup(const QString &role, const QString &hex);
    Q_INVOKABLE QStringList groupMembers(const QString &group) const;
    QVariantList colorGroups() const;

    Q_INVOKABLE QVariantMap createColorGroup(const QString &name,
                                             const QStringList &roles,
                                             const QString &hex,
                                             bool moveFromOtherGroups);
    Q_INVOKABLE bool setGroupColor(const QString &groupId, const QString &hex);
    Q_INVOKABLE bool renameColorGroup(const QString &groupId, const QString &name);
    Q_INVOKABLE QVariantMap addToColorGroup(const QString &groupId,
                                            const QString &role,
                                            bool moveFromOtherGroups);
    Q_INVOKABLE bool detachFromColorGroup(const QString &role);
    Q_INVOKABLE bool deleteColorGroup(const QString &groupId);
    Q_INVOKABLE QString groupOfRole(const QString &role) const;
    Q_INVOKABLE QVariantMap colorGroupImpact(const QStringList &roles) const;

    static constexpr int kMaxColorGroups = 16;

    static int normalizeColorGroups(QVariantList *groups, QVariantMap *colors,
                                    const QVariantMap &gradients);

    Q_INVOKABLE void resetAll();
    Q_INVOKABLE void discard();

    bool canUndo() const;
    bool canRedo() const;
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE void sealUndoStep();
    QString undoLabel() const;
    QString redoLabel() const;
    Q_INVOKABLE void nameNextStep(const QString &label);
    Q_INVOKABLE void beginUndoGroup(const QString &label);
    Q_INVOKABLE void endUndoGroup();

    Q_INVOKABLE QString createTheme(const QString &name);
    Q_INVOKABLE QString duplicateActiveTheme(const QString &name);
    Q_INVOKABLE void deleteTheme(const QString &id);

    Q_INVOKABLE QString exportTheme(const QString &id) const;
    Q_INVOKABLE QString importTheme(const QString &payload);
    Q_INVOKABLE QVariantMap previewImport(const QString &payload) const;
    Q_INVOKABLE QString replaceActiveTheme(const QString &payload);
    Q_INVOKABLE QString readThemeFile(const QUrl &url) const;

    static constexpr int kMaxThemes = 24;
    static constexpr qint64 kMaxShareFileBytes = 24576;
    static constexpr int kMaxNameLength = 48;

    Q_INVOKABLE bool isValidColor(const QString &hex) const;
    Q_INVOKABLE bool isEditableRole(const QString &role) const;

    Q_INVOKABLE QVariantList audit(const QVariantMap &palette) const;
    Q_INVOKABLE QVariantList auditForRole(const QVariantMap &palette,
                                          const QString &role) const;
    Q_INVOKABLE QVariantList auditSkipped(const QVariantMap &palette,
                                          const QString &role) const;
    Q_INVOKABLE QVariantList auditWithGradients(const QVariantMap &palette,
                                                const QVariantMap &gradients) const;
    Q_INVOKABLE double contrastRatio(const QString &a, const QString &b) const;
    Q_INVOKABLE double lightness(const QString &hex) const;
    Q_INVOKABLE QString readableColor(const QString &hex, const QString &against,
                                      double minimum, bool edge) const;

    static QStringList editableRoles();
    static bool roleIsEditable(const QString &role);
    static bool colorIsValid(const QString &hex);
    static QVariantMap sanitize(const QVariantMap &raw);
    static QStringList gradientRoles();
    static bool roleTakesGradient(const QString &role);
    static QVariantMap sanitizeGradient(const QVariant &raw);
    static QVariantList sanitizeGradientPositions(const QVariant &raw, int stopCount);
    static QVariantMap sanitizeGradients(const QVariantMap &raw);
    static constexpr int kMinGradientStops = 2;
    static constexpr int kMaxGradientStops = 3;
    static double contrast(const QString &a, const QString &b);
    static double lstar(const QString &hex);
    static QString nudgeToPass(const QString &hex, const QString &against,
                               double minimum, bool edge);
    static QString roleLabel(const QString &role);
    static QString roleFollows(const QString &role);
    static int readabilityCheckCount();
    static QVariantList readabilityChecks();
    static QVariantMap paletteKeyAliases();
    Q_INVOKABLE QVariantMap roleAliases() const;
    Q_INVOKABLE QString paletteKeyForRole(const QString &role) const;
    static QStringList readabilityPaletteKeys();
    static QVariantMap derivedPaletteKeys();

Q_SIGNALS:
    void customThemeChanged();
    void rolesChanged();
    void historyChanged();

private:
    // Drops the parsed cache so the next read uses the account active now.
    //
    // This is a data-loss guard: the collection is account-scoped
    // (SettingsManager::appearanceValue), and without it account B's first
    // write would save account A's cached themes over B's record. Wired to
    // SettingsManager::sessionChanged, which fires after the active account
    // id has moved (switch, add, sign-out).
    void invalidate();

    struct Theme {
        QString id;
        QString name;
        int baseTheme = 0;
        QVariantMap colors;
        QVariantMap gradients;
        QVariantList groups;
    };

    void store(const QVariantMap &colors);
    const QList<Theme> &load() const;
    void save(const QList<Theme> &themes, const QString &activeId);
    int activeIndex() const;
    static QString makeId(const QList<Theme> &existing);
    static bool parseShared(const QString &payload, Theme *out, QString *error);
    static Theme fromJson(const QJsonObject &object);
    static QJsonObject toJson(const Theme &theme);

    struct UndoStep {
        QString themeId;
        int baseBefore = 0;
        int baseAfter = 0;
        QVariantMap colorsBefore;
        QVariantMap colorsAfter;
        QVariantMap gradientsBefore;
        QVariantMap gradientsAfter;
        QVariantList groupsBefore;
        QVariantList groupsAfter;
        QString coalesce;
        QString label;
    };
    void recordStep(const QString &themeIdBefore, int baseBefore,
                    const QVariantMap &colorsBefore,
                    const QVariantMap &gradientsBefore, const QString &coalesce,
                    const QString &label);
    void recordStep(const QString &themeIdBefore, int baseBefore,
                    const QVariantMap &colorsBefore,
                    const QVariantMap &gradientsBefore,
                    const QVariantList &groupsBefore, const QString &coalesce,
                    const QString &label);
    QVariantList groupsRaw() const;
    bool editActive(const std::function<bool(Theme &)> &edit,
                    const QString &coalesce, const QString &label);
    void clearHistory();
    void applyHistoryState(int base, const QVariantMap &colors,
                           const QVariantMap &gradients,
                           const QVariantList &groups);
    QString m_pendingLabel;
    QString m_groupLabel;
    int m_groupDepth = 0;

    QList<UndoStep> m_undo;
    QList<UndoStep> m_redo;
    bool m_undoSealed = true;

    SettingsManager *m_settings = nullptr;
    mutable QList<Theme> m_cache;
    mutable QString m_activeId;
    mutable bool m_loaded = false;
};
