// SPDX-License-Identifier: GPL-3.0-or-later
#include "networkcatalog.h"
#include "domainconfig.h"
#include "containment.h"
#include <QDomDocument>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QRegularExpression>
#include <QMap>
#include <sys/stat.h>
#include <unistd.h>

QVariantMap NetworkCatalog::describe(const QString &xml) {
    QDomDocument doc;
    if (!doc.setContent(xml) || doc.documentElement().tagName() != "network") return {};
    auto root = doc.documentElement(), forward = root.firstChildElement("forward"), ip = root.firstChildElement("ip");
    const QString mode = forward.attribute("mode");
    QString category, description;
    if (forward.isNull()) {
        category = ip.isNull() ? "Isolated" : "Host-only";
        description = ip.isNull()
                              ? "VMs on this network can only reach each other. Not this computer, not the internet."
                              : "VMs on this network can reach each other and this computer, but not the internet.";
    } else if (mode == "nat") {
        category = "Shared NAT";
        description = "VMs on this network can reach each other, this computer and the internet.";
    } else if (mode == "route" || mode == "open") {
        category = mode == "route" ? "Routed" : "Open";
        description = "What VMs can reach depends on this computer's routing and firewall.";
    } else {
        category = "Bridged";
        description = "VMs join your local network, as if plugged into it.";
    }
    const auto title = root.firstChildElement("title").text();
    auto name = root.firstChildElement("name").text();
    return {{"name", name},
            {"displayName", title.isEmpty() ? QString(name).remove(QRegularExpression("^omaware-")) : title},
            {"bridge", root.firstChildElement("bridge").attribute("name")}, {"category", category},
            {"description", description},
            {"subnet", ip.attribute("address") +
                               (ip.isNull() ? "" : " / " + ip.attribute("prefix", ip.attribute("netmask")))},
            {"dhcp", !ip.firstChildElement("dhcp").isNull()},
            {"unsupported",
                    !root.firstChildElement("virtualport").isNull() ||
                            (!mode.isEmpty() && !QStringList{"nat", "route", "open", "bridge"}.contains(mode))}};
}

namespace {
QString bridgeFailure(const QString &bridge) {
    if (!QFileInfo::exists("/sys/class/net/" + bridge + "/bridge")) return "This bridge is not active on the host.";
    if (geteuid() == 0) return {};
    bool helperReady = false;
    for (const QString &path :
            {QString("/usr/lib/qemu/qemu-bridge-helper"), QString("/usr/libexec/qemu-bridge-helper")}) {
        struct stat info{};
        if (stat(path.toUtf8().constData(), &info) == 0 && info.st_uid == 0 && (info.st_mode & S_ISUID) &&
                access(path.toUtf8().constData(), X_OK) == 0)
            helperReady = true;
    }
    if (!helperReady || !DomainConfig::bridgeAllowed(bridge))
        return "Your VMs need permission to join this network. Use Allow VMs on the Networks page; it asks for your "
               "password once.";
    return {};
}
}

QVariantList NetworkCatalog::discover(virConnectPtr session, QString &status) {
    QVariantList options{
            QVariantMap{{"id", "user"}, {"kind", "user"}, {"source", ""}, {"label", "Internet · private to this VM"},
                    {"category", "NAT"}, {"displayName", "Internet"}, {"available", true}, {"reason", ""},
                    {"description", "Reaches the internet through this computer. The VM gets its own private "
                                    "connection, so other VMs can't see it and it can't be reached from outside."}}};
    QSet<QString> bridges;
    auto collect = [&](virConnectPtr conn, bool system) {
        virNetworkPtr *networks = nullptr;
        int count = virConnectListAllNetworks(conn, &networks, 0);
        if (count < 0) return false;
        for (int i = 0; i < count; ++i) {
            char *raw = virNetworkGetXMLDesc(networks[i], 0);
            const QString networkXml = raw ? QString::fromUtf8(raw) : QString{};
            auto data = describe(networkXml);
            QDomDocument marker;
            marker.setContent(networkXml, true);
            if (!data.isEmpty())
                data["managed"] =
                        data["name"].toString().startsWith("omaware-") &&
                        marker.elementsByTagNameNS("https://omaware.org/xmlns/network/1", "managed").count() == 1;
            free(raw);
            if (data["category"] == "Isolated")
                data["isolation"] = Containment::verifyNetwork(networkXml, virNetworkIsActive(networks[i]) == 1);
            if (!data.isEmpty()) {
                const auto bridge = data["bridge"].toString(), name = data["name"].toString();
                if (!bridge.isEmpty()) bridges.insert(bridge);
                QString reason;
                if (data["unsupported"].toBool())
                    reason = "This network needs adapter settings that OmaWare does not support yet.";
                else if (virNetworkIsActive(networks[i]) != 1)
                    reason = "This network is stopped. Start it on the Networks page.";
                else if (system)
                    reason = bridgeFailure(bridge);
                data["needsPermission"] = reason.startsWith("Your VMs need permission");
                const QString kind = system ? "bridge" : "network", source = system ? bridge : name;
                data["id"] = kind + ":" + source;
                data["kind"] = kind;
                data["source"] = source;
                static const QMap<QString, QString> purpose{{"Shared NAT", "internet + VMs"},
                        {"Host-only", "this computer + VMs"}, {"Isolated", "VMs only"}, {"Routed", "routed"},
                        {"Open", "open"}, {"Bridged", "local network"}};
                data["label"] = data["displayName"].toString() + " · " +
                                purpose.value(data["category"].toString(), data["category"].toString().toLower());
                data["available"] = reason.isEmpty();
                data["reason"] = reason;
                options.append(data);
            }
            virNetworkFree(networks[i]);
        }
        free(networks);
        return true;
    };
    if (session) collect(session, false);
    // Discovery only: never merge system VM inventories or mutate host networks.
    auto system = virConnectOpenReadOnly("qemu:///system");
    bool systemRead = system && collect(system, true);
    if (system) virConnectClose(system);
    status = systemRead ? "Existing host networks are listed below. Network creation and physical bridge setup are "
                          "managed on the host."
                        : "Host virtual networks could not be read. Existing Linux bridges are still listed; NAT "
                          "remains available.";
    for (const auto &name : QDir("/sys/class/net").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (bridges.contains(name) || !QFileInfo::exists("/sys/class/net/" + name + "/bridge")) continue;
        const auto reason = bridgeFailure(name);
        options.append(QVariantMap{{"id", "bridge:" + name}, {"kind", "bridge"}, {"source", name},
                {"label", name + " · local network"}, {"displayName", name}, {"category", "Bridged"},
                {"available", reason.isEmpty()}, {"reason", reason},
                {"needsPermission", reason.startsWith("Your VMs need permission")},
                {"description", "An existing bridge on this computer. What VMs reach depends on how it is set up, "
                                "usually your local network."}});
    }
    for (const QString &category : {QString("Bridged"), QString("Host-only"), QString("Isolated")}) {
        bool found = false;
        for (auto option : options)
            if (option.toMap()["category"].toString() == category) found = true;
        if (!found) {
            const QString plain = category == "Bridged"     ? "local network"
                                  : category == "Host-only" ? "this computer + VMs"
                                                            : "VMs only";
            options.append(QVariantMap{{"id", "unavailable:" + category}, {"label", "No “" + plain + "” network yet"},
                    {"category", category}, {"available", false},
                    {"reason", category == "Bridged" ? "No bridge to your local network is set up on this computer."
                                                     : "Create one on the Networks page first."},
                    {"description",
                            category == "Bridged"
                                    ? "Joining your local network needs a bridge set up on this computer; OmaWare "
                                      "doesn't create one."
                                    : "Create a network of this kind on the Networks page, then connect VMs to it."}});
        }
    }
    return options;
}
