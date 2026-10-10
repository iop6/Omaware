// SPDX-License-Identifier: GPL-3.0-or-later
#include "configuration.h"
#include "domainconfig.h"
#include <QDomDocument>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <QRegularExpression>

namespace {
QDomElement ensure(QDomDocument &doc, QDomElement parent, QString name) {
    auto e = parent.firstChildElement(name);
    if (e.isNull()) {
        e = doc.createElement(name);
        parent.appendChild(e);
    }
    return e;
}

void text(QDomDocument &doc, QDomElement element, QString value) {
    while (!element.firstChild().isNull())
        element.removeChild(element.firstChild());
    element.appendChild(doc.createTextNode(value));
}

QString deviceKey(QDomElement e) {
    if (e.tagName() == "interface") return "nic:" + e.firstChildElement("mac").attribute("address");
    if (e.tagName() == "disk") return "disk:" + e.firstChildElement("target").attribute("dev");
    if (e.tagName() == "channel" && e.attribute("type") == "qemu-vdagent") return "clipboard";
    if (e.tagName() == "video") return "display";
    return {};
}

QVariantMap summaries(const QString &xml) {
    QString error;
    auto info = DomainConfig::describe(xml, error);
    if (!error.isEmpty()) return {};
    QVariantMap out{
            {"cpu", QString("%1 vCPUs · %2 · %3")
                            .arg(info["vcpus"].toString(), info["cpuMode"].toString(), info["cpuTopology"].toString())},
            {"memory", QString("%1 MiB").arg(info["currentMemoryMiB"].toDouble())}, {"boot", info["bootOrder"]}};
    for (const auto &v : info["interfaces"].toList()) {
        const auto n = v.toMap();
        out["nic:" + n["mac"].toString()] = n["kind"].toString() + " " + n["source"].toString() + " · " +
                                            n["model"].toString() +
                                            (n["linkUp"].toBool() ? " · connected" : " · disconnected");
    }
    for (const auto &v : info["disks"].toList()) {
        const auto d = v.toMap();
        out["disk:" + d["target"].toString()] =
                d["source"].toString() + " · " + d["format"].toString() + " · " + d["bus"].toString();
    }
    if (const auto adapter = info["videoModel"].toString(); !adapter.isEmpty()) {
        const auto resolution = info["videoResolution"].toString();
        out["display"] = adapter + " · " + (resolution.isEmpty() ? QString("default screen size") : resolution);
    }
    QDomDocument doc;
    doc.setContent(xml);
    const auto channels = doc.elementsByTagName("channel");
    out["clipboard"] = "Disabled";
    for (int i = 0; i < channels.count(); ++i) {
        auto c = channels.at(i).toElement();
        if (c.attribute("type") == "qemu-vdagent" &&
                c.firstChildElement("source").firstChildElement("clipboard").attribute("copypaste") == "yes")
            out["clipboard"] = "Enabled";
    }
    return out;
}
}

namespace {
// Sets the processor count and CPU mode, with all processors as cores of one socket.
bool setProcessors(QDomDocument &doc, const QVariantMap &before, int cpus, const QString &mode, QString &error) {
    auto root = doc.documentElement();
    auto cpu = root.firstChildElement("cpu");
    if (cpus == before["vcpus"].toInt() && mode == before["cpuMode"].toString()) return true;
    if (!root.firstChildElement("vcpus").isNull() || !cpu.firstChildElement("numa").isNull() ||
            !root.firstChildElement("cputune").isNull()) {
        error = "This VM uses advanced CPU pinning, NUMA or hotplug. Keep its CPU configuration unchanged.";
        return false;
    }
    auto vcpu = ensure(doc, root, "vcpu");
    text(doc, vcpu, QString::number(cpus));
    vcpu.removeAttribute("current");
    if (mode != before["cpuMode"].toString()) {
        if (!QStringList{"host-model", "host-passthrough"}.contains(mode)) {
            error = "Choose a supported CPU mode.";
            return false;
        }
        if (!cpu.isNull()) root.removeChild(cpu);
        cpu = ensure(doc, root, "cpu");
        cpu.setAttribute("mode", mode);
    }
    if (!cpu.isNull()) {
        cpu.removeChild(cpu.firstChildElement("topology"));
        auto topology = ensure(doc, cpu, "topology");
        topology.setAttribute("sockets", "1");
        topology.setAttribute("cores", QString::number(cpus));
        topology.setAttribute("threads", "1");
    }
    return true;
}

bool setMemory(QDomDocument &doc, const QVariantMap &before, int memory, QString &error) {
    auto root = doc.documentElement();
    if (memory == before["currentMemoryMiB"].toInt()) return true;
    if (!root.firstChildElement("maxMemory").isNull() ||
            !root.firstChildElement("cpu").firstChildElement("numa").isNull() ||
            !root.firstChildElement("devices").firstChildElement("memory").isNull()) {
        error = "This VM uses memory hotplug or NUMA. Keep its memory configuration unchanged.";
        return false;
    }
    for (auto tag : {"memory", "currentMemory"}) {
        auto e = ensure(doc, root, tag);
        e.setAttribute("unit", "MiB");
        text(doc, e, QString::number(memory));
    }
    return true;
}

// `boot` is a comma-separated list of devices ("cdrom,hd"); it replaces every other boot setting.
bool setBootOrder(QDomDocument &doc, const QString &boot, QString &error) {
    if (!QStringList{"hd", "cdrom,hd", "hd,cdrom", "network,hd"}.contains(boot)) {
        error = "Choose a supported boot order.";
        return false;
    }
    auto os = doc.documentElement().firstChildElement("os");
    const auto devices = doc.documentElement().firstChildElement("devices");
    while (!os.firstChildElement("boot").isNull())
        os.removeChild(os.firstChildElement("boot"));
    for (auto e = devices.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
        e.removeChild(e.firstChildElement("boot"));
    for (const auto &dev : boot.split(',')) {
        auto e = doc.createElement("boot");
        e.setAttribute("dev", dev);
        os.appendChild(e);
    }
    return true;
}

// Puts an ISO in the first optical drive (adding a SATA drive if there's none), or ejects it when `path` is
// empty.
bool setInstallationMedia(QDomDocument &doc, const QString &path, QString &error) {
    if (!path.isEmpty() &&
            (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile() || !QFileInfo(path).isReadable())) {
        error = "The installation ISO cannot be read. Choose an existing local file.";
        return false;
    }
    auto devices = doc.documentElement().firstChildElement("devices");
    QDomElement cdrom;
    for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk"))
        if (d.attribute("device") == "cdrom") {
            cdrom = d;
            break;
        }
    if (cdrom.isNull() && !path.isEmpty()) {
        cdrom = doc.createElement("disk");
        cdrom.setAttribute("device", "cdrom");
        cdrom.setAttribute("type", "file");
        devices.appendChild(cdrom);
        auto driver = ensure(doc, cdrom, "driver");
        driver.setAttribute("name", "qemu");
        driver.setAttribute("type", "raw");
        QSet<QString> targets;
        for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk"))
            targets.insert(d.firstChildElement("target").attribute("dev"));
        QString target;
        for (char c = 'a'; c <= 'z' && target.isEmpty(); ++c)
            if (!targets.contains("sd" + QString(QChar(c)))) target = "sd" + QString(QChar(c));
        if (target.isEmpty()) {
            error = "No free optical drive target is available.";
            return false;
        }
        auto t = ensure(doc, cdrom, "target");
        t.setAttribute("dev", target);
        t.setAttribute("bus", "sata");
        ensure(doc, cdrom, "readonly");
    }
    if (!cdrom.isNull()) {
        cdrom.removeChild(cdrom.firstChildElement("source"));
        if (!path.isEmpty()) ensure(doc, cdrom, "source").setAttribute("file", path);
    }
    return true;
}

// Clipboard sharing through QEMU's own vdagent channel (spice-vdagent in the guest talks to it).
bool setClipboard(QDomDocument &doc, bool on, QString &error) {
    auto devices = doc.documentElement().firstChildElement("devices");
    QDomElement agent;
    for (auto c = devices.firstChildElement("channel"); !c.isNull(); c = c.nextSiblingElement("channel")) {
        if (c.firstChildElement("target").attribute("name") != "com.redhat.spice.0") continue;
        if (c.attribute("type") != "qemu-vdagent") {
            error = "This VM already has a SPICE channel. Its integration settings must be kept.";
            return false;
        }
        agent = c;
    }
    if (on) {
        if (agent.isNull()) {
            agent = doc.createElement("channel");
            agent.setAttribute("type", "qemu-vdagent");
            devices.appendChild(agent);
        }
        auto target = ensure(doc, agent, "target");
        target.setAttribute("type", "virtio");
        target.setAttribute("name", "com.redhat.spice.0");
        ensure(doc, ensure(doc, agent, "source"), "clipboard").setAttribute("copypaste", "yes");
    } else if (!agent.isNull()) {
        ensure(doc, ensure(doc, agent, "source"), "clipboard").setAttribute("copypaste", "no");
    }
    return true;
}
}

// The screen size the display adapter advertises as preferred ("1920x1080"; empty for QEMU's default). The firmware
// boots at that size, and a guest without a display driver (Windows on QEMU's adapters) stays at it.
bool setResolution(QDomDocument &doc, const QString &value, QString &error) {
    auto model = doc.documentElement().firstChildElement("devices").firstChildElement("video").firstChildElement("model");
    if (model.isNull()) {
        error = "This VM has no display adapter to set a screen size on.";
        return false;
    }
    if (value.isEmpty()) {
        model.removeChild(model.firstChildElement("resolution"));
        return true;
    }
    const auto match = QRegularExpression("^(\\d{3,4})x(\\d{3,4})$").match(value);
    const int x = match.captured(1).toInt(), y = match.captured(2).toInt();
    if (!match.hasMatch() || x < 640 || x > 7680 || y < 480 || y > 4320) {
        error = "Choose a screen size between 640x480 and 7680x4320, written as WIDTHxHEIGHT.";
        return false;
    }
    auto resolution = ensure(doc, model, "resolution");
    resolution.setAttribute("x", x);
    resolution.setAttribute("y", y);
    return true;
}

QString Configuration::hardware(const QString &xml, const QVariantMap &values, QString &error) {
    QDomDocument doc;
    if (!doc.setContent(xml)) {
        error = "Could not parse saved configuration.";
        return {};
    }
    QString parseError;
    const auto before = DomainConfig::describe(xml, parseError);
    const int cpus = values.value("cpus", before["vcpus"]).toInt();
    const int memory = values.value("memoryMiB", before["currentMemoryMiB"]).toInt();
    if (cpus < 1 || cpus > 256 || memory < 256 || memory > 1048576) {
        error = "Choose 1–256 processors and 256–1048576 MiB RAM.";
        return {};
    }
    const auto mode = values.value("cpuMode", before["cpuMode"]).toString();
    if (!setProcessors(doc, before, cpus, mode, error) || !setMemory(doc, before, memory, error)) return {};
    if (values.contains("boot") && !setBootOrder(doc, values["boot"].toString(), error)) return {};
    if (values.contains("iso") && !setInstallationMedia(doc, values["iso"].toString(), error)) return {};
    if (values.contains("clipboard") && !setClipboard(doc, values["clipboard"].toBool(), error)) return {};
    if (values.contains("resolution") && !setResolution(doc, values["resolution"].toString(), error)) return {};
    return doc.toString(-1);
}

QVariantList Configuration::changes(const QString &before, const QString &after) {
    const auto a = summaries(before), b = summaries(after);
    QSet<QString> keys;
    for (auto i = a.begin(); i != a.end(); ++i)
        keys.insert(i.key());
    for (auto i = b.begin(); i != b.end(); ++i)
        keys.insert(i.key());
    QStringList ordered(keys.begin(), keys.end());
    ordered.sort();
    QVariantList result;
    for (const auto &key : ordered)
        if (a.value(key) != b.value(key)) {
            QString label = key == "cpu"             ? "Processors"
                            : key == "memory"        ? "Memory"
                            : key == "boot"          ? "Boot order"
                            : key == "clipboard"     ? "Clipboard integration"
                            : key == "display"       ? "Display"
                            : key.startsWith("nic:") ? "Adapter " + key.mid(4)
                                                     : "Disk " + key.mid(5);
            result.append(QVariantMap{{"key", key}, {"label", label}, {"before", a.value(key, "Not attached")},
                    {"after", b.value(key, "Not attached")}});
        }
    return result;
}

QString Configuration::revert(const QString &baseline, const QString &current, const QString &key, QString &error) {
    QDomDocument original, doc;
    if (!original.setContent(baseline) || !doc.setContent(current)) {
        error = "Pending configuration could not be read.";
        return {};
    }
    auto root = doc.documentElement(), base = original.documentElement();
    auto restore = [&](QDomElement from, QDomElement to, QString tag) {
        while (!to.firstChildElement(tag).isNull())
            to.removeChild(to.firstChildElement(tag));
        for (auto e = from.firstChildElement(tag); !e.isNull(); e = e.nextSiblingElement(tag))
            to.appendChild(doc.importNode(e, true));
    };
    if (key == "cpu") {
        restore(base, root, "vcpu");
        restore(base, root, "cpu");
    } else if (key == "memory") {
        restore(base, root, "memory");
        restore(base, root, "currentMemory");
    } else if (key == "boot") {
        restore(base.firstChildElement("os"), root.firstChildElement("os"), "boot");
        for (auto e = root.firstChildElement("devices").firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            e.removeChild(e.firstChildElement("boot"));
            for (auto old = base.firstChildElement("devices").firstChildElement(); !old.isNull();
                    old = old.nextSiblingElement())
                if (!deviceKey(e).isEmpty() && deviceKey(old) == deviceKey(e)) restore(old, e, "boot");
        }
    } else if (key.startsWith("nic:") || key.startsWith("disk:") || key == "clipboard" || key == "display") {
        auto devices = root.firstChildElement("devices");
        for (auto e = devices.firstChildElement(); !e.isNull();) {
            auto next = e.nextSiblingElement();
            if (deviceKey(e) == key) devices.removeChild(e);
            e = next;
        }
        for (auto e = base.firstChildElement("devices").firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
            if (deviceKey(e) == key) devices.appendChild(doc.importNode(e, true));
    } else {
        error = "Select a pending change to discard.";
        return {};
    }
    return doc.toString(-1);
}

QString Configuration::networkXml(const QVariantMap &v, const QString &uuid, const QString &bridge, QString &error) {
    const auto name = v["name"].toString().trimmed(), mode = v["mode"].toString();
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9 _-]{0,47}$").match(name).hasMatch()) {
        error = "Use a network name of 1–48 letters, numbers, spaces, dashes or underscores.";
        return {};
    }
    if (!QStringList{"nat", "hostonly", "isolated"}.contains(mode)) {
        error = "Choose NAT, host-only or isolated.";
        return {};
    }
    QDomDocument doc;
    auto root = doc.createElement("network");
    doc.appendChild(root);
    // Isolated switches must not give the host an IPv6 link-local address that guests could reach.
    if (mode != "isolated") root.setAttribute("ipv6", "yes");
    text(doc, ensure(doc, root, "name"), "omaware-" + name.toLower().replace(' ', '-'));
    text(doc, ensure(doc, root, "uuid"), uuid);
    text(doc, ensure(doc, root, "title"), name);
    auto marker = doc.createElementNS("https://omaware.org/xmlns/network/1", "omaware:managed");
    ensure(doc, root, "metadata").appendChild(marker);
    auto br = ensure(doc, root, "bridge");
    br.setAttribute("name", bridge);
    br.setAttribute("stp", "on");
    br.setAttribute("delay", "0");
    if (mode == "nat") ensure(doc, root, "forward").setAttribute("mode", "nat");
    if (mode != "isolated") {
        auto subnet = QHostAddress::parseSubnet(v["subnet"].toString());
        bool good = false;
        quint32 address = subnet.first.toIPv4Address(&good);
        const int prefix = subnet.second;
        if (!good || prefix < 16 || prefix > 29 || (address >> 24) == 127 || (address >> 28) >= 14 || address == 0) {
            error = "Enter an IPv4 subnet with a /16–/29 prefix, for example 192.168.80.0/24.";
            return {};
        }
        quint32 mask = 0xffffffffu << (32 - prefix), baseIp = address & mask, last = baseIp | ~mask;
        auto ip = ensure(doc, root, "ip");
        ip.setAttribute("address", QHostAddress(baseIp + 1).toString());
        ip.setAttribute("prefix", prefix);
        if (v.value("dhcp", true).toBool()) {
            bool ok1, ok2;
            auto first = QHostAddress(v["dhcpStart"].toString()).toIPv4Address(&ok1),
                 end = QHostAddress(v["dhcpEnd"].toString()).toIPv4Address(&ok2);
            if (!ok1 || !ok2 || first <= baseIp + 1 || end >= last || end < first) {
                error = "The DHCP range must be inside the subnet and exclude its gateway, network and broadcast "
                        "addresses.";
                return {};
            }
            auto range = ensure(doc, ensure(doc, ip, "dhcp"), "range");
            range.setAttribute("start", QHostAddress(first).toString());
            range.setAttribute("end", QHostAddress(end).toString());
        }
    } else
        ensure(doc, root, "dns").setAttribute("enable", "no");
    return doc.toString(-1);
}

PendingChanges::PendingChanges(QString uuid) {
    if (QUuid(uuid).isNull()) return;
    path_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pending/" + uuid + ".json";
    QFile file(path_);
    if (file.open(QIODevice::ReadOnly) && file.size() < 8 * 1024 * 1024)
        data_ = QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
}

QString PendingChanges::baseline() const {
    return data_["baseline"].toString();
}

bool PendingChanges::matches(const QString &xml) const {
    return data_.isEmpty() || data_["revision"].toString() == DomainConfig::revision(xml);
}

bool PendingChanges::prepare(const QString &xml, QString &error) {
    if (!matches(xml)) {
        error = "The saved VM configuration changed outside this pending-change session. Refresh and reconcile it "
                "before editing.";
        return false;
    }
    if (data_.isEmpty()) {
        data_["baseline"] = xml;
        data_["revision"] = DomainConfig::revision(xml);
        return write(error);
    }
    return true;
}

bool PendingChanges::saved(const QString &xml, QString &error) {
    data_["revision"] = DomainConfig::revision(xml);
    return write(error);
}

bool PendingChanges::rebase(const QString &baseline, const QString &xml, QString &error) {
    data_["baseline"] = baseline;
    data_["revision"] = DomainConfig::revision(xml);
    return write(error);
}

QVariantList PendingChanges::items(const QString &xml) const {
    return baseline().isEmpty() ? QVariantList{} : Configuration::changes(baseline(), xml);
}

void PendingChanges::clear() {
    if (!path_.isEmpty()) QFile::remove(path_);
    data_.clear();
}

bool PendingChanges::write(QString &error) {
    if (path_.isEmpty() || !QDir().mkpath(QFileInfo(path_).absolutePath())) {
        error = "Cannot save the pending-change record.";
        return false;
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            file.write(QJsonDocument::fromVariant(data_).toJson()) < 0 || !file.commit()) {
        error = "Cannot save the pending-change record: " + file.errorString();
        return false;
    }
    return true;
}
