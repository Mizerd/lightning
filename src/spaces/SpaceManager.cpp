#include "spaces/SpaceManager.h"

SpaceManager::SpaceManager(QObject *parent)
    : QObject(parent)
{
}

QStringList SpaceManager::roomsInSpace(const QString &) const
{
    return {};
}
