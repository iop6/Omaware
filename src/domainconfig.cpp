// SPDX-License-Identifier: GPL-3.0-or-later
#include "domainconfig.h"
#include <QCryptographicHash>
#include <QDomDocument>
#include <QFile>
#include <QSet>
#include <QRegularExpression>
#include <QTextStream>

namespace {
bool parse(const QString &xml, QDomDocument &doc, QString &error) {
    if (xml.size() > 4 * 1024 * 1024 || xml.contains("<!DOCTYPE") || xml.contains("<!ENTITY")) {
        error = "Unsupported or oversized VM XML"; return false;
    }
    if (!doc.setContent(xml, true, &error) || doc.documentElement().tagName() != "domain") {
        if (error.isEmpty()) error = "Invalid VM XML";
        return false;
    }
    return true;
}
QString attr(QDomElement parent, const QString &tag, const QString &key, const QString &fallback = {}) {
    return parent.firstChildElement(tag).attribute(key, fallback);
}
double memoryMiB(QDomElement e) {
    double value = e.text().toDouble();
    const auto unit = e.attribute("unit", "KiB").toLower();
    if (unit == "gib" || unit == "g") return value * 1024;
    if (unit == "mib" || unit == "m") return value;
    if (unit == "bytes" || unit == "b") return value / 1048576;
    if (unit == "kb") return value * 1000 / 1048576;
    if (unit == "mb") return value * 1000000 / 1048576;
    if (unit == "gb") return value * 1000000000 / 1048576;
    return value / 1024;
}
bool editable(QDomElement nic) {
    if (!QStringList{"user", "bridge", "network"}.contains(nic.attribute("type"))) return false;
    if (!QStringList{"virtio", "e1000", "e1000e", "rtl8139"}.contains(attr(nic, "model", "type"))) return false;
    // Preserve advanced adapters instead of silently discarding their policy or backend.
    const QSet<QString> supported = {"mac", "source", "model", "link", "alias", "address", "target", "driver", "rom", "boot"};
    for (auto e = nic.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
        if (!supported.contains(e.tagName())) return false;
    const auto source = nic.firstChildElement("source");
    for (int i = 0; i < source.attributes().count(); ++i)
        if (!QStringList{"bridge", "network"}.contains(source.attributes().item(i).nodeName())) return false;
    return true;
}
QVariantMap interfaceInfo(QDomElement nic) {
    auto source = nic.firstChildElement("source");
    // libvirt can resolve a bridge-backed network to type=bridge in live XML.
    const QString type = source.hasAttribute("network") ? "network" : nic.attribute("type");
    QString src = source.attribute("network", source.attribute("bridge", source.attribute("dev", source.attribute("address"))));
    QString kind = type == "user" ? "Internet · private to this VM" : type == "network" ? "Virtual network" : type == "bridge" ? "Network bridge" : type;
    return {{"mac", attr(nic, "mac", "address")}, {"type", type}, {"kind", kind}, {"source", src},
        {"networkId", type == "user" ? "user" : type + ":" + src},
        {"model", attr(nic, "model", "type", "default")}, {"linkUp", attr(nic, "link", "state", "up") != "down"},
        {"target", attr(nic, "target", "dev")}, {"editable", editable(nic)}};
}
void readAcl(const QString &path, const QString &bridge, QSet<QString> &seen, bool &allow, bool &deny, bool &valid) {
    if (seen.contains(path) || seen.size() >= 16) { valid = false; return; }
    seen.insert(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) { valid = false; return; }
    for (auto raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const auto match = QRegularExpression("^(allow|deny|include)\\s+(.+)$").match(line);
        if (!match.hasMatch()) { valid = false; continue; }
        const auto command = match.captured(1), value = match.captured(2).trimmed();
        if (command == "include") readAcl(value, bridge, seen, allow, deny, valid);
        else if (value == bridge || value == "all") {
            if (command == "allow") allow = true;
            else deny = true;
        }
    }
}
}

QString DomainConfig::revision(const QString &xml) {
    return QString::fromLatin1(QCryptographicHash::hash(xml.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QVariantMap DomainConfig::describe(const QString &xml, QString &error) {
    QDomDocument doc;
    if (!parse(xml, doc, error)) return {};
    const auto root = doc.documentElement(), os = root.firstChildElement("os"), cpu = root.firstChildElement("cpu");
    const auto loader = os.firstChildElement("loader"), topology = cpu.firstChildElement("topology");
    const auto devices = root.firstChildElement("devices");
    QStringList boot;
    for (auto e = os.firstChildElement("boot"); !e.isNull(); e = e.nextSiblingElement("boot")) boot << e.attribute("dev");
    QString firmware = os.attribute("firmware") == "efi" || loader.attribute("type") == "pflash" ? "UEFI" : "BIOS";
    QString secure = "Not reported";
    if (firmware == "BIOS") secure = "Not applicable";
    else if (loader.hasAttribute("secure")) secure = loader.attribute("secure") == "yes" ? "Supported by firmware" : "Disabled";
    auto features = os.firstChildElement("firmware");
    for (auto e = features.firstChildElement("feature"); !e.isNull(); e = e.nextSiblingElement("feature"))
        if (e.attribute("name") == "secure-boot") secure = e.attribute("enabled") == "yes" ? "Enabled in firmware configuration" : "Disabled";
    QString osProfile;
    auto osNodes = root.firstChildElement("metadata").elementsByTagNameNS("http://libosinfo.org/xmlns/libvirt/domain/1.0", "os");
    if (osNodes.count()) osProfile = osNodes.item(0).toElement().attribute("id");
    QVariantMap result{{"name", root.firstChildElement("name").text()}, {"uuid", root.firstChildElement("uuid").text()},
        {"title", root.firstChildElement("title").text()}, {"description", root.firstChildElement("description").text()},
        {"hypervisor", root.attribute("type").toUpper()}, {"architecture", attr(os, "type", "arch")},
        {"machine", attr(os, "type", "machine")}, {"osProfile", osProfile}, {"firmware", firmware},
        {"loader", loader.text()}, {"nvram", os.firstChildElement("nvram").text()}, {"secureBoot", secure},
        {"bootOrder", boot.join(" → ")}, {"cpuMode", cpu.attribute("mode", "Hypervisor default")},
        {"cpuModel", cpu.firstChildElement("model").text()}, {"vcpus", root.firstChildElement("vcpu").text().toInt()},
        {"cpuTopology", topology.isNull() ? QString{} : QString("%1 sockets · %2 cores · %3 threads").arg(topology.attribute("sockets", "1"), topology.attribute("cores", "1"), topology.attribute("threads", "1"))},
        {"memoryMiB", memoryMiB(root.firstChildElement("memory"))},
        {"currentMemoryMiB", memoryMiB(root.firstChildElement("currentMemory").isNull() ? root.firstChildElement("memory") : root.firstChildElement("currentMemory"))},
        {"emulator", devices.firstChildElement("emulator").text()}, {"revision", revision(xml)}};
    QVariantList disks, nics, displays, shares;
    QStringList inputs, sound;
    bool agentConfigured = false, agentConnected = false;
    bool clipboardConfigured = false;
    for (auto e = devices.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        if (e.tagName() == "disk") {
            auto source = e.firstChildElement("source");
            QString location = source.attribute("file", source.attribute("dev", source.attribute("name")));
            if (source.hasAttribute("pool")) location = source.attribute("pool") + "/" + source.attribute("volume");
            if (source.hasAttribute("protocol")) location = source.attribute("protocol") + ":" + location;
            disks.append(QVariantMap{{"device", e.attribute("device", "disk")}, {"type", e.attribute("type")},
                {"target", attr(e, "target", "dev")}, {"bus", attr(e, "target", "bus")}, {"source", location},
                {"format", attr(e, "driver", "type", "Not reported")}, {"readOnly", !e.firstChildElement("readonly").isNull()},
                {"bootOrder", attr(e, "boot", "order")}});
        } else if (e.tagName() == "interface") nics.append(interfaceInfo(e));
        else if (e.tagName() == "graphics") displays.append(QVariantMap{{"type", e.attribute("type").toUpper()}, {"listen", attr(e, "listen", "type", "Not reported")}});
        else if (e.tagName() == "video") {
            result["videoModel"] = attr(e, "model", "type");
            result["videoMemoryKiB"] = attr(e, "model", "vram").toLongLong();
            result["videoHeads"] = attr(e, "model", "heads", "1").toInt();
            result["acceleration3d"] = attr(e.firstChildElement("model"), "acceleration", "accel3d", "no") == "yes";
        } else if (e.tagName() == "channel" && attr(e, "target", "name") == "org.qemu.guest_agent.0") {
            agentConfigured = true; agentConnected = attr(e, "target", "state") == "connected";
        } else if (e.tagName() == "channel" && e.attribute("type") == "qemu-vdagent") clipboardConfigured = attr(e.firstChildElement("source"), "clipboard", "copypaste") == "yes";
        else if (e.tagName() == "input") inputs << e.attribute("type") + " (" + e.attribute("bus") + ")";
        else if (e.tagName() == "sound") sound << e.attribute("model");
        else if (e.tagName() == "filesystem") shares.append(QVariantMap{{"source", attr(e, "source", "dir")}, {"target", attr(e, "target", "dir")}});
    }
    result["disks"] = disks; result["interfaces"] = nics; result["displays"] = displays;
    result["inputs"] = inputs.join(", "); result["sound"] = sound.join(", "); result["shares"] = shares;
    result["agentConfigured"] = agentConfigured; result["agentConnected"] = agentConnected;
    result["clipboardConfigured"] = clipboardConfigured;
    return result;
}
bool DomainConfig::networkDevice(const QString &xml, const QString &mac, const QString &kind,
    const QString &source, const QString &model, bool linkUp, bool remove, QString &deviceXml, QString &error) {
    QDomDocument doc;
    if (!parse(xml, doc, error)) return false;
    auto devices = doc.documentElement().firstChildElement("devices");
    QDomElement nic;
    for (auto e = devices.firstChildElement("interface"); !e.isNull(); e = e.nextSiblingElement("interface")) {
        if (!mac.isEmpty() && attr(e, "mac", "address").compare(mac, Qt::CaseInsensitive) == 0) {
            if (!nic.isNull()) { error = "Ambiguous adapter MAC address"; return false; }
            nic = e;
        }
    }
    if (!mac.isEmpty() && nic.isNull()) { error = "This adapter no longer exists. Refresh Details."; return false; }
    if (remove) {
        if (nic.isNull()) { error = "Select an adapter to remove"; return false; }
    } else {
        if (!QStringList{"user", "bridge", "network"}.contains(kind) ||
            !QStringList{"virtio", "e1000", "e1000e", "rtl8139"}.contains(model)) {
            error = "Unsupported network or adapter model"; return false;
        }
        if (kind != "user" && (source.isEmpty() || source.size() > 128)) { error = "Select a network source"; return false; }
        if (!nic.isNull() && !editable(nic)) { error = "This adapter has advanced settings that OmaWare cannot edit yet."; return false; }
        if (nic.isNull()) nic = doc.createElement("interface");
        const bool changedSource = nic.attribute("type") != kind ||
            attr(nic, "source", kind == "bridge" ? "bridge" : "network") != source;
        nic.setAttribute("type", kind);
        auto ensure = [&](QString name) { auto e = nic.firstChildElement(name); if (e.isNull()) { e = doc.createElement(name); nic.appendChild(e); } return e; };
        if (changedSource) {
            nic.removeChild(nic.firstChildElement("source"));
            nic.removeChild(nic.firstChildElement("target")); // Let libvirt allocate the host tap for the new source.
            if (kind != "user") ensure("source").setAttribute(kind == "bridge" ? "bridge" : "network", source);
        }
        ensure("model").setAttribute("type", model);
        ensure("link").setAttribute("state", linkUp ? "up" : "down");
    }
    QDomDocument device;
    device.appendChild(device.importNode(nic, true));
    deviceXml = device.toString(-1);
    return true;
}
bool DomainConfig::bridgeAllowed(const QString &bridge, const QString &aclPath) {
    QSet<QString> seen;
    bool allow = false, deny = false, valid = true;
    readAcl(aclPath, bridge, seen, allow, deny, valid);
    return valid && allow && !deny;
}
