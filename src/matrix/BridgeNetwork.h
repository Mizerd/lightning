#pragma once

#include <QString>

namespace matrix::bridge {

QString networkIdForUserId(const QString &userId);

QString networkIdForAlias(const QString &alias);

QString networkIdForRoom(const QString &directUserId,
                         const QString &canonicalAlias);

QString labelForNetworkId(const QString &networkId);

struct AdvertisedBridgeLabel {
    QString networkId;
    QString label;
};
AdvertisedBridgeLabel labelForAdvertisedBridge(const QString &protocolId,
                                               const QString &protocolName,
                                               const QString &networkName);

struct DmNamePresentation {
    QString name;
    QString networkLabel;
};
DmNamePresentation presentableDmName(const QString &computedName,
                                     const QString &directUserId);

}
