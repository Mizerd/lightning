#pragma once

#include <QObject>
#include <QStringList>

// v0.1 placeholder. Real implementation in v0.5 with hierarchy walks and
// rooms grouped by Space.
class SpaceManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QStringList spaceIds READ spaceIds NOTIFY spaceIdsChanged)

public:
    explicit SpaceManager(QObject *parent = nullptr);

    QStringList spaceIds() const { return m_spaceIds; }

    Q_INVOKABLE QStringList roomsInSpace(const QString &spaceId) const;

Q_SIGNALS:
    void spaceIdsChanged();

private:
    QStringList m_spaceIds;
};
