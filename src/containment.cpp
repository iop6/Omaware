// SPDX-License-Identifier: GPL-3.0-or-later
#include "containment.h"
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <libvirt/libvirt.h>

namespace {
QDomDocument parse(const QString &xml) {
    QDomDocument doc;
    doc.setContent(xml, true);
    return doc;
}

QString readFirstLine(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readLine()).trimmed() : QString{};
}
}

bool Containment::enabled(const QString &domainXml) {
    return parse(domainXml).elementsByTagNameNS(ns, "containment").count() > 0;
}

QString Containment::withMarker(const QString &domainXml, bool on) {
    QDomDocument doc;
    if (!doc.setContent(domainXml, true)) return domainXml;
    auto root = doc.documentElement();
    auto existing = doc.elementsByTagNameNS(ns, "containment");
    while (existing.count() > 0) {
        auto node = existing.at(0);
        node.parentNode().removeChild(node);
    }
    if (on) {
        auto metadata = root.firstChildElement("metadata");
        if (metadata.isNull()) metadata = root.appendChild(doc.createElement("metadata")).toElement();
        auto marker = doc.createElementNS(ns, "containment:containment");
        marker.setAttribute("enabled", "yes");
        metadata.appendChild(marker);
    }
    return doc.toString(-1);
}

QVariantMap Containment::verifyNetwork(const QString &networkXml, bool active, const QString &sysRoot) {
    const auto doc = parse(networkXml);
    const auto root = doc.documentElement();
    const auto bridge = root.firstChildElement("bridge").attribute("name"),
               name = root.firstChildElement("name").text();
    QVariantList checks;
    bool all = true;
    auto add = [&](const QString &label, bool ok, const QString &detail) {
        checks.append(QVariantMap{{"label", label}, {"ok", ok}, {"detail", detail}});
        all = all && ok;
    };
    add("No forwarding, NAT or routing", root.firstChildElement("forward").isNull(),
            "The definition must have no <forward> element.");
    add("No host address, DHCP or DNS on the switch", root.elementsByTagName("ip").isEmpty(),
            "Any <ip> element gives the host an address guests can reach.");
    add("IPv6 disabled on the host side", root.attribute("ipv6") != "yes",
            "ipv6='yes' leaves an IPv6 link-local address on the host bridge.");
    add("Switch is running", active, "Start the switch to verify its live state.");
    if (active && !bridge.isEmpty()) {
        const auto base = QDir(sysRoot).filePath("sys/class/net/" + bridge);
        add("Host bridge exists", QFileInfo::exists(base + "/bridge"), "libvirt should have created " + bridge + ".");
        const auto iface = sysRoot == "/" ? QNetworkInterface::interfaceFromName(bridge) : QNetworkInterface{};
        add("Host has no IP address on the switch", iface.addressEntries().isEmpty(),
                "Guests must not be able to address the host.");
        const auto v6 = QDir(sysRoot).filePath("proc/sys/net/ipv6/conf/" + bridge + "/disable_ipv6");
        add("Host IPv6 stack off for this bridge", !QFileInfo::exists(v6) || readFirstLine(v6) == "1",
                "Without it the host answers IPv6 link-local traffic.");
        QStringList foreign;
        for (const auto &port : QDir(base + "/brif").entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System))
            if (!QFileInfo::exists(QDir(sysRoot).filePath("sys/class/net/" + port + "/tun_flags"))) foreign << port;
        add("Only VM ports on the switch", foreign.isEmpty(),
                foreign.isEmpty() ? "No physical or host adapter is joined."
                                  : "Non-VM interfaces joined: " + foreign.join(", "));
    }
    return {{"isolated", all}, {"bridge", bridge}, {"name", name}, {"checks", checks},
            {"note", "Forwarding out of isolated switches is also rejected by libvirt's firewall rules; reading them "
                     "needs administrator rights."}};
}

QHash<QString, QVariantMap> Containment::hostBridges() {
    QHash<QString, QVariantMap> result;
    auto system = virConnectOpenReadOnly("qemu:///system");
    if (!system) return result;
    virNetworkPtr *networks = nullptr;
    const int count = virConnectListAllNetworks(system, &networks, 0);
    for (int i = 0; i < count; ++i) {
        char *raw = virNetworkGetXMLDesc(networks[i], 0);
        if (raw) {
            auto v = verifyNetwork(QString::fromUtf8(raw), virNetworkIsActive(networks[i]) == 1);
            if (!v["bridge"].toString().isEmpty()) result[v["bridge"].toString()] = v;
        }
        free(raw);
        virNetworkFree(networks[i]);
    }
    free(networks);
    virConnectClose(system);
    return result;
}

namespace {
// libvirt's namespace for raw QEMU arguments and property overrides, which bypass every device rule.
const QString qemuNs = "http://libvirt.org/schemas/domain/qemu/1.0";

QString adapterProblem(const QDomElement &nic, const QHash<QString, QVariantMap> &bridges) {
    const auto mac = nic.firstChildElement("mac").attribute("address"), type = nic.attribute("type");
    const auto label = "Adapter " + (mac.isEmpty() ? QString("without MAC") : mac);
    if (type == "user")
        return label + " uses a private internet connection, which reaches the internet through this computer.";
    if (type != "bridge")
        return label + " uses a " + (type.isEmpty() ? QString("custom") : type) +
               " connection; only verified isolated switches are allowed.";
    const auto bridge = nic.firstChildElement("source").attribute("bridge");
    const auto verdict = bridges.value(bridge);
    if (verdict.isEmpty())
        return label + " uses bridge " + bridge + ", which is not an OmaWare-verifiable isolated switch.";
    if (!verdict["isolated"].toBool())
        return label + " uses " + verdict["name"].toString() + ", which failed isolation checks.";
    return {};
}

// A disk or optical drive may only read the guest's own image file: not network storage, a host block or NVMe
// device, a host folder presented as a disk, or SCSI passthrough.
QString diskProblem(const QDomElement &disk) {
    const auto target = disk.firstChildElement("target").attribute("dev");
    const auto source = disk.firstChildElement("source");
    if (disk.attribute("device") == "lun") return "Disk " + target + " passes a host SCSI device through to the guest.";
    if (source.isNull() || !source.hasAttributes()) return {}; // an empty drive
    const auto type = disk.attribute("type", "file");
    if (type == "file") return {};
    return "Disk " + target + " uses a " + type + " source; contained VMs may only use local image files.";
}

// Serial ports, consoles and channels: a terminal, a log file or a socket libvirt creates are fine; anything that
// reaches a host device, pipe, existing host service or the network is not.
void checkCharacterDevice(const QDomElement &device, QStringList &violations, QStringList &warnings) {
    const auto tag = device.tagName(), type = device.attribute("type");
    const auto source = device.firstChildElement("source");
    const auto target = device.firstChildElement("target").attribute("name");
    if (type == "spicevmc" ||
            (type == "qemu-vdagent" && source.firstChildElement("clipboard").attribute("copypaste") == "yes"))
        violations << "Clipboard sharing lets guest data reach the host desktop.";
    else if (type == "tcp" || type == "udp")
        violations << "A " + tag + " device is exposed on a network socket.";
    else if (type == "unix" && source.attribute("mode") == "connect")
        violations << "A " + tag + " device connects to a service on this computer (" + source.attribute("path") + ").";
    else if (target == "org.qemu.guest_agent.0")
        warnings << "The QEMU guest agent is configured; the host reads guest-controlled replies through it.";
    else if (type == "unix")
        warnings << "A " + tag + " device uses a host socket.";
    else if (!QStringList{"pty", "null", "file", "vc", "qemu-vdagent"}.contains(type))
        violations << "A " + tag + " device uses a " + type + " backend, which reaches a host device or pipe.";
}

// Whether a console accepts connections from the network. VNC and SPICE without a listen element listen on
// TCP (127.0.0.1 by default); only no listener or a local socket is safe.
bool listensOnNetwork(const QDomElement &graphics) {
    if (!QStringList{"vnc", "spice", "rdp"}.contains(graphics.attribute("type"))) return false;
    if (graphics.hasAttribute("socket")) return false;
    const auto listen = graphics.firstChildElement("listen");
    return listen.isNull() || !QStringList{"none", "socket"}.contains(listen.attribute("type"));
}
}

QVariantMap Containment::check(const QString &domainXml, const QHash<QString, QVariantMap> &bridges) {
    QStringList violations, warnings;
    const auto doc = parse(domainXml);
    const auto root = doc.documentElement();
    for (auto e = root.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
        if (e.namespaceURI() == qemuNs)
            violations << "Raw QEMU settings (" + e.localName() +
                                  ") can add devices and connections OmaWare can't check.";
    const auto devices = root.firstChildElement("devices");
    for (auto e = devices.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        const auto tag = e.tagName(), type = e.attribute("type");
        if (tag == "interface") {
            if (const auto problem = adapterProblem(e, bridges); !problem.isEmpty()) violations << problem;
        } else if (tag == "disk") {
            if (const auto problem = diskProblem(e); !problem.isEmpty()) violations << problem;
        } else if (tag == "filesystem")
            violations << "Shared folder " + e.firstChildElement("target").attribute("dir") + " exposes host files.";
        else if (tag == "hostdev")
            violations << "Device passthrough gives the guest direct access to host hardware.";
        else if (tag == "redirdev")
            violations << "USB redirection connects host USB devices to the guest.";
        else if (tag == "smartcard")
            violations << "A smartcard device gives the guest this computer's card reader or certificates.";
        else if (tag == "shmem")
            violations << "Shared memory with the host is configured.";
        else if (tag == "vsock")
            violations << "A vsock device opens a direct socket channel between the guest and this computer.";
        else if (tag == "input" && (type == "passthrough" || type == "evdev"))
            violations << "Input passthrough gives the guest this computer's keyboard or other input device.";
        else if (tag == "audio" && !QStringList{"none", "spice"}.contains(type))
            violations << "Audio goes to this computer's sound system, which can include its microphone.";
        else if (tag == "rng" && e.firstChildElement("backend").attribute("model") == "egd")
            violations << "The random number device reads from a service outside the VM.";
        else if (tag == "tpm" && e.firstChildElement("backend").attribute("type") == "passthrough")
            violations << "TPM passthrough gives the guest this computer's TPM chip.";
        else if (tag == "video" &&
                 e.firstChildElement("model").firstChildElement("acceleration").attribute("accel3d") == "yes")
            violations << "3D acceleration runs guest graphics code on this computer's GPU stack.";
        else if (tag == "graphics") {
            if (listensOnNetwork(e)) violations << "The " + type + " console listens on a network address.";
            if (type == "egl-headless" || type == "dbus" || e.firstChildElement("gl").attribute("enable") == "yes")
                violations << "The " + type +
                                      " display uses host GPU rendering or exports the screen to other programs.";
        } else if (tag == "channel" || tag == "serial" || tag == "parallel" || tag == "console")
            checkCharacterDevice(e, violations, warnings);
    }
    violations.removeDuplicates();
    warnings.removeDuplicates();
    return {{"violations", violations}, {"warnings", warnings}};
}

QVariantMap Containment::check(const QString &domainXml) {
    return check(domainXml, hostBridges());
}

QString Containment::summary(const QVariantMap &result) {
    return result["violations"].toStringList().join(" ");
}

QString Containment::blocker(virDomainPtr domain, bool live) {
    char *raw = virDomainGetXMLDesc(domain, live ? 0 : VIR_DOMAIN_XML_INACTIVE);
    const QString xml = raw ? QString::fromUtf8(raw) : QString{};
    free(raw);
    if (xml.isEmpty()) return "The VM definition could not be read to check containment.";
    if (!enabled(xml)) return {};
    const auto verdict = check(xml);
    return verdict["violations"].toStringList().isEmpty() ? QString{} : "Contained VM blocked: " + summary(verdict);
}
