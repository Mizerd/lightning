#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class SettingsManager;

class MediaVisibilityStore : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenCountChanged)

public:
    explicit MediaVisibilityStore(QObject *parent = nullptr);

    Q_INVOKABLE bool isHidden(const QString &key) const;
    Q_INVOKABLE void hide(const QString &key);
    Q_INVOKABLE void show(const QString &key);
    Q_INVOKABLE void setHidden(const QString &key, bool hidden);
    Q_INVOKABLE void clear();

    void setSettings(SettingsManager *settings);
    /// Drops the in-memory set without writing (sign-out). clear() would
    /// persist the empty set and lose the account's saved list.
    void resetForSession();
    void reloadForAccount();

    int hiddenCount() const { return int(m_hidden.size()); }

    static constexpr int kMaxHidden = 4096;

Q_SIGNALS:
    void hiddenChanged(const QString &key, bool hidden);
    void hiddenCountChanged();

private:
    void persist();

    SettingsManager *m_settings = nullptr;
    QSet<QString> m_hidden;
    QStringList m_order;
};
