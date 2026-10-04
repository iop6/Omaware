// SPDX-License-Identifier: GPL-3.0-or-later
#include "labplan.h"
#include "cloudseed.h"
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

QString LabPlan::slug(const QString &name) {
    auto s = name.toLower();
    s.replace(QRegularExpression("[^a-z0-9]+"), "-");
    s.remove(QRegularExpression("^-+|-+$"));
    return s.left(24);
}

namespace {
// Fixed addresses on an isolated network without a given subnet: 172.30.N.0/24, VMs from .10.
QString isolatedSubnet(int index) {
    return QString("172.30.%1.0/24").arg(index + 1);
}

QString text(const QVariant &v) {
    return v.toString().trimmed();
}
}

QVariantMap LabPlan::check(const QVariantMap &plan, const Host &host) {
    QStringList problems, warnings;
    const auto name = text(plan["name"]);
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9 _-]{0,31}$").match(name).hasMatch())
        problems << "Give the lab a name of 1–32 letters, numbers, spaces, dashes or underscores.";
    // The user name may be given as "user" or as {"login": {"user": ...}}.
    auto user = text(plan.value("user", plan.value("login").toMap().value("user", plan.value("login"))));
    if (!CloudSeed::validUser(user))
        problems << "Choose a VM user name of lower-case letters, digits, dashes or underscores (not root, admin or "
                    "the image's default user). Ask the user what they want.";
    const auto labSlug = slug(name);

    // Networks.
    QVariantList networks;
    QHash<QString, QVariantMap> byName;
    int isolatedCount = 0;
    const auto inputNetworks = plan["networks"].toList();
    if (inputNetworks.size() > 8) problems << "A lab can have at most 8 networks.";
    for (const auto &entry : inputNetworks.mid(0, 8)) {
        const auto n = entry.toMap();
        const auto netName = text(n["name"]).toLower();
        if (!QRegularExpression("^[a-z0-9][a-z0-9-]{0,19}$").match(netName).hasMatch()) {
            problems << "Network names must be 1–20 lower-case letters, digits or dashes (got “" + text(n["name"]) +
                                "”).";
            continue;
        }
        if (byName.contains(netName)) {
            problems << "There are two networks called “" + netName + "”.";
            continue;
        }
        const auto type = text(n.value("type", "private")).toLower();
        if (!QStringList{"internet", "private", "isolated"}.contains(type)) {
            problems << "Network “" + netName + "”: type must be internet, private or isolated.";
            continue;
        }
        QVariantMap out{{"name", netName}, {"type", type}, {"fullName", labSlug + "-" + netName},
                {"mode", type == "internet"  ? "nat"
                         : type == "private" ? "hostonly"
                                             : "isolated"}};
        auto subnet = text(n["subnet"]);
        if (!subnet.isEmpty()) {
            const auto parsed = QHostAddress::parseSubnet(subnet);
            bool v4 = false;
            parsed.first.toIPv4Address(&v4);
            if (!v4 || parsed.second < 16 || parsed.second > 28) {
                problems << "Network “" + netName + "”: use an IPv4 subnet like 10.20.0.0/24 (prefix /16 to /28).";
                continue;
            }
            subnet = QHostAddress(parsed.first.toIPv4Address() & (0xffffffffu << (32 - parsed.second))).toString() +
                     "/" + QString::number(parsed.second);
        } else if (type == "isolated")
            subnet = isolatedSubnet(isolatedCount);
        if (type == "isolated") ++isolatedCount;
        out["subnet"] = subnet;
        if (host.existingNetworks.contains(out["fullName"].toString()) ||
                host.existingNetworks.contains("omaware-" + out["fullName"].toString()))
            problems << "A network called “" + out["fullName"].toString() +
                                "” already exists. Rename the lab or the network.";
        byName[netName] = out;
        networks.append(out);
    }
    for (int i = 0; i < networks.size(); ++i)
        for (int j = i + 1; j < networks.size(); ++j) {
            const auto a = QHostAddress::parseSubnet(networks[i].toMap()["subnet"].toString()),
                       b = QHostAddress::parseSubnet(networks[j].toMap()["subnet"].toString());
            if (a.second >= 0 && b.second >= 0 && (a.first.isInSubnet(b) || b.first.isInSubnet(a)))
                problems << "Networks “" + networks[i].toMap()["name"].toString() + "” and “" +
                                    networks[j].toMap()["name"].toString() + "” have overlapping subnets.";
        }

    // VMs.
    QVariantList vms;
    QSet<QString> vmNames;
    QHash<QString, QSet<QString>> usedIps;
    QHash<QString, int> nextHost;
    const auto inputVms = plan["vms"].toList();
    if (inputVms.isEmpty()) problems << "Add at least one VM.";
    if (inputVms.size() > 12) problems << "A lab can have at most 12 VMs.";
    qint64 memoryTotal = 0;
    // Addresses given in the plan are reserved first, so automatic ones never collide with them.
    QHash<QString, QHash<QString, int>> given;
    for (const auto &entry : inputVms)
        for (const auto &link : entry.toMap()["networks"].toList())
            if (link.typeId() == QMetaType::QVariantMap && !text(link.toMap()["ip"]).isEmpty()) {
                const auto net = text(link.toMap()["network"]).toLower(),
                           ip = text(link.toMap()["ip"]).section('/', 0, 0);
                usedIps[net].insert(ip);
                ++given[net][ip];
            }
    for (const auto &entry : inputVms.mid(0, 12)) {
        const auto v = entry.toMap();
        const auto vmName = text(v["name"]);
        QStringList own;
        if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9-]{0,30}$").match(vmName).hasMatch()) {
            problems << "VM names must be 1–31 letters, digits or dashes (got “" + vmName + "”).";
            continue;
        }
        if (vmNames.contains(vmName.toLower())) {
            problems << "There are two VMs called “" + vmName + "”.";
            continue;
        }
        vmNames.insert(vmName.toLower());
        if (host.existingVms.contains(vmName.toLower()))
            own << "a VM with this name already exists; choose another name";
        const auto os = text(v.value("os", "ubuntu")).toLower();
        if (!host.images.contains(os)) own << "os must be one of " + host.images.join(", ");
        const int cpus = v.value("cpus", 2).toInt(), memory = v.value("memory_mib", v.value("memoryMiB", 2048)).toInt(),
                  disk = v.value("disk_gib", v.value("diskGiB", 16)).toInt();
        if (cpus < 1 || cpus > std::min(16, std::max(1, host.cpus)))
            own << QString("cpus must be 1–%1").arg(std::min(16, std::max(1, host.cpus)));
        if (memory < 512 || memory > 65536 || (host.memoryMiB > 0 && memory > host.memoryMiB))
            own << "memory_mib must be 512–65536 and fit this computer";
        if (disk < 8 || disk > 512) own << "disk_gib must be 8–512";
        memoryTotal += memory;
        QVariantList nics;
        bool internet = false, reachable = false;
        QSet<QString> joined;
        const auto links = v["networks"].toList();
        if (links.size() > 4) own << "a VM can join at most 4 networks";
        for (const auto &link : links.mid(0, 4)) {
            const auto l = link.typeId() == QMetaType::QVariantMap ? link.toMap() : QVariantMap{{"network", link}};
            const auto netName = text(l["network"]).toLower();
            if (!byName.contains(netName)) {
                own << "network “" + netName + "” isn't in the plan";
                continue;
            }
            if (joined.contains(netName)) {
                own << "joins “" + netName + "” twice";
                continue;
            }
            joined.insert(netName);
            const auto net = byName[netName];
            const auto type = net["type"].toString();
            internet |= type == "internet";
            reachable |= type != "isolated";
            auto ip = text(l["ip"]).section('/', 0, 0);
            const auto subnet = QHostAddress::parseSubnet(net["subnet"].toString());
            if (!ip.isEmpty()) {
                QHostAddress address(ip);
                bool v4 = false;
                const quint32 value = address.toIPv4Address(&v4);
                if (subnet.second < 0) {
                    own << "give network “" + netName + "” a subnet to use fixed addresses on it";
                    continue;
                }
                const quint32 base = subnet.first.toIPv4Address(), last = base | ~(0xffffffffu << (32 - subnet.second));
                if (!v4 || !address.isInSubnet(subnet) || value <= base + 1 || value >= last) {
                    own << ip + " isn't a usable address in " + net["subnet"].toString() +
                                    " (the first address is the network's own)";
                    continue;
                }
                // On networks with DHCP, .100–.200 of a /24 is handed out automatically.
                if (type != "isolated" && subnet.second == 24 && (value & 0xff) >= 100 && (value & 0xff) <= 200) {
                    own << ip + " is in the automatic range .100–.200; pick an address outside it";
                    continue;
                }
            } else if (type == "isolated") {
                // Isolated networks have no DHCP, so every VM gets a fixed address.
                const quint32 base = subnet.first.toIPv4Address();
                int &n = nextHost[netName];
                if (n == 0) n = 10;
                while (usedIps[netName].contains(QHostAddress(base + quint32(n)).toString()))
                    ++n;
                ip = QHostAddress(base + quint32(n++)).toString();
            }
            if (!ip.isEmpty()) {
                if (given[netName].value(ip) > 1) own << ip + " is given to more than one VM on “" + netName + "”";
                usedIps[netName].insert(ip);
                ip += "/" + QString::number(subnet.second);
            }
            nics.append(QVariantMap{{"network", netName}, {"ip", ip}});
        }
        QStringList packages;
        for (const auto &p : v["packages"].toList()) {
            const auto package = text(p);
            if (!QRegularExpression("^[a-z0-9][a-z0-9+.-]{0,63}$").match(package).hasMatch())
                own << "package “" + package + "” isn't a valid package name";
            else
                packages << package;
        }
        if (packages.size() > 30) own << "at most 30 packages";
        if (!packages.isEmpty() && !internet) own << "installing packages needs a network of type internet";
        QStringList setup;
        for (const auto &c : v["setup"].toList())
            setup << c.toString();
        if (setup.size() > 20 || std::any_of(setup.begin(), setup.end(),
                                         [](const QString &c) { return c.size() > 4000 || c.trimmed().isEmpty(); }))
            own << "setup takes up to 20 non-empty commands of up to 4000 characters";
        for (const auto &p : own)
            problems << vmName + ": " + p;
        if (!reachable)
            warnings << vmName + (nics.isEmpty() ? " has no network" : " is only on isolated networks") +
                                ", so OmaWare can't run commands in it; use its screen instead.";
        vms.append(QVariantMap{{"name", vmName}, {"os", os}, {"cpus", cpus}, {"memoryMiB", memory}, {"diskGiB", disk},
                {"nics", nics}, {"packages", packages}, {"setup", setup}, {"internet", internet},
                {"reachable", reachable}});
    }
    if (host.memoryMiB > 0 && memoryTotal > host.memoryMiB * 8 / 10)
        warnings << QString("Together the VMs use %1 GiB of memory, most of this computer's %2 GiB; running them all "
                            "at once may be slow.")
                            .arg(memoryTotal / 1024.0, 0, 'f', 1)
                            .arg(host.memoryMiB / 1024.0, 0, 'f', 1);
    const QVariantMap normalized{
            {"name", name}, {"slug", labSlug}, {"user", user}, {"networks", networks}, {"vms", vms}};
    return {{"ok", problems.isEmpty()}, {"problems", problems}, {"warnings", warnings}, {"plan", normalized}};
}
