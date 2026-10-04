// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "applianceimport.h"
#include "agentprovision.h"
#include "bridgehelper.h"
#include <QScopeGuard>
#include <unistd.h>
#include "paths.h"
#include "configuration.h"
#include "domainconfig.h"
#include "networkcatalog.h"
#include "checkpoints.h"
#include "containment.h"
#include "workspace.h"
#include "cloudimages.h"
#include "cloudseed.h"
#include "unattended.h"
#include <QTimeZone>
#include <archive.h>
#include <archive_entry.h>
#include "guestinput.h"
#include "labs.h"
#include "vmfiles.h"
#include <QLocale>
#include <QStandardPaths>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <libvirt/libvirt-qemu.h>
#include <QSet>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QProcess>
#include <QStorageInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QLocale>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#include <libvirt/virterror.h>
#include <sys/stat.h>

namespace {
// OS presets offered when creating a VM, if this host's libosinfo knows them.
const QList<QPair<QString, QString>> &supportedPresets() {
    static const QList<QPair<QString, QString>> list{{"ubuntu26.04", "Ubuntu 26.04"}, {"ubuntu24.04", "Ubuntu 24.04"}, {"ubuntu22.04", "Ubuntu 22.04"},
        {"fedora44", "Fedora 44"}, {"fedora43", "Fedora 43"}, {"fedora42", "Fedora 42"}, {"debian13", "Debian 13"}, {"debian12", "Debian 12"},
        {"win11", "Windows 11"}, {"win10", "Windows 10"}, {"generic", "Generic OS"}};
    return list;
}
}
namespace {
using Domain = std::unique_ptr<virDomain, decltype(&virDomainFree)>;
using Connection = std::unique_ptr<virConnect, decltype(&virConnectClose)>;
using Network = std::unique_ptr<virNetwork, decltype(&virNetworkFree)>;
using Snapshot = std::unique_ptr<virDomainSnapshot, decltype(&virDomainSnapshotFree)>;
QString xmlOf(virDomainPtr d, bool live = false) {
    char *raw = virDomainGetXMLDesc(d, VIR_DOMAIN_XML_SECURE | (live ? 0 : VIR_DOMAIN_XML_INACTIVE));
    QString value = raw ? QString::fromUtf8(raw) : QString{}; free(raw); return value;
}
QString networkXml(virNetworkPtr n) {
    char *raw = virNetworkGetXMLDesc(n, virNetworkIsPersistent(n) == 1 ? VIR_NETWORK_XML_INACTIVE : 0);
    QString value = raw ? QString::fromUtf8(raw) : QString{}; free(raw); return value;
}
// libvirt emulates a TPM with swtpm, which has to be installed on this computer.
bool tpmAvailable() { return !QStandardPaths::findExecutable("swtpm").isEmpty(); }
QString lastError(QString context) { auto e = virGetLastError(); return context + ": " + (e && e->message ? QString::fromUtf8(e->message) : "libvirt did not provide details"); }
bool run(QString program, QStringList args, QByteArray &output, QString &error, int timeout = 120000) {
    QProcess process; process.start(program, args);
    if (!process.waitForStarted(5000)) { error = "Cannot start " + program + ". Check that it is installed."; return false; }
    if (!process.waitForFinished(timeout)) { process.kill(); process.waitForFinished(3000); error = program + " did not finish within the operation timeout."; return false; }
    output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) { error = program + ": " + QString::fromUtf8(process.readAllStandardError()).left(6000); return false; }
    return true;
}
// Copies one file (e.g. "casper/vmlinuz") out of an ISO image.
bool extractFromIso(const QString &iso, const QString &entryName, const QString &target, QString &error) {
    auto archive = archive_read_new();
    archive_read_support_format_iso9660(archive);
    const auto freeArchive = qScopeGuard([&] { archive_read_free(archive); });
    if (archive_read_open_filename(archive, QFile::encodeName(iso).constData(), 1 << 20) != ARCHIVE_OK) { error = "The installation ISO couldn't be read."; return false; }
    archive_entry *entry = nullptr;
    while (archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
        auto name = QString::fromUtf8(archive_entry_pathname(entry));
        if (name.startsWith("./")) name = name.mid(2);
        if (name != entryName || archive_entry_filetype(entry) != AE_IFREG) continue;
        if (archive_entry_size(entry) > (qint64(512) << 20)) { error = "The installer's " + entryName + " is unexpectedly large."; return false; }
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) { error = "Couldn't write " + target + "."; return false; }
        out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        char buffer[1 << 16];
        for (la_ssize_t n; (n = archive_read_data(archive, buffer, sizeof buffer)) > 0;)
            if (out.write(buffer, n) != n) { error = "Couldn't write " + target + "."; return false; }
        return true;
    }
    error = "The installation ISO has no " + entryName + ", so it can't be installed automatically.";
    return false;
}
QDomElement child(QDomDocument &doc, QDomElement parent, QString name, QString content = {}) {
    auto e = doc.createElement(name); if (!content.isNull()) e.appendChild(doc.createTextNode(content)); parent.appendChild(e); return e;
}
bool managedNetwork(const QString &xml) {
    QDomDocument doc; if (!doc.setContent(xml, true)) return false;
    return doc.documentElement().firstChildElement("name").text().startsWith("omaware-") && doc.elementsByTagNameNS("https://omaware.org/xmlns/network/1", "managed").count() == 1;
}
QVariantList usersOf(virConnectPtr conn, const QString &bridge, const QString &name) {
    QVariantList users; if (!conn) return users;
    virDomainPtr *domains = nullptr; int count = virConnectListAllDomains(conn, &domains, 0);
    if (count < 0) return {"VM inventory unavailable"};
    for (int i = 0; i < count; ++i) {
        bool attached = false;
        for (bool live : {false, true}) {
            if (live && virDomainIsActive(domains[i]) != 1) continue;
            char *raw = virDomainGetXMLDesc(domains[i], !live && virDomainIsPersistent(domains[i]) == 1 ? VIR_DOMAIN_XML_INACTIVE : 0);
            if (!raw) { users.append("VM network configuration unavailable"); continue; }
            QString failure; auto info = DomainConfig::describe(QString::fromUtf8(raw), failure); free(raw);
            if (!failure.isEmpty()) { users.append("VM network configuration unavailable"); continue; }
            for (const auto &v : info["interfaces"].toList()) {
                auto nic = v.toMap();
                if ((nic["type"] == "bridge" && nic["source"] == bridge) || (nic["type"] == "network" && nic["source"] == name)) attached = true;
            }
        }
        if (attached) users.append(QString::fromUtf8(virDomainGetName(domains[i])));
        virDomainFree(domains[i]);
    }
    free(domains); return users;
}
bool overlaps(QString subnet, QString other) {
    const auto a = QHostAddress::parseSubnet(subnet), b = QHostAddress::parseSubnet(other);
    return a.second >= 0 && b.second >= 0 && (a.first.isInSubnet(b) || b.first.isInSubnet(a));
}
QString snapshotBlocker(const QString &xml, bool active) {
    QString error; auto info = DomainConfig::describe(xml, error);
    if (!error.isEmpty()) return error;
    bool disk = false;
    for (const auto &v : info["disks"].toList()) {
        auto d = v.toMap(); if (d["device"] != "disk" || d["readOnly"].toBool()) continue;
        disk = true;
        if (d["type"] != "file" || !QStringList{"qcow2", "raw"}.contains(d["format"].toString())) return "Every writable disk must be a local raw or qcow2 image for a checkpoint.";
    }
    if (!info["shares"].toList().isEmpty()) return "Shared folders are not captured by checkpoints. Remove them before using this checkpoint workflow.";
    QDomDocument doc; doc.setContent(xml);
    if (active && doc.documentElement().firstChildElement("os").firstChildElement("nvram").attribute("format", "raw") != "raw") return "Live UEFI checkpoints require a raw firmware variable store. This VM can be checkpointed while stopped.";
    if (!doc.elementsByTagName("hostdev").isEmpty() || !doc.elementsByTagName("shareable").isEmpty()) return "Passthrough devices and shared writable disks are not supported by this checkpoint workflow.";
    const auto tpms = doc.elementsByTagName("tpm");
    for (int i = 0; i < tpms.size(); ++i) if (tpms.at(i).toElement().firstChildElement("backend").attribute("type") != "emulator") return "Only emulated TPMs can be captured in snapshots.";
    return disk ? QString{} : "Attach a local disk before creating a checkpoint.";
}
// Flattens libvirt typed parameters (bulk domain or node statistics) into numbers and strings.
QHash<QString, QVariant> typedParams(virTypedParameterPtr params, int count) {
    QHash<QString, QVariant> p;
    for (int i = 0; i < count; ++i) {
        const auto &param = params[i];
        const QString key = QString::fromUtf8(param.field);
        switch (param.type) {
        case VIR_TYPED_PARAM_STRING: p[key] = QString::fromUtf8(param.value.s); break;
        case VIR_TYPED_PARAM_ULLONG: p[key] = double(param.value.ul); break;
        case VIR_TYPED_PARAM_LLONG: p[key] = double(param.value.l); break;
        case VIR_TYPED_PARAM_UINT: p[key] = double(param.value.ui); break;
        case VIR_TYPED_PARAM_INT: p[key] = double(param.value.i); break;
        default: break;
        }
    }
    return p;
}
double sumOf(const QHash<QString, QVariant> &p, const QString &prefix, const QString &field) {
    double total = 0;
    for (int i = 0; i < p.value(prefix + ".count").toInt(); ++i) total += p.value(prefix + "." + QString::number(i) + "." + field).toDouble();
    return total;
}
// Runs OmaWare's root helper through pkexec for these networks. On failure, `failure` is for the
// user (with the helper's own explanation) and `code` a stable reason for agents (BridgeHelper::classify).
bool runHelper(const QStringList &uuids, QString &failure, QString &code) {
    QString why;
    const auto helper = BridgeHelper::trusted(&why);
    if (helper.isEmpty()) {
        code = why;
        const auto build = QFileInfo::exists(QCoreApplication::applicationDirPath() + "/CMakeCache.txt") ? QCoreApplication::applicationDirPath() : QString{};
        failure = (why == "helper_untrusted" ? "OmaWare won't run the network helper at " + BridgeHelper::destination() + ": it, or a folder above it, isn't safely owned by root. Reinstall it with:\n"
                                              : "Letting your VMs join networks needs OmaWare's small administrator helper. Install it once with:\n")
            + BridgeHelper::installCommands(BridgeHelper::script(), build).join("\nor\n");
        return false;
    }
    QProcess process; process.start("pkexec", QStringList{helper} + uuids);
    if (!process.waitForStarted(5000)) { code = "authorization_unavailable"; failure = "Cannot start pkexec. Install polkit to let OmaWare ask for administrator authorization."; return false; }
    // The password prompt waits for the user; give them a few minutes.
    if (!process.waitForFinished(300000)) { process.kill(); process.waitForFinished(3000); code = "authorization_timeout"; failure = "Nobody answered the administrator password prompt in time."; return false; }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) return true;
    const auto errors = process.readAllStandardError();
    code = BridgeHelper::classify(process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1, errors);
    if (code == "authorization_cancelled") failure = "The administrator password prompt was closed, so nothing changed.";
    else failure = (code == "helper_failed" ? QString("The network helper failed: ") : QString()) + QString::fromUtf8(errors).trimmed().left(2000);
    if (failure.isEmpty()) failure = BridgeHelper::agentMessage(code);
    return false;
}
// One pass over /proc maps each QEMU process's -uuid to its uptime, so a fleet sample costs one scan
// instead of one scan per VM.
QHash<QString, double> qemuUptimes() {
    QHash<QString, double> result;
    QFile uptime("/proc/uptime"); if (!uptime.open(QIODevice::ReadOnly)) return result;
    const double now = uptime.readAll().split(' ').value(0).toDouble();
    const double ticksPerSecond = double(sysconf(_SC_CLK_TCK));
    for (const auto &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!pid.front().isDigit()) continue;
        QFile cmd("/proc/" + pid + "/cmdline"); if (!cmd.open(QIODevice::ReadOnly)) continue;
        const auto args = cmd.read(65536).split('\0');
        if (args.isEmpty() || args.first().isEmpty() || !QFileInfo(QString::fromLocal8Bit(args.first())).fileName().startsWith("qemu-system-")) continue;
        const int flag = args.indexOf("-uuid"); if (flag < 0) continue;
        QFile stat("/proc/" + pid + "/stat"); if (!stat.open(QIODevice::ReadOnly)) continue;
        const auto line = stat.readAll(); const auto fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
        bool ok; const double ticks = fields.value(19).toDouble(&ok);
        if (ok) result[QString::fromUtf8(args.value(flag + 1))] = std::max(0.0, now - ticks / ticksPerSecond);
    }
    return result;
}
double processUptime(QString uuid) { return qemuUptimes().value(uuid, -1); }
}

void VmWorker::cancelRestart() {
    if (restartUuid_.isEmpty()) return;
    if (restartTimer_) restartTimer_->stop();
    restartUuid_.clear();
    emit finished("Automatic start cancelled. The shutdown request already sent to the guest cannot be withdrawn.", true);
}

void VmWorker::manage(QString op, QVariantMap in) {
    const auto requestedOp = op;
    const bool query = op.endsWith(".list") || op.startsWith("stats") || op == "capabilities";
    auto done = [&](bool ok, QString message, QVariantMap result = {}) {
        result["message"] = message;
        if (in.contains("requestTag")) result["requestTag"] = in["requestTag"];
        if (!query) { if (!storageOnly_) refresh(); emit finished(message, ok); }
        emit managed(requestedOp, ok, result);
    };
    if (op == "media.list") {
        auto result = Workspace::mediaFiles(in["folder"].toString());
        // A folder problem belongs to this picker, not the global connection.
        done(true, "ISO library refreshed", result); return;
    }
    if (!conn_ && storageOnly_) conn_ = virConnectOpen("qemu:///session");
    if (!conn_) { done(false, "Reconnect to the local VM session first."); return; }
    // Provisioning envelopes are prepared at the bridge and reconstructed against fresh
    // libvirt ownership/availability here, before ANY storage or network writes.
    const bool provisioning = in.contains("provisionTool");
    // Checks compare this untouched copy: a later in["key"] read would insert an empty key into
    // the live map and make a valid approved request look changed.
    const QVariantMap envelope = in;
    int provisionMediaFd = -1;
    const auto closeMedia = qScopeGuard([&] { if (provisionMediaFd >= 0) ::close(provisionMediaFd); });
    auto checkProvision = [&]() -> bool {
        if (!provisioning) return true;
        if (!(provisionEpoch() & 1) || envelope["provisionEpoch"].typeId() != QMetaType::ULongLong || envelope["provisionEpoch"].toULongLong() != provisionEpoch()) { done(false,"Agent provisioning authorization was revoked."); return false; }
        const auto tool = envelope["provisionTool"].toString();
        const auto expectedOp = tool == "create_vm" ? "vm.create" : tool == "create_network" ? "networks.save" : tool == "authorize_network" ? "networks.authorize" : "";
        QString error;
        const auto args = envelope["provisionArgs"].toMap();
        if (!envelope["agentRequest"].toBool() || op != expectedOp || !AgentProvision::validate(tool,args,error) || args.value("dry_run",false).toBool()) { done(false,"Invalid provisioning envelope or dry-run mutation."); return false; }
        Connection system(virConnectOpenReadOnly("qemu:///system"),virConnectClose);
        if (!system) { done(false,"Cannot recheck owned host networks."); return false; }
        QString status; const auto choices = NetworkCatalog::discover(conn_,status);
        QVariantList networks;
        virNetworkPtr *all = nullptr; const int count = virConnectListAllNetworks(system.get(),&all,0);
        if (count < 0) { done(false,"Cannot recheck network inventory."); return false; }
        for (int i=0; i<count; ++i) {
            const auto xml = networkXml(all[i]); auto n = NetworkCatalog::describe(xml);
            char uuid[VIR_UUID_STRING_BUFLEN]; virNetworkGetUUIDString(all[i],uuid);
            n["uuid"] = QString::fromUtf8(uuid); n["managed"] = managedNetwork(xml); n["active"] = virNetworkIsActive(all[i]) == 1; n["revision"] = DomainConfig::revision(xml);
            for (const auto &v : choices) if (v.toMap()["id"] == "bridge:"+n["bridge"].toString()) n["available"] = v.toMap()["available"];
            networks.append(n); virNetworkFree(all[i]);
        }
        free(all);
        if (!AgentProvision::verifyEnvelope(op,envelope,networks,error)) { done(false,error); return false; }
        // Discovery itself may take time; revocation during the checks must also win.
        if (!(provisionEpoch() & 1) || envelope["provisionEpoch"].toULongLong() != provisionEpoch()) { done(false,"Agent provisioning authorization was revoked."); return false; }
        return true;
    };
    if (!checkProvision()) return;
    if (op == "vm.create" && in["agentRequest"].toBool() && in["sourceMode"] != "cloud" && !provisioning) { done(false,"Agent local VM creation requires a provisioning envelope."); return; }
    if (provisioning && op == "vm.create") {
        QVariantMap identity; QString error;
        const auto args = envelope["provisionArgs"].toMap();
        provisionMediaFd = AgentProvision::openMedia(args["media_kind"].toString(),args["media"].toString(),identity,error);
        if (provisionMediaFd < 0 || identity != envelope["mediaIdentity"].toMap()) { done(false,"Approved media changed before worker dispatch."); return; }
    }
    if (op == "stats.all") {
        // One bulk call covers every running VM; rates are derived in the UI from consecutive samples.
        QVariantList vms; virDomainStatsRecordPtr *records = nullptr;
        const unsigned groups = VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL | VIR_DOMAIN_STATS_BALLOON | VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_INTERFACE | VIR_DOMAIN_STATS_BLOCK;
        const int count = virConnectGetAllDomainStats(conn_, groups, &records, VIR_CONNECT_GET_ALL_DOMAINS_STATS_ACTIVE);
        const auto uptimes = qemuUptimes();
        for (int i = 0; i < count; ++i) {
            const auto p = typedParams(records[i]->params, records[i]->nparams);
            char id[VIR_UUID_STRING_BUFLEN]; virDomainGetUUIDString(records[i]->dom, id);
            QVariantMap vm{{"uuid", QString::fromUtf8(id)}, {"name", QString::fromUtf8(virDomainGetName(records[i]->dom))},
                {"state", p.value("state.state")}, {"cpuTime", p.value("cpu.time")}, {"vcpus", p.value("vcpu.current")},
                {"rdBytes", sumOf(p, "block", "rd.bytes")}, {"wrBytes", sumOf(p, "block", "wr.bytes")},
                {"rxBytes", sumOf(p, "net", "rx.bytes")}, {"txBytes", sumOf(p, "net", "tx.bytes")}};
            // Each adapter's counters, so the network map can show traffic per cable. `name` is the
            // host tap (matching the adapter's target), absent for some backends; `index` is libvirt's order.
            QVariantList nics;
            for (int n = 0; n < p.value("net.count").toInt(); ++n) {
                const auto key = "net." + QString::number(n) + ".";
                nics.append(QVariantMap{{"index", n}, {"name", p.value(key + "name").toString()}, {"rxBytes", p.value(key + "rx.bytes")}, {"txBytes", p.value(key + "tx.bytes")},
                    {"rxPkts", p.value(key + "rx.pkts")}, {"txPkts", p.value(key + "tx.pkts")},
                    {"errors", p.value(key + "rx.errs").toDouble() + p.value(key + "tx.errs").toDouble() + p.value(key + "rx.drop").toDouble() + p.value(key + "tx.drop").toDouble()}});
            }
            vm["nics"] = nics;
            for (const auto &[field, key] : std::initializer_list<std::pair<const char *, const char *>>{{"balloon.current", "balloonKiB"}, {"balloon.available", "availableKiB"}, {"balloon.unused", "unusedKiB"}, {"balloon.rss", "rssKiB"}})
                if (p.contains(field)) vm[key] = p.value(field);
            vm["uptimeSeconds"] = uptimes.value(vm["uuid"].toString(), -1);
            vms.append(vm);
        }
        if (records) virDomainStatsRecordListFree(records);
        QVariantMap host{{"sampledAt", double(QDateTime::currentMSecsSinceEpoch())}};
        virNodeInfo node{};
        if (virNodeGetInfo(conn_, &node) == 0) { host["cpus"] = node.cpus; host["memoryKiB"] = double(node.memory); }
        int n = 0;
        if (virNodeGetCPUStats(conn_, VIR_NODE_CPU_STATS_ALL_CPUS, nullptr, &n, 0) == 0 && n > 0) {
            std::vector<virNodeCPUStats> cpu(n);
            if (virNodeGetCPUStats(conn_, VIR_NODE_CPU_STATS_ALL_CPUS, cpu.data(), &n, 0) == 0)
                for (const auto &c : cpu) host["cpu_" + QString::fromUtf8(c.field)] = double(c.value);
        }
        n = 0;
        if (virNodeGetMemoryStats(conn_, VIR_NODE_MEMORY_STATS_ALL_CELLS, nullptr, &n, 0) == 0 && n > 0) {
            std::vector<virNodeMemoryStats> memory(n);
            if (virNodeGetMemoryStats(conn_, VIR_NODE_MEMORY_STATS_ALL_CELLS, memory.data(), &n, 0) == 0)
                for (const auto &m : memory) host["mem_" + QString::fromUtf8(m.field)] = double(m.value);
        }
        done(count >= 0, count >= 0 ? "Fleet sample" : lastError("Read VM statistics"), {{"vms", vms}, {"host", host}}); return;
    }
    if (op == "capabilities") {
        virNodeInfo host{}; virNodeGetInfo(conn_, &host);
        QString status;
        QByteArray installed; QString failure;
        const bool creatorReady = run("virt-install", {"--osinfo", "list"}, installed, failure, 15000);
        QVariantList presets;
        for (const auto &preset : supportedPresets())
            if (QString::fromUtf8(installed).contains(QRegularExpression("(^|[,\\n])\\s*" + QRegularExpression::escape(preset.first) + "(,|\\n|$)"))) presets.append(QVariantMap{{"id", preset.first}, {"label", preset.second}});
        // Every OS id this host's libosinfo knows, so an ISO from the shop can select its exact OS.
        QStringList osinfo;
        for (const auto &token : QString::fromUtf8(installed).split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts))
            if (QRegularExpression("^[a-z][a-z0-9.+-]{1,40}$").match(token).hasMatch()) osinfo << token;
        osinfo.removeDuplicates();
        done(true, "Host capabilities loaded", {{"cpus", host.cpus}, {"memoryMiB", qulonglong(host.memory / 1024)},
            {"storage", Paths::vms()}, {"isos", Paths::isos()},
            {"virtInstall", creatorReady}, {"presets", presets}, {"osinfo", osinfo}, {"tpm", tpmAvailable()}, {"networks", NetworkCatalog::discover(conn_, status)}});
        return;
    }
    if (op.startsWith("networks.")) {
        Connection system(op == "networks.list" ? virConnectOpenReadOnly("qemu:///system") : virConnectOpen("qemu:///system"), virConnectClose);
        if (!system) { done(false, lastError("Open host networks; administrator authorization may be required")); return; }
        if (op == "networks.list") {
            QVariantList networks, topology;
            virNetworkPtr *all = nullptr; int count = virConnectListAllNetworks(system.get(), &all, 0);
            QString status; auto choices = NetworkCatalog::discover(conn_, status);
            for (int i = 0; i < count; ++i) {
                auto xml = networkXml(all[i]); auto data = NetworkCatalog::describe(xml);
                QDomDocument doc; doc.setContent(xml);
                data["uuid"] = doc.documentElement().firstChildElement("uuid").text();
                data["title"] = doc.documentElement().firstChildElement("title").text();
                data["active"] = virNetworkIsActive(all[i]) == 1; data["managed"] = managedNetwork(xml);
                data["revision"] = DomainConfig::revision(xml);
                data["users"] = usersOf(conn_, data["bridge"].toString(), data["name"].toString());
                data["systemUsers"] = usersOf(system.get(), data["bridge"].toString(), data["name"].toString());
                data["mode"] = doc.documentElement().firstChildElement("forward").isNull() ? (doc.documentElement().firstChildElement("ip").isNull() ? "isolated" : "hostonly") : "nat";
                if (data["mode"] == "isolated") data["isolation"] = Containment::verifyNetwork(xml, data["active"].toBool());
                auto ip = doc.documentElement().firstChildElement("ip");
                data["cidr"] = ip.isNull() ? QString{} : ip.attribute("address") + "/" + ip.attribute("prefix", QString::number(QHostAddress(ip.attribute("netmask")).toIPv4Address() ? 32 - qPopulationCount(~QHostAddress(ip.attribute("netmask")).toIPv4Address()) : 24));
                auto range = ip.firstChildElement("dhcp").firstChildElement("range"); data["dhcpStart"] = range.attribute("start"); data["dhcpEnd"] = range.attribute("end");
                int autostart = 0; virNetworkGetAutostart(all[i], &autostart); data["autostart"] = bool(autostart);
                for (auto choice : choices) if (choice.toMap()["id"].toString() == "bridge:" + data["bridge"].toString()) { data["available"] = choice.toMap()["available"]; data["reason"] = choice.toMap()["reason"]; }
                networks.append(data); virNetworkFree(all[i]);
            }
            free(all);
            // Guest IPv4 addresses by MAC: DHCP leases of host networks, then the host's neighbour table.
            QHash<QString, QStringList> leases;
            virNetworkPtr *running = nullptr; int nets = virConnectListAllNetworks(system.get(), &running, VIR_CONNECT_LIST_NETWORKS_ACTIVE);
            for (int i = 0; i < nets; ++i) {
                virNetworkDHCPLeasePtr *list = nullptr; int count = virNetworkGetDHCPLeases(running[i], nullptr, &list, 0);
                for (int j = 0; j < count; ++j) {
                    if (list[j]->mac && list[j]->ipaddr && list[j]->type == VIR_IP_ADDR_TYPE_IPV4) leases[QString::fromUtf8(list[j]->mac).toLower()] << QString::fromUtf8(list[j]->ipaddr);
                    virNetworkDHCPLeaseFree(list[j]);
                }
                free(list); virNetworkFree(running[i]);
            }
            free(running);
            virDomainPtr *domains = nullptr; int n = virConnectListAllDomains(conn_, &domains, 0);
            for (int i = 0; i < n; ++i) {
                QString failure; const auto saved = xmlOf(domains[i]); auto info = DomainConfig::describe(saved, failure);
                info["active"] = virDomainIsActive(domains[i]) == 1;
                QVariantMap addresses, addressSources;
                if (info["active"].toBool()) {
                    info["liveInterfaces"] = DomainConfig::describe(xmlOf(domains[i], true), failure)["interfaces"];
                    QHash<QString, QStringList> found = leases;
                    QHash<QString, QString> sources;   // where each MAC's addresses came from, first source wins
                    for (auto it = leases.cbegin(); it != leases.cend(); ++it) sources[it.key()] = "dhcp_lease";
                    virDomainInterfacePtr *ifaces = nullptr; int count = virDomainInterfaceAddresses(domains[i], &ifaces, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP, 0);
                    for (int j = 0; j < count; ++j) {
                        const auto mac = QString::fromUtf8(ifaces[j]->hwaddr ? ifaces[j]->hwaddr : "").toLower();
                        for (unsigned k = 0; k < ifaces[j]->naddrs; ++k)
                            if (ifaces[j]->addrs[k].type == VIR_IP_ADDR_TYPE_IPV4 && !found[mac].contains(QString::fromUtf8(ifaces[j]->addrs[k].addr))) { found[mac] << QString::fromUtf8(ifaces[j]->addrs[k].addr); if (!sources.contains(mac)) sources[mac] = "arp"; }
                        virDomainInterfaceFree(ifaces[j]);
                    }
                    free(ifaces);
                    for (const auto &nic : info["liveInterfaces"].toList()) {
                        const auto mac = nic.toMap()["mac"].toString().toLower();
                        if (found.contains(mac) && !found[mac].isEmpty()) { addresses[mac] = found[mac]; addressSources[mac] = sources.value(mac); }
                    }
                }
                // Saved changes the running VM doesn't use yet (they apply on its next full start).
                int pendingChanges = 0;
                if (info["active"].toBool()) {
                    char id[VIR_UUID_STRING_BUFLEN]; virDomainGetUUIDString(domains[i], id);
                    PendingChanges pending(QString::fromUtf8(id));
                    if (!pending.baseline().isEmpty() && !Configuration::changes(xmlOf(domains[i], true), saved).isEmpty()) pendingChanges = pending.items(saved).size();
                }
                // The revision lets the topology canvas rewire adapters with the same stale-edit protection as Details.
                topology.append(QVariantMap{{"uuid", info["uuid"]}, {"name", info["name"]}, {"active", info["active"]}, {"interfaces", info["interfaces"]},
                    {"liveInterfaces", info["liveInterfaces"]}, {"revision", DomainConfig::revision(saved)}, {"addresses", addresses}, {"addressSources", addressSources}, {"pendingChanges", pendingChanges}});
                virDomainFree(domains[i]);
            }
            free(domains);
            done(true, status, {{"items", networks}, {"topology", topology}, {"choices", choices}}); return;
        }
        if (op == "networks.authorizeMany") {
            // One administrator prompt for several networks (a lab's), with the helper that accepts several.
            QStringList ids;
            for (const auto &v : in["uuids"].toList()) {
                Network each(virNetworkLookupByUUIDString(system.get(), v.toString().toUtf8().constData()), virNetworkFree);
                if (!each || !managedNetwork(networkXml(each.get()))) { done(false, "Only OmaWare-created host networks can be authorized."); return; }
                if (virNetworkIsActive(each.get()) != 1) { done(false, "Start the networks before allowing VMs to join them."); return; }
                ids << v.toString();
            }
            if (ids.isEmpty() || ids.size() > 16) { done(false, "Choose one to sixteen networks."); return; }
            QString failure, code;
            if (!runHelper(ids, failure, code)) {
                // Helpers from before 1.3 take one network at a time.
                if (!failure.contains("one network UUID")) { done(false, failure, {{"code", code}, {"reason", BridgeHelper::agentMessage(code)}}); return; }
                for (const auto &id : ids) if (!runHelper({id}, failure, code)) { done(false, failure, {{"code", code}, {"reason", BridgeHelper::agentMessage(code)}}); return; }
            }
            done(true, ids.size() == 1 ? "Your VMs can now join this network." : "Your VMs can now join these networks."); return;
        }
        const auto uuid = in["uuid"].toString();
        Network net(uuid.isEmpty() ? nullptr : virNetworkLookupByUUIDString(system.get(), uuid.toUtf8().constData()), virNetworkFree);
        QString original = net ? networkXml(net.get()) : QString{};
        if (!uuid.isEmpty() && (!net || !managedNetwork(original))) { done(false, "Only OmaWare-created host networks can be changed here.", {{"code", "network_not_owned"}, {"reason", BridgeHelper::agentMessage("network_not_owned")}}); return; }
        QDomDocument old; old.setContent(original); auto oldRoot = old.documentElement();
        const auto oldBridge = oldRoot.firstChildElement("bridge").attribute("name"), oldName = oldRoot.firstChildElement("name").text();
        auto users = net ? usersOf(conn_, oldBridge, oldName) + usersOf(system.get(), oldBridge, oldName) : QVariantList{};
        if (net && in["revision"].toString() != DomainConfig::revision(original)) { done(false, "The network configuration changed. Refresh Networks before retrying."); return; }
        // pkexec runs this helper as root, so only a root-owned copy in a root-only directory is trusted:
        // a helper that the user (or any program running as the user) can modify would escalate to root.
        auto authorize = [&](const QString &identity, QString &failure, QString &code) { return runHelper({identity}, failure, code); };
        if (op == "networks.save") {
            if (net && (virNetworkIsActive(net.get()) == 1 || !users.isEmpty())) { done(false, "Stop this network and disconnect its VMs before editing its addresses."); return; }
            const auto identity = uuid.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : uuid;
            const auto bridge = net ? oldBridge : "oma" + QUuid(identity).toString(QUuid::Id128).left(8);
            // Why a subnet can't be used, or empty when it's free.
            auto conflict = [&](const QString &cidr) -> QString {
                for (const auto &iface : QNetworkInterface::allInterfaces()) if (iface.name() != oldBridge)
                    for (const auto &entry : iface.addressEntries()) if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && overlaps(cidr, entry.ip().toString() + "/" + QString::number(entry.prefixLength()))) return "The subnet overlaps host interface " + iface.name() + ". Choose another subnet.";
                QString found;
                virNetworkPtr *all = nullptr; int n = virConnectListAllNetworks(system.get(), &all, 0);
                for (int i = 0; i < n; ++i) {
                    QDomDocument d; d.setContent(networkXml(all[i])); auto r = d.documentElement();
                    if (found.isEmpty() && r.firstChildElement("uuid").text() != identity) {
                        for (auto ip = r.firstChildElement("ip"); !ip.isNull(); ip = ip.nextSiblingElement("ip")) {
                            const auto address = ip.attribute("address"), prefix = ip.attribute("prefix", ip.attribute("netmask"));
                            if (overlaps(cidr, address + "/" + prefix)) found = "The subnet overlaps network " + r.firstChildElement("name").text() + ". Choose another subnet.";
                        }
                    }
                    virNetworkFree(all[i]);
                }
                free(all);
                return found;
            };
            auto settings = in;
            if (settings["mode"] != "isolated" && settings["subnet"].toString().trimmed().isEmpty()) {
                // No subnet given: pick the first free 192.168.X.0/24, with a DHCP range in it.
                for (int x = 100; x < 250 && settings["subnet"].toString().isEmpty(); ++x) {
                    const auto cidr = QString("192.168.%1.0/24").arg(x);
                    if (conflict(cidr).isEmpty()) {
                        settings["subnet"] = cidr;
                        if (settings["dhcpStart"].toString().isEmpty()) settings["dhcpStart"] = QString("192.168.%1.100").arg(x);
                        if (settings["dhcpEnd"].toString().isEmpty()) settings["dhcpEnd"] = QString("192.168.%1.200").arg(x);
                    }
                }
                if (settings["subnet"].toString().isEmpty()) { done(false, "No free 192.168.x.0/24 subnet was found. Enter one under Address settings."); return; }
            }
            QString failure; auto xml = Configuration::networkXml(settings, identity, bridge, failure);
            if (!failure.isEmpty()) { done(false, failure); return; }
            QDomDocument edited; edited.setContent(xml);
            if (net && edited.documentElement().firstChildElement("name").text() != oldName) { done(false, "Keep the existing network name when editing its settings."); return; }
            if (settings["mode"] != "isolated") failure = conflict(settings["subnet"].toString());
            if (!failure.isEmpty()) { done(false, failure); return; }
            if (!checkProvision()) return;
            Network defined(virNetworkDefineXML(system.get(), xml.toUtf8().constData()), virNetworkFree);
            if (!defined) { done(false, lastError("Save host network")); return; }
            if (virNetworkSetAutostart(defined.get(), settings.value("autostart", true).toBool()) < 0) { done(false, lastError("Network saved, but setting autostart failed")); return; }
            const bool started = !net && settings.value("start", true).toBool();
            if (started && virNetworkCreate(defined.get()) < 0) { done(false, lastError("Network saved, but starting it failed")); return; }
            QVariantMap result{{"uuid", identity}, {"subnet", settings["subnet"]}, {"authorized", false}, {"revision", DomainConfig::revision(networkXml(defined.get()))}};
            QString message = net ? "Network saved." : "Network created.";
            if (started && settings.value("authorize", false).toBool()) {
                QString why, code;
                if (authorize(identity, why, code)) { result["authorized"] = true; message += " Your VMs can join it."; }
                else { message += " Your VMs can't join it yet: " + why; result["code"] = code; result["reason"] = BridgeHelper::agentMessage(code); }
            }
            done(true, message, result); return;
        }
        if (!net) { done(false, "Select an existing network."); return; }
        int result = -1;
        if (op == "networks.autostart") {
            if (virNetworkSetAutostart(net.get(), in["autostart"].toBool() ? 1 : 0) < 0) { done(false, lastError("Set network autostart")); return; }
            done(true, in["autostart"].toBool() ? "The network now starts with the computer." : "The network no longer starts with the computer.", {{"revision", DomainConfig::revision(networkXml(net.get()))}}); return;
        }
        if (op == "networks.start") result = virNetworkCreate(net.get());
        else if (op == "networks.stop" || op == "networks.remove") {
            if (!users.isEmpty()) { done(false, "This network is still attached to VMs: " + [&] { QStringList names; for (auto user : users) names << user.toString(); return names.join(", "); }()); return; }
            if (op == "networks.stop") result = virNetworkDestroy(net.get());
            else if (virNetworkIsActive(net.get()) == 1) { done(false, "Stop this network before removing it."); return; }
            else result = virNetworkUndefine(net.get());
        } else if (op == "networks.authorize") {
            if (virNetworkIsActive(net.get()) != 1) { done(false, "Start the network before allowing VMs to join it.", {{"code", "bridge_inactive"}, {"reason", BridgeHelper::agentMessage("bridge_inactive")}}); return; }
            if (!checkProvision()) return;
            QString failure, code;
            if (!authorize(uuid, failure, code)) { done(false, failure, {{"code", code}, {"reason", BridgeHelper::agentMessage(code)}}); return; }
            done(true, "Your VMs can now join this network.", {{"revision", DomainConfig::revision(original)}}); return;
        } else { done(false, "Unknown host network operation."); return; }
        done(result == 0, result == 0 ? "Host network operation completed." : lastError("Update host network"), {{"revision", result == 0 && op != "networks.remove" ? DomainConfig::revision(networkXml(net.get())) : QString{}}}); return;
    }
    if (op == "vm.create") {
        auto name = in["name"].toString().trimmed();
        if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9 ._-]{0,47}$").match(name).hasMatch()) { done(false, "Use a VM name of 1–48 letters, numbers, spaces, dots, dashes or underscores, starting with a letter or number."); return; }
        name = "omaware-" + name.toLower().replace(' ', '-');
        Domain duplicate(virDomainLookupByName(conn_, name.toUtf8().constData()), virDomainFree);
        if (duplicate) { done(false, "A VM with this name already exists."); return; }
        int cpus = in.value("cpus", 2).toInt(), memory = in.value("memoryMiB", 4096).toInt(), size = in.value("diskGiB", 32).toInt();
        virNodeInfo host{}; virNodeGetInfo(conn_, &host);
        if (cpus < 1 || unsigned(cpus) > std::min(256u, std::max(1u, host.cpus)) || memory < 256 || qulonglong(memory) > host.memory / 1024 || size < 1 || size > 2048) { done(false, "Choose CPU and memory within the host limits and a 1–2048 GiB disk."); return; }
        const bool cloud = in["sourceMode"] == "cloud";
        auto preset = in.value("preset", "generic").toString();
        const auto firmware = in.value("firmware", "bios").toString();
        bool knownPreset = std::any_of(supportedPresets().begin(), supportedPresets().end(), [&](const auto &p) { return p.first == preset; });
        if (!knownPreset && QRegularExpression("^[a-z][a-z0-9.+-]{1,40}$").match(preset).hasMatch()) {
            // Any other libosinfo id this host knows is fine too (checked, never passed through unvalidated).
            QByteArray list; QString ignored;
            if (run("virt-install", {"--osinfo", "list"}, list, ignored, 15000))
                knownPreset = QString::fromUtf8(list).split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts).contains(preset);
        }
        // A cloud image newer than this host's OS list still boots fine as a generic Linux.
        if (!knownPreset && cloud) { preset = "generic"; knownPreset = true; }
        if (!knownPreset || !QStringList{"bios", "uefi"}.contains(firmware)) { done(false, "Choose a supported OS preset and firmware."); return; }
        const bool importing = in["sourceMode"] == "disk" || cloud || (!provisioning && ApplianceImport::mediaType(in["source"].toString()) == "disk");
        auto source = in["source"].toString();
        // qemu-img reads the pinned descriptor through the parent's proc directory, not a
        // replaceable filename. CLOEXEC is fine: it remains open in this worker process.
        if (provisioning && importing) source = QString("/proc/%1/fd/%2").arg(::getpid()).arg(provisionMediaFd);
        if (!QFileInfo(source).isAbsolute() || !QFileInfo(source).isFile() || !QFileInfo(source).isReadable()) { done(false, "Choose a readable local ISO or disk image."); return; }
        // Cloud images only come from OmaWare's own image folder, where they were checked against their publisher's checksum.
        if (cloud && QFileInfo(source).canonicalPath() != QFileInfo(CloudImages::folder()).canonicalFilePath()) { done(false, "Cloud images must be in OmaWare's image folder."); return; }
        // "Set it up for me": answers for the installer, only for ISOs that support it.
        const auto setupIn = in.value("unattended").toMap();
        const auto setupKind = setupIn.isEmpty() ? QString() : Unattended::kindForFile(QFileInfo(source).fileName());
        if (!setupIn.isEmpty()) {
            const auto password = setupIn["password"].toString();
            if (importing || setupKind.isEmpty()) { done(false, "This ISO can't be installed automatically."); return; }
            if (!CloudSeed::validUser(setupIn["user"].toString())) { done(false, "Choose a user name of lower-case letters, digits, dashes or underscores."); return; }
            if (password.isEmpty() || password.size() > 127 || password.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))) { done(false, "Choose a password of 1–127 characters."); return; }
        }
        const auto seed = in.value("seed").toMap();
        if (cloud && (seed["userData"].toByteArray().size() > 262144 || seed["metaData"].toByteArray().isEmpty() || seed["networkConfig"].toByteArray().size() > 65536)) { done(false, "The first-boot setup is missing or too large."); return; }
        auto parent = in.value("location").toString().isEmpty() ? Paths::vms() : in.value("location").toString();
        QString storageError;
        if (!checkProvision()) return;
        if (provisioning && !AgentProvision::storageRoot(storageError, true)) { done(false,storageError); return; }
        if (!QFileInfo(parent).isAbsolute() || (!QDir().mkpath(parent)) || !QFileInfo(parent).isWritable()) { done(false, "The storage directory is not writable."); return; }
        auto identity = QUuid::createUuid().toString(QUuid::WithoutBraces), directory = parent + "/" + name + "-" + identity.left(8), disk = directory + "/system.qcow2";
        QByteArray output; QString failure;
        // Staging sits next to the new VM: an appliance can unpack to tens of GiB, too much for a RAM-backed /tmp.
        ApplianceImport importedDisk(parent);
        quint64 required = qulonglong(size) * 1073741824;
        if (importing) {
            emit progress("Unpacking and checking the appliance or disk image…");
            if (!importedDisk.prepare(source, in["source"].toString(), failure)) { done(false, failure); return; }
            required = std::max<quint64>(importedDisk.capacity(), cloud ? qulonglong(size) * 1073741824 : 0);
        }
        if (QStorageInfo(parent).bytesAvailable() < qint64(required)) { done(false, "There is not enough free space for the disk's full capacity at this location."); return; }
        if (!checkProvision()) return;
        if (!QDir().mkdir(directory)) { done(false, "Could not create a new unique VM storage directory."); return; }
        QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        emit progress(importing ? "Copying the source disk into a new independent image…" : "Creating the VM's new disk…");
        bool diskOk = importing ? importedDisk.convert(disk, failure) : run("qemu-img", {"create", "-f", "qcow2", disk, QString::number(size) + "G"}, output, failure);
        auto seedPath = directory + "/seed.iso";
        auto cleanup = [&] { QFile::remove(disk); QFile::remove(seedPath); for (const auto &f : {"/setup.iso", "/installer-vmlinuz", "/installer-initrd"}) QFile::remove(directory + f); QDir().rmdir(directory); };
        if (diskOk && cloud) {
            // The image's own size is just big enough for its system; grow it to the size asked for.
            diskOk = run("qemu-img", {"resize", "-f", "qcow2", disk, QString::number(size) + "G"}, output, failure);
            QString seedError;
            if (diskOk && !CloudSeed::writeIso(seedPath, "cidata", {{"user-data", seed["userData"].toByteArray()}, {"meta-data", seed["metaData"].toByteArray()}, {"network-config", seed["networkConfig"].toByteArray()}}, seedError))
                { diskOk = false; failure = seedError; }
        }
        // The answers disc, and for Ubuntu its installer's kernel, which the first boot starts directly.
        const auto answersPath = directory + "/setup.iso", kernelPath = directory + "/installer-vmlinuz", initrdPath = directory + "/installer-initrd";
        if (diskOk && !setupKind.isEmpty()) {
            Unattended::Settings settings;
            settings.user = setupIn["user"].toString(); settings.password = setupIn["password"].toString();
            settings.passwordHash = CloudSeed::hashPassword(settings.password);
            settings.hostname = CloudSeed::hostname(in["name"].toString());
            const auto zone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
            settings.timezone = QRegularExpression("^[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+)*$").match(zone).hasMatch() ? zone : QString("UTC");
            const auto locale = QLocale::system().name();
            settings.locale = QRegularExpression("^[a-z]{2,3}_[A-Z]{2}$").match(locale).hasMatch() ? locale : QString("en_US");
            settings.keyboard = Unattended::keyboardFor(settings.locale);
            settings.uefi = firmware == "uefi";
            const auto file = QFileInfo(source).fileName();
            emit progress("Writing the answers for the installer…");
            QString setupError;
            bool written = false;
            if (setupKind == "windows") written = CloudSeed::writeIso(answersPath, "OMAWARE", {{"autounattend.xml", Unattended::autounattend(settings, file)}}, setupError);
            else written = CloudSeed::writeIso(answersPath, "cidata", {{"user-data", Unattended::subiquityUserData(settings)}, {"meta-data", Unattended::subiquityMetaData("omaware-setup-" + identity)}}, setupError)
                && extractFromIso(source, "casper/vmlinuz", kernelPath, setupError) && extractFromIso(source, "casper/initrd", initrdPath, setupError);
            if (!written) { diskOk = false; failure = setupError; }
            else QFile::setPermissions(answersPath, QFile::ReadOwner | QFile::WriteOwner);
        }
        if (!diskOk) { cleanup(); QFile::remove(answersPath); QFile::remove(kernelPath); QFile::remove(initrdPath); done(false, failure); return; }
        QFile::setPermissions(disk, QFile::ReadOwner | QFile::WriteOwner);
        emit progress("Preparing and validating the VM definition…");
        // Windows installs without extra drivers on SATA disks, e1000e network cards and a standard VGA display.
        // A TPM (which Windows 11 requires) and Secure Boot make it look like a current PC.
        const bool windows = preset.startsWith("win");
        const bool tpm = in.value("tpm", false).toBool();
        if (tpm && !tpmAvailable()) { cleanup(); done(false, "This computer can't give VMs a TPM yet: install the swtpm package, then try again."); return; }
        QStringList args{"--connect", uri_, "--name", name, "--uuid", identity, "--memory", QString::number(memory), "--vcpus", QString::number(cpus), "--osinfo", preset,
            "--disk", "path=" + disk + ",format=qcow2,bus=" + (windows ? "sata" : "virtio"),
            "--graphics", "vnc,listen=none", "--video", windows ? "vga" : "virtio", "--network", "none", "--tpm", tpm ? "emulator,model=tpm-crb,version=2.0" : "none",
            "--channel", "unix,target.type=virtio,target.name=org.qemu.guest_agent.0", "--noautoconsole", "--dry-run", "--print-xml", "1"};
        if (importing) args << "--import"; else args << "--cdrom" << source;
        if (cloud) args << "--disk" << "path=" + seedPath + ",device=cdrom";
        if (!setupKind.isEmpty()) args << "--disk" << "path=" + answersPath + ",device=cdrom";
        if (disk.contains(',') || source.contains(',')) { cleanup(); done(false, "Choose paths without commas for the virt-install workflow."); return; }
        const auto boot = firmware == "uefi" ? QString("uefi") : importing ? QString("hd") : QString("cdrom,hd");
        // Secure Boot when the host has firmware for it, preferably with Microsoft's keys preloaded;
        // otherwise plain UEFI. virt-install's dry run doesn't resolve firmware, so the choice
        // follows the host's firmware descriptors instead of waiting for the define to fail.
        const auto secureFirmware = windows && firmware == "uefi" ? DomainConfig::secureBootFirmware() : QString();
        const QString secureBoot = "uefi,firmware.feature0.name=secure-boot,firmware.feature0.enabled=yes,firmware.feature1.name=enrolled-keys,firmware.feature1.enabled=" + QString(secureFirmware == "enrolled" ? "yes" : "no");
        bool defined = !secureFirmware.isEmpty() && run("virt-install", args + QStringList{"--boot", secureBoot}, output, failure);
        if (!defined && !run("virt-install", args + QStringList{"--boot", boot}, output, failure)) { cleanup(); done(false, failure); return; }
        QDomDocument doc;
        if (!doc.setContent(output)) { cleanup(); done(false, "virt-install did not return a valid domain definition."); return; }
        auto root = doc.documentElement(), devices = root.firstChildElement("devices");
        if (!importing) {
            // virt-install's first stage normally stops at reboot so its own installer
            // can swap XML. OmaWare retains the media and provides explicit ejection.
            auto reboot = root.firstChildElement("on_reboot");
            if (!reboot.isNull()) root.removeChild(reboot);
            child(doc, root, "on_reboot", "restart");
        }
        auto metadata = root.firstChildElement("metadata"); if (metadata.isNull()) metadata = child(doc, root, "metadata");
        metadata.appendChild(doc.createElementNS("https://omaware.org/xmlns/prototype/1", "omaware:managed"));
        if (!setupKind.isEmpty()) {
            // Still to be set up: what start() and finishSetup() need. The answers stay in the VM's private folder.
            auto setup = doc.createElementNS(Unattended::ns, "omasetup:setup");
            setup.setAttribute("kind", setupKind); setup.setAttribute("state", "new"); setup.setAttribute("uefi", firmware == "uefi" ? "1" : "0");
            setup.setAttribute("answers", answersPath); setup.setAttribute("media", source);
            if (setupKind == "subiquity") { setup.setAttribute("kernel", kernelPath); setup.setAttribute("initrd", initrdPath); }
            metadata.appendChild(setup);
            // The installer boots from its own disc, the first CD drive; Ubuntu boots its installer's kernel directly
            // the first time, so it boots from the disk from then on.
            auto os = root.firstChildElement("os");
            for (auto b = os.firstChildElement("boot"); !b.isNull(); b = os.firstChildElement("boot")) os.removeChild(b);
            for (const auto &dev : setupKind == "subiquity" ? QStringList{"hd", "cdrom"} : QStringList{"cdrom", "hd"}) child(doc, os, "boot").setAttribute("dev", dev);
            auto disks = devices.elementsByTagName("disk");
            for (int i = 0; i < disks.size(); ++i) {
                auto e = disks.at(i).toElement();
                if (e.attribute("device") == "cdrom" && e.firstChildElement("source").attribute("file") == answersPath) { devices.appendChild(devices.removeChild(e)); break; }
            }
        }
        if (const auto lab = in["lab"].toMap(); !lab.isEmpty()) {
            // Which lab built this VM and the login it was set up with (the name of a saved login, never a password).
            auto tag = doc.createElementNS(Labs::ns, "omalab:lab");
            for (const QString key : {"name", "slug", "login", "user"}) tag.setAttribute(key, lab[key].toString().left(64));
            metadata.appendChild(tag);
        }
        QString status; const auto choices = NetworkCatalog::discover(conn_, status); QVariantMap choice;
        for (auto v : choices) if (v.toMap()["id"] == in.value("networkId", "user")) choice = v.toMap();
        if (in.contains("networks")) {
            // Several adapters, each with the MAC address its first-boot network settings expect.
            QSet<QString> macs;
            for (const auto &entry : in["networks"].toList()) {
                const auto want = entry.toMap(); QVariantMap found;
                for (auto v : choices) if (v.toMap()["id"] == want["id"]) found = v.toMap();
                const auto mac = want["mac"].toString().toLower();
                if (found.isEmpty() || !found["available"].toBool()) { cleanup(); done(false, "A network for this VM is unavailable: " + (found.isEmpty() ? want["id"].toString() : found["reason"].toString())); return; }
                if (!QRegularExpression("^52:54:00(:[0-9a-f]{2}){3}$").match(mac).hasMatch() || macs.contains(mac)) { cleanup(); done(false, "Each adapter needs its own MAC address."); return; }
                macs.insert(mac);
                QString nic; DomainConfig::networkDevice(doc.toString(-1), {}, found["kind"].toString(), found["source"].toString(), windows ? "e1000e" : "virtio", true, false, nic, failure);
                QDomDocument device; device.setContent(nic);
                auto macElement = device.documentElement().firstChildElement("mac");
                if (macElement.isNull()) { macElement = device.createElement("mac"); device.documentElement().insertBefore(macElement, device.documentElement().firstChild()); }
                macElement.setAttribute("address", mac);
                devices.appendChild(doc.importNode(device.documentElement(), true));
            }
        } else if (in["networkId"] != "none") {
            if (choice.isEmpty() || !choice["available"].toBool()) { cleanup(); done(false, "The selected network is unavailable. Refresh its configuration before creating the VM."); return; }
            QString nic; DomainConfig::networkDevice(doc.toString(-1), {}, choice["kind"].toString(), choice["source"].toString(), windows ? "e1000e" : "virtio", true, false, nic, failure);
            QDomDocument device; device.setContent(nic); devices.appendChild(doc.importNode(device.documentElement(), true));
        }
        auto balloon = devices.firstChildElement("memballoon"); if (!balloon.isNull() && balloon.attribute("model") == "virtio") child(doc, balloon, "stats").setAttribute("period", "5");
        if (provisioning && !checkProvision()) { cleanup(); return; }
        Domain createdVm(virDomainDefineXMLFlags(conn_, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE), virDomainFree);
        if (!createdVm) { failure = lastError("Define VM"); cleanup(); done(false, failure); return; }
        emit created(identity);
        const auto created = !setupKind.isEmpty() ? QString("VM created. Start it and the installer sets it up by itself, then OmaWare removes the installation media.")
            : QString("VM created. Start it to boot the selected installation media or imported disk.");
        done(true, (QStringList{created} + importedDisk.notes()).join(' '), {{"uuid", identity}, {"storage", directory}, {"importNotes", importedDisk.notes()}, {"unattended", setupKind}}); return;
    }

    const auto uuid = in["uuid"].toString();
    Domain domain(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
    if (!domain) { done(false, lastError("Find VM")); return; }
    if (in.value("agentRequest").toBool() && (!owned(domain.get()) || virDomainIsPersistent(domain.get()) != 1 || Containment::enabled(xmlOf(domain.get())))) { done(false, "Agents require an owned, persistent, non-contained VM."); return; }
    const bool active = virDomainIsActive(domain.get()) == 1;
    if (op == "stats") {
        virDomainInfo info{}; QVariantMap result{{"uuid", uuid}, {"active", active}};
        if (active && virDomainGetInfo(domain.get(), &info) == 0) {
            auto now = QDateTime::currentMSecsSinceEpoch(); auto previous = cpuSamples_.value(uuid);
            if (previous.second > 0 && now > previous.second && info.cpuTime >= previous.first) result["cpuPercent"] = std::clamp(double(info.cpuTime - previous.first) / double(now - previous.second) / 10000 / std::max(1u, unsigned(info.nrVirtCpu)), 0.0, 100.0);
            cpuSamples_[uuid] = {info.cpuTime, now};
            virDomainMemoryStatStruct memory[VIR_DOMAIN_MEMORY_STAT_NR]; int n = virDomainMemoryStats(domain.get(), memory, VIR_DOMAIN_MEMORY_STAT_NR, 0);
            qulonglong total = 0, unused = 0; bool hasUnused = false;
            for (int i = 0; i < n; ++i) {
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON) total = memory[i].val;
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_UNUSED) { unused = memory[i].val; hasUnused = true; }
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_RSS) result["hostRssMiB"] = double(memory[i].val) / 1024;
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_AVAILABLE) result["guestTotalMiB"] = double(memory[i].val) / 1024;
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_DISK_CACHES) result["cacheMiB"] = double(memory[i].val) / 1024;
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_SWAP_IN) result["swapInKiB"] = double(memory[i].val);
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_SWAP_OUT) result["swapOutKiB"] = double(memory[i].val);
                if (memory[i].tag == VIR_DOMAIN_MEMORY_STAT_MAJOR_FAULT) result["majorFaults"] = double(memory[i].val);
            }
            if (total && hasUnused && unused <= total) { result["usedMiB"] = double(total - unused) / 1024; result["freeMiB"] = double(unused) / 1024; }
            if (total) result["balloonMiB"] = double(total) / 1024;
            result["uptimeSeconds"] = processUptime(uuid);
            // Cumulative device counters; the UI derives rates from consecutive samples.
            virDomainPtr list[] = {domain.get(), nullptr}; virDomainStatsRecordPtr *records = nullptr;
            if (virDomainListGetStats(list, VIR_DOMAIN_STATS_BLOCK | VIR_DOMAIN_STATS_INTERFACE | VIR_DOMAIN_STATS_VCPU, &records, 0) > 0 && records[0]) {
                const auto p = typedParams(records[0]->params, records[0]->nparams);
                auto rows = [&](const QString &prefix, const QStringList &fields) {
                    QVariantList out;
                    for (int i = 0; i < p.value(prefix + ".count").toInt(); ++i) {
                        const auto base = prefix + "." + QString::number(i) + ".";
                        QVariantMap row;
                        for (const auto &field : fields) if (p.contains(base + field)) row[QString(field).replace('.', '_')] = p[base + field];
                        out.append(row);
                    }
                    return out;
                };
                result["blocks"] = rows("block", {"name", "rd.bytes", "wr.bytes", "rd.reqs", "wr.reqs", "allocation", "capacity", "physical"});
                result["nets"] = rows("net", {"name", "rx.bytes", "tx.bytes", "rx.pkts", "tx.pkts", "rx.errs", "tx.errs", "rx.drop", "tx.drop"});
                QVariantList vcpus;
                for (int i = 0; i < p.value("vcpu.maximum").toInt(); ++i) {
                    const auto base = "vcpu." + QString::number(i) + ".";
                    if (p.contains(base + "time")) vcpus.append(QVariantMap{{"id", i}, {"time", p[base + "time"]}, {"state", p.value(base + "state")}});
                }
                result["vcpus"] = vcpus;
            }
            if (records) virDomainStatsRecordListFree(records);
        } else cpuSamples_.remove(uuid);
        result["sampledAt"] = double(QDateTime::currentMSecsSinceEpoch());
        virNodeInfo host{};
        if (virNodeGetInfo(conn_, &host) == 0) { result["hostCpus"] = host.cpus; result["hostMemoryMiB"] = double(host.memory) / 1024; }
        if (auto freeBytes = virNodeGetFreeMemory(conn_)) result["hostFreeMiB"] = double(freeBytes) / 1048576;
        done(true, "Live resource sample", result); return;
    }
    const auto xml = xmlOf(domain.get());
    if (op == "snapshots.list") {
        QVariantList rows; virDomainSnapshotPtr *snapshots = nullptr;
        int n = virDomainListAllSnapshots(domain.get(), &snapshots, 0);
        for (int i = 0; i < n; ++i) {
            char *raw = virDomainSnapshotGetXMLDesc(snapshots[i], 0); QDomDocument doc; doc.setContent(raw ? QString::fromUtf8(raw) : QString{}); free(raw);
            auto r = doc.documentElement(); rows.append(QVariantMap{{"kind", "internal"}, {"capture", "stopped"}, {"name", r.firstChildElement("name").text()}, {"notes", r.firstChildElement("description").text()},
                {"time", r.firstChildElement("creationTime").text().toLongLong()}, {"parent", r.firstChildElement("parent").firstChildElement("name").text()},
                {"parentId", r.firstChildElement("parent").firstChildElement("name").text().isEmpty() ? QString{} : "internal:" + r.firstChildElement("parent").firstChildElement("name").text()},
                {"state", r.firstChildElement("state").text()}, {"current", virDomainSnapshotIsCurrent(snapshots[i], 0) == 1}, {"leaf", virDomainSnapshotNumChildren(snapshots[i], 0) == 0}});
            virDomainSnapshotFree(snapshots[i]);
        }
        free(snapshots); const auto history = Checkpoints::history(uuid); rows += history["items"].toList();
        QString createBlocker = snapshotBlocker(xml, active);
        if (createBlocker.isEmpty() && n > 0) createBlocker = "This VM has legacy internal snapshots. Remove those before using independent checkpoints.";
        PendingChanges pending(uuid);
        if (createBlocker.isEmpty() && (!pending.items(xml).isEmpty() || !pending.matches(xml))) createBlocker = "Apply or discard pending hardware settings before creating a checkpoint.";
        auto restoreBlocker = snapshotBlocker(xml, false);
        if (restoreBlocker.isEmpty() && (!pending.items(xml).isEmpty() || !pending.matches(xml))) restoreBlocker = "Apply or discard pending hardware settings before restoring a checkpoint.";
        done(true, "Checkpoints loaded", {{"uuid", uuid}, {"items", rows}, {"currentId", history["currentId"]}, {"storage", Checkpoints::storage(conn_, uuid)}, {"blocker", createBlocker}, {"restoreBlocker", restoreBlocker}}); return;
    }
    if (!owned(domain.get()) || virDomainIsPersistent(domain.get()) != 1) { done(false, "This operation requires an OmaWare-managed persistent VM."); return; }
    // Agent-originated operations recheck policy on the worker, not just cached inventory.
    if (in.value("agentRequest").toBool() && Containment::enabled(xml)) { done(false, "Contained VMs cannot be accessed by agents."); return; }
    if (op == "vm.readiness") {
        int state = 0, reason = 0;
        const bool running = virDomainGetState(domain.get(), &state, &reason, 0) == 0 && state == VIR_DOMAIN_RUNNING;
        bool agentReady = false;
        if (running && in["probeAgent"].toBool()) {
            char *raw = virDomainQemuAgentCommand(domain.get(), "{\"execute\":\"guest-ping\"}", 2, 0);
            if (raw) { agentReady = QJsonDocument::fromJson(raw).object().contains("return"); free(raw); }
        }
        done(true, "Readiness observed.", {{"running", running}, {"active", active}, {"paused", state == VIR_DOMAIN_PAUSED}, {"guest_agent", agentReady}}); return;
    }
    if (op == "vm.details") {
        QString failure;
        auto details = DomainConfig::describe(xml, failure);
        if (!failure.isEmpty()) { done(false, failure); return; }
        auto live = active ? DomainConfig::describe(xmlOf(domain.get(), true), failure) : details;
        PendingChanges pending(uuid);
        // After a full restart the VM runs its saved settings; nothing is pending any more.
        if (active && !pending.baseline().isEmpty() && Configuration::changes(xmlOf(domain.get(), true), xml).isEmpty()) pending.clear();
        details["active"] = active;
        details["liveInterfaces"] = live["interfaces"];
        details["agentConnected"] = active && live["agentConnected"].toBool();
        details["changes"] = pending.items(xml);
        details["pendingConflict"] = !pending.matches(xml);
        QString status;
        QVariantList options;
        for (const auto &v : NetworkCatalog::discover(conn_, status))
            if (!in.value("agentRequest").toBool() || v.toMap()["id"] == "user" || v.toMap()["managed"].toBool()) options.append(v);
        details["networkOptions"] = options;
        // Inspect metadata only, on the worker thread. Never read disk contents or host logs.
        QVariantList diskChecks;
        for (const auto &value : details["disks"].toList()) {
            const auto disk = value.toMap();
            const auto source = disk["source"].toString();
            QVariantMap check{{"target", disk["target"]}, {"device", disk["device"]}};
            if (source.isEmpty() || disk["type"] != "file") {
                check["checked"] = false;
                check["reason"] = source.isEmpty() ? "No media attached" : "Not a local file-backed disk";
            } else {
                QFileInfo file(source);
                check["checked"] = true;
                check["exists"] = file.exists();
                check["readable"] = file.isReadable();
                if (file.exists()) check["file_bytes"] = file.size();
                QStorageInfo storage(file.absolutePath());
                if (storage.isValid() && storage.isReady()) {
                    check["filesystem_available_bytes"] = storage.bytesAvailable();
                    check["filesystem_read_only"] = storage.isReadOnly();
                }
            }
            diskChecks.append(check);
        }
        details["diskDiagnostics"] = diskChecks;
        done(true, "VM configuration loaded.", details); return;
    }
    if (op == "adapter.save") {
        if (in.value("agentRequest").toBool() && !in["remove"].toBool()) {
            QString status; bool allowed = false;
            for (const auto &v : NetworkCatalog::discover(conn_, status)) {
                const auto choice = v.toMap();
                if (choice["id"] == in["networkId"] && (choice["id"] == "user" || choice["managed"].toBool())) allowed = true;
            }
            if (!allowed) { done(false, "Agents may attach only to OmaWare-managed networks or private user networking."); return; }
        }
        QString message;
        const bool ok = changeNetwork(uuid, in["mac"].toString(), in["networkId"].toString(), in["model"].toString(), in["linkUp"].toBool(), in["remove"].toBool(), in["revision"].toString(), message);
        // The new revision and what still waits for a restart, so callers don't have to list again.
        const auto saved = xmlOf(domain.get());
        done(ok, message, {{"revision", DomainConfig::revision(saved)}, {"pending_change_count", PendingChanges(uuid).items(saved).size()}, {"active", virDomainIsActive(domain.get()) == 1}}); return;
    }
    if (op == "vm.delete") {
        // Deleting is never something an agent may do, and only OmaWare's own VMs can be deleted.
        if (in.value("agentRequest").toBool()) { done(false, "AI agents can't delete VMs."); return; }
        if (!owned(domain.get())) { done(false, "OmaWare only deletes VMs it created; this one is read-only."); return; }
        const auto app = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), checkpointRoot = app + "/checkpoints/" + uuid;
        if (QUuid(uuid).isNull()) { done(false, "Invalid VM identity."); return; }
        if (QFile::exists(checkpointRoot + "/restore.json") || QFile::exists(checkpointRoot + "/freeze.json")) { done(false, "A snapshot restore or guest freeze for this VM hasn't finished. Recover it on the Snapshots page before deleting the VM."); return; }
        if (active) {
            if (!in.value("powerOff").toBool()) { done(false, "Turn the VM off before deleting it."); return; }
            emit progress("Powering the VM off…");
            if (virDomainDestroy(domain.get()) < 0) { done(false, lastError("Power off")); return; }
        }
        const auto name = QString::fromUtf8(virDomainGetName(domain.get()));
        // Restores keep the disks and firmware variables they replace, so every definition the VM has had counts.
        QStringList definitions{xmlOf(domain.get())};
        for (const auto &id : QDir(checkpointRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile manifest(checkpointRoot + "/" + id + "/manifest.json");
            if (manifest.size() < 8 * 1024 * 1024 && manifest.open(QIODevice::ReadOnly)) definitions << QJsonDocument::fromJson(manifest.readAll()).object()["xml"].toString();
        }
        QFile pendingChanges(app + "/pending/" + uuid + ".json");
        if (pendingChanges.size() < 8 * 1024 * 1024 && pendingChanges.open(QIODevice::ReadOnly)) definitions << QJsonDocument::fromJson(pendingChanges.readAll()).object()["xml"].toString();
        const auto nvramFolder = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/libvirt/qemu/nvram";
        auto plan = VmFiles::plan(definitions, uuid, {checkpointRoot, Paths::vms() + "/" + uuid, Paths::legacyVms() + "/" + uuid}, nvramFolder);
        emit progress("Deleting the VM…");
        // Firmware variables (UEFI), TPM state, saved state and libvirt's snapshot records go with the definition.
        const unsigned flags = VIR_DOMAIN_UNDEFINE_MANAGED_SAVE | VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA | VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA
            | VIR_DOMAIN_UNDEFINE_NVRAM | VIR_DOMAIN_UNDEFINE_TPM;
        if (virDomainUndefineFlags(domain.get(), flags) < 0) { done(false, lastError("Delete VM")); return; }
        QFile::remove(app + "/pending/" + uuid + ".json");
        const auto tpm = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/libvirt/qemu/swtpm/" + uuid;
        if (QFileInfo(tpm).isDir() && !QFileInfo(tpm).isSymLink()) QDir(tpm).removeRecursively();
        // A disk is deleted only once no other VM uses it. If that can't be checked, everything stays.
        QSet<QString> referenced; QString failure;
        // Its own snapshots refer to each other; only what other VMs use keeps a file.
        if (!Checkpoints::references(conn_, referenced, failure, uuid)) {
            done(false, name + " was removed, but its disks and snapshots were kept because OmaWare couldn't check whether another VM uses them (" + failure + ").", {{"uuid", uuid}}); return;
        }
        VmFiles::exclude(plan, referenced);
        emit progress("Deleting its disks and snapshots…");
        QStringList failures;
        const auto freed = VmFiles::remove(plan, failures);
        auto message = QString("%1 was deleted with its disks and snapshots. %2 freed.").arg(name, QLocale().formattedDataSize(freed));
        if (!plan.kept.isEmpty()) message += " Kept (not made for this VM, or used by another VM): " + plan.kept.join(", ") + ".";
        if (!failures.isEmpty()) message += " Couldn't delete: " + failures.join(", ") + ".";
        done(failures.isEmpty(), message, {{"uuid", uuid}, {"freedBytes", double(freed)}, {"kept", plan.kept}}); return;
    }
    if (op == "vm.power") {
        const auto action = in["action"].toString();
        if (!QStringList{"start", "shutdown", "force-off", "pause", "resume", "remove"}.contains(action)) { done(false, "Unknown power action."); return; }
        const auto message = power(domain.get(), action);
        static const QMap<QString, QString> verbs{{"start", "VM started."}, {"shutdown", "Shutdown requested; the guest decides when it stops."}, {"force-off", "VM powered off."},
            {"pause", "VM paused."}, {"resume", "VM resumed."}, {"remove", "VM removed. Its disk files were kept."}};
        done(message.isEmpty(), message.isEmpty() ? verbs.value(action) : message, {{"uuid", uuid}}); return;
    }
    if (op == "vm.serial") {
        // Text on the VM's first serial console, through libvirt. SAFE opens it only when libvirt can
        // make sure nobody else (virsh console, another agent call) has it at the same time.
        if (!active) { done(false, "Start the VM first."); return; }
        std::unique_ptr<virStream, decltype(&virStreamFree)> stream(virStreamNew(conn_, VIR_STREAM_NONBLOCK), virStreamFree);
        if (!stream || virDomainOpenConsole(domain.get(), nullptr, stream.get(), VIR_DOMAIN_CONSOLE_SAFE) < 0) {
            done(false, lastError("Open the serial console") + ". The VM needs a serial console, and nobody else may have it open (close virsh console).", {{"code", "console_unavailable"}}); return;
        }
        const auto closeStream = qScopeGuard([&] { virStreamAbort(stream.get()); });
        const auto send = in["send"].toString().replace("\r\n", "\r").replace('\n', '\r').toUtf8();
        const auto expected = in["waitFor"].toString().toUtf8();
        const qint64 limit = std::clamp(in.value("timeout", 10).toInt(), 1, 60) * 1000LL;
        const int maxBytes = std::clamp(in.value("maxBytes", 16384).toInt(), 1024, 65536);
        QElapsedTimer clock; clock.start();
        for (qsizetype sent = 0; sent < send.size();) {
            const int n = virStreamSend(stream.get(), send.constData() + sent, size_t(std::min<qsizetype>(send.size() - sent, 4096)));
            if (n == -2) { if (clock.elapsed() > limit) break; QThread::msleep(20); continue; }
            if (n < 0) { done(false, lastError("Write to the serial console"), {{"code", "console_unavailable"}}); return; }
            sent += n;
        }
        // Read until the expected text shows up, or (without one) the guest has been quiet for a second.
        QByteArray output; bool matched = false, truncated = false, closed = false;
        qint64 lastData = clock.elapsed();
        char buffer[4096];
        while (clock.elapsed() < limit) {
            const int n = virStreamRecv(stream.get(), buffer, sizeof buffer);
            if (n > 0) {
                output.append(buffer, n); lastData = clock.elapsed();
                if (output.size() > maxBytes) { output = output.right(maxBytes); truncated = true; }
                if (!expected.isEmpty() && output.contains(expected)) { matched = true; break; }
                continue;
            }
            if (n == 0) { closed = true; break; }
            if (n == -1) { done(false, lastError("Read the serial console"), {{"code", "console_unavailable"}}); return; }
            if (expected.isEmpty() && clock.elapsed() - lastData >= 1000) break;
            QThread::msleep(30);
        }
        // Terminal control sequences and carriage returns are noise in text an agent reads.
        auto text = QString::fromUtf8(output);
        text.remove(QRegularExpression("\\x1b(\\[[0-9;?]*[ -/]*[@-~]|\\][^\\x07]*\\x07|[()][0-9A-Za-z]|[=>78DEHMc])")).remove('\r');
        done(true, "Serial console read.", {{"output", text}, {"bytes", output.size()}, {"matched", matched}, {"timedOut", !expected.isEmpty() && !matched && !closed},
            {"truncated", truncated}, {"closed", closed}, {"elapsedMs", clock.elapsed()}}); return;
    }
    if (op == "vm.screenshot") {
        if (!active) { done(false, "Start the VM to see its screen."); return; }
        std::unique_ptr<virStream, decltype(&virStreamFree)> stream(virStreamNew(conn_, 0), virStreamFree);
        char *mime = stream ? virDomainScreenshot(domain.get(), stream.get(), 0, 0) : nullptr;
        if (!mime) { done(false, lastError("Capture the screen")); return; }
        free(mime);
        QByteArray data; char buffer[65536];
        for (;;) {
            const int n = virStreamRecv(stream.get(), buffer, sizeof buffer);
            if (n > 0) data.append(buffer, n);
            if (n == 0) break;
            if (n < 0 || data.size() > 128 * 1024 * 1024) { virStreamAbort(stream.get()); done(false, lastError("Read the screen")); return; }
        }
        virStreamFinish(stream.get());
        QImage image;
        if (!image.loadFromData(data)) { done(false, "The screen image couldn't be read."); return; }
        const QSize screen = image.size();
        // Large screens are scaled down so they stay readable for an AI model; clicks use the scaled size.
        const int maxWidth = std::clamp(in.value("maxWidth", 1280).toInt(), 320, 3840);
        if (image.width() > maxWidth) image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);
        QByteArray png; QBuffer buffer2(&png); buffer2.open(QIODevice::WriteOnly); image.save(&buffer2, "PNG");
        done(true, "Screen captured", {{"uuid", uuid}, {"png", QString::fromLatin1(png.toBase64())}, {"width", image.width()}, {"height", image.height()},
            {"screenWidth", screen.width()}, {"screenHeight", screen.height()}}); return;
    }
    if (op == "vm.input") {
        if (!active) { done(false, "Start the VM before using its screen."); return; }
        const double width = in["width"].toDouble(), height = in["height"].toDouble();
        QDomDocument live; live.setContent(xmlOf(domain.get(), true));
        bool tablet = false;
        for (auto e = live.documentElement().firstChildElement("devices").firstChildElement("input"); !e.isNull(); e = e.nextSiblingElement("input")) tablet |= e.attribute("type") == "tablet";
        auto monitor = [&](const QJsonArray &events, QString &why) {
            const auto command = QJsonDocument(QJsonObject{{"execute", "input-send-event"}, {"arguments", QJsonObject{{"events", events}}}}).toJson(QJsonDocument::Compact);
            char *reply = nullptr;
            const bool ok = virDomainQemuMonitorCommand(domain.get(), command.constData(), &reply, 0) == 0 && reply && !QByteArray(reply).contains("\"error\"");
            if (!ok) why = reply ? QString::fromUtf8(reply).left(300) : lastError("Send pointer input");
            free(reply);
            return ok;
        };
        auto at = [&](double x, double y, QString &why) {
            if (!tablet) { why = "This VM has no tablet pointer, so OmaWare can't click at a position. Use keys instead."; return QJsonArray{}; }
            if (width < 2 || height < 2 || x < 0 || y < 0 || x > width || y > height) { why = "The position is outside the screen. Take a screenshot and use its coordinates."; return QJsonArray{}; }
            auto axis = [](const char *name, double value, double size) { return QJsonObject{{"type", "abs"}, {"data", QJsonObject{{"axis", name}, {"value", int(std::lround(value * 32767.0 / (size - 1)))}}}}; };
            return QJsonArray{axis("x", std::min(x, width - 1), width), axis("y", std::min(y, height - 1), height)};
        };
        auto button = [](const QString &name, bool down) { return QJsonObject{{"type", "btn"}, {"data", QJsonObject{{"down", down}, {"button", name}}}}; };
        auto keys = [&](const QList<GuestInput::Chord> &chords, QString &why) {
            for (const auto &chord : chords) {
                std::vector<unsigned> codes(chord.begin(), chord.end());
                if (virDomainSendKey(domain.get(), VIR_KEYCODE_SET_LINUX, 20, codes.data(), int(codes.size()), 0) < 0) { why = lastError("Type"); return false; }
                // Lets the guest keep up with long text.
                QThread::msleep(25);
            }
            return true;
        };
        QStringList doneSteps; QString why;
        const auto actions = in["actions"].toList();
        if (actions.isEmpty() || actions.size() > 50) { done(false, "Send one to fifty actions."); return; }
        for (const auto &entry : actions) {
            const auto a = entry.toMap();
            const auto type = a["type"].toString();
            const QString name = a.value("button", "left").toString();
            if (type == "click" || type == "double_click" || type == "move" || type == "scroll") {
                if (!QStringList{"left", "right", "middle"}.contains(name)) { why = "button must be left, right or middle"; break; }
                auto events = at(a["x"].toDouble(), a["y"].toDouble(), why);
                if (events.isEmpty()) break;
                if (type == "move" && !monitor(events, why)) break;
                if (type == "click" || type == "double_click") {
                    for (int i = 0; i < (type == "double_click" ? 2 : 1); ++i) {
                        auto all = events; all.append(button(name, true));
                        if (!monitor(all, why) || !monitor({button(name, false)}, why)) break;
                        QThread::msleep(60);
                    }
                    if (!why.isEmpty()) break;
                }
                if (type == "scroll") {
                    const int amount = std::clamp(a.value("amount", 3).toInt(), -30, 30);
                    if (!monitor(events, why)) break;
                    for (int i = 0; i < std::abs(amount) && why.isEmpty(); ++i) {
                        const QString wheel = amount > 0 ? "wheel-down" : "wheel-up";
                        if (monitor({button(wheel, true)}, why)) monitor({button(wheel, false)}, why);
                    }
                    if (!why.isEmpty()) break;
                }
            } else if (type == "drag") {
                auto from = at(a["x"].toDouble(), a["y"].toDouble(), why);
                auto to = from.isEmpty() ? QJsonArray{} : at(a["to_x"].toDouble(), a["to_y"].toDouble(), why);
                if (to.isEmpty()) break;
                auto press = from; press.append(button("left", true));
                if (!monitor(press, why)) break;
                // A few steps in between, so the guest sees a real drag.
                for (int i = 1; i <= 5 && why.isEmpty(); ++i) {
                    const double t = i / 5.0;
                    auto step = at(a["x"].toDouble() + (a["to_x"].toDouble() - a["x"].toDouble()) * t, a["y"].toDouble() + (a["to_y"].toDouble() - a["y"].toDouble()) * t, why);
                    if (!step.isEmpty()) { monitor(step, why); QThread::msleep(30); }
                }
                if (!why.isEmpty() || !monitor({button("left", false)}, why)) break;
            } else if (type == "type") {
                const auto text = a["text"].toString();
                QList<GuestInput::Chord> chords;
                if (text.size() > 4000) { why = "Type at most 4000 characters at a time."; break; }
                if (!GuestInput::chordsForText(text, chords, why) || !keys(chords, why)) break;
            } else if (type == "key") {
                GuestInput::Chord chord;
                if (!GuestInput::chordForKeys(a["keys"].toString(), chord, why) || !keys({chord}, why)) break;
            } else if (type == "wait") {
                QThread::msleep(ulong(std::clamp(a.value("seconds", 1.0).toDouble(), 0.0, 10.0) * 1000));
            } else { why = "Unknown action “" + type + "”. Use click, double_click, move, drag, scroll, type, key or wait."; break; }
            doneSteps << type;
        }
        if (!why.isEmpty()) { done(false, QString("Stopped at action %1 (%2): %3").arg(doneSteps.size() + 1).arg(actions.value(doneSteps.size()).toMap()["type"].toString(), why), {{"completed", doneSteps.size()}}); return; }
        done(true, QString("%1 action%2 sent.").arg(doneSteps.size()).arg(doneSteps.size() == 1 ? "" : "s"), {{"completed", doneSteps.size()}}); return;
    }
    if (op == "vm.agentExec") {
        // A command run through the QEMU guest agent (as root), which works without any network.
        if (!active) { done(false, "Start the VM first."); return; }
        QString ignored; const auto live = DomainConfig::describe(xmlOf(domain.get(), true), ignored);
        if (!live["agentConnected"].toBool()) { done(false, "The QEMU guest agent isn't running in this VM.", {{"noAgent", true}}); return; }
        auto agent = [&](const QJsonObject &command, int timeout, QJsonObject &result, QString &why) {
            char *reply = virDomainQemuAgentCommand(domain.get(), QJsonDocument(command).toJson(QJsonDocument::Compact).constData(), timeout, 0);
            if (!reply) { why = lastError("Guest agent"); return false; }
            result = QJsonDocument::fromJson(reply).object(); free(reply);
            return true;
        };
        QJsonObject reply; QString why;
        const bool windows = agent(QJsonObject{{"execute", "guest-get-osinfo"}}, 5, reply, why) && reply["return"].toObject()["id"].toString() == "mswindows";
        const auto command = in["command"].toString();
        const QJsonObject exec{{"execute", "guest-exec"}, {"arguments", windows
            ? QJsonObject{{"path", "powershell.exe"}, {"arg", QJsonArray{"-NoProfile", "-NonInteractive", "-Command", command}}, {"capture-output", true}}
            : QJsonObject{{"path", "/bin/sh"}, {"arg", QJsonArray{"-c", command}}, {"capture-output", true}}}};
        if (!agent(exec, 10, reply, why)) { done(false, why); return; }
        const int pid = reply["return"].toObject()["pid"].toInt(-1);
        if (pid < 0) { done(false, "The guest agent didn't start the command."); return; }
        QElapsedTimer clock; clock.start();
        const qint64 limit = std::clamp(in.value("timeout", 60).toInt(), 1, 900) * 1000LL;
        while (true) {
            if (!agent(QJsonObject{{"execute", "guest-exec-status"}, {"arguments", QJsonObject{{"pid", pid}}}}, 10, reply, why)) { done(false, why); return; }
            const auto status = reply["return"].toObject();
            if (status["exited"].toBool()) {
                auto decode = [&](const char *key) { const auto bytes = QByteArray::fromBase64(status[key].toString().toLatin1()); return QString::fromUtf8(bytes.right(65536)); };
                done(true, "Command finished.", {{"exitCode", status["exitcode"].toInt(status.contains("signal") ? 128 + status["signal"].toInt() : -1)},
                    {"stdout", decode("out-data")}, {"stderr", decode("err-data")}, {"via", "guest agent"}, {"truncated", status["out-truncated"].toBool() || status["err-truncated"].toBool()}}); return;
            }
            if (clock.elapsed() > limit) { done(false, QString("The command is still running after %1 seconds (process %2 in the guest).").arg(limit / 1000).arg(pid), {{"running", true}}); return; }
            QThread::msleep(250);
        }
    }
    if (op == "vm.addresses") {
        // IPv4 addresses for each adapter: DHCP leases of host networks, the host's neighbour table and the guest agent.
        QString ignored; const auto live = DomainConfig::describe(xmlOf(domain.get(), true), ignored);
        QHash<QString, QStringList> found;
        QHash<QString, QString> sources;   // per MAC, the first source that knew an address
        QString source = "dhcp_lease";
        auto add = [&](const QString &mac, const QString &ip) { auto &list = found[mac.toLower()]; if (!list.contains(ip)) list << ip; if (!sources.contains(mac.toLower())) sources[mac.toLower()] = source; };
        if (active) {
            Connection system(virConnectOpenReadOnly("qemu:///system"), virConnectClose);
            virNetworkPtr *nets = nullptr; const int count = system ? virConnectListAllNetworks(system.get(), &nets, VIR_CONNECT_LIST_NETWORKS_ACTIVE) : 0;
            for (int i = 0; i < count; ++i) {
                virNetworkDHCPLeasePtr *leases = nullptr; const int n = virNetworkGetDHCPLeases(nets[i], nullptr, &leases, 0);
                for (int j = 0; j < n; ++j) { if (leases[j]->mac && leases[j]->ipaddr && leases[j]->type == VIR_IP_ADDR_TYPE_IPV4) add(QString::fromUtf8(leases[j]->mac), QString::fromUtf8(leases[j]->ipaddr)); virNetworkDHCPLeaseFree(leases[j]); }
                free(leases); virNetworkFree(nets[i]);
            }
            free(nets);
            for (const unsigned from : {unsigned(VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP), unsigned(VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT)}) {
                if (from == VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT && !live["agentConnected"].toBool()) continue;
                source = from == VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT ? "guest_agent" : "arp";
                virDomainInterfacePtr *ifaces = nullptr; const int n = virDomainInterfaceAddresses(domain.get(), &ifaces, from, 0);
                for (int j = 0; j < n; ++j) {
                    for (unsigned k = 0; k < ifaces[j]->naddrs; ++k)
                        if (ifaces[j]->hwaddr && ifaces[j]->addrs[k].type == VIR_IP_ADDR_TYPE_IPV4 && !QString::fromUtf8(ifaces[j]->addrs[k].addr).startsWith("127.")) add(QString::fromUtf8(ifaces[j]->hwaddr), QString::fromUtf8(ifaces[j]->addrs[k].addr));
                    virDomainInterfaceFree(ifaces[j]);
                }
                free(ifaces);
            }
        }
        QVariantList nics;
        for (const auto &v : live["interfaces"].toList()) {
            auto nic = v.toMap();
            const auto mac = nic["mac"].toString().toLower();
            nics.append(QVariantMap{{"mac", nic["mac"]}, {"type", nic["type"]}, {"source", nic["source"]}, {"linkUp", nic["linkUp"]}, {"ips", found.value(mac)}, {"ipSource", found.value(mac).isEmpty() ? QString("unknown") : sources.value(mac)}});
        }
        done(true, "Addresses read.", {{"uuid", uuid}, {"active", active}, {"agent", live["agentConnected"]}, {"interfaces", nics}}); return;
    }
    if (op == "vm.restart") {
        if (const auto blocked = Containment::blocker(domain.get(), false); !blocked.isEmpty()) { done(false, blocked); return; }
        if (!active) { int r = start(domain.get()); done(r == 0, r == 0 ? "VM started with its saved settings." : lastError("Start VM")); return; }
        if (virDomainShutdown(domain.get()) < 0) { done(false, lastError("Request guest shutdown")); return; }
        restartUuid_ = uuid; restartTicks_ = 0;
        if (!restartTimer_) {
            restartTimer_ = new QTimer(this); restartTimer_->setInterval(1000);
            connect(restartTimer_, &QTimer::timeout, this, [this] {
                Domain d(virDomainLookupByUUIDString(conn_, restartUuid_.toUtf8().constData()), virDomainFree);
                if (!d || ++restartTicks_ >= 120) { restartTimer_->stop(); restartUuid_.clear(); emit finished("The guest did not shut down in time. Check its console; no forced power-off was performed.", false); return; }
                if (virDomainIsActive(d.get()) == 0) {
                    restartTimer_->stop(); restartUuid_.clear();
                    const auto blocked = Containment::blocker(d.get(), false);
                    int result = owned(d.get()) && blocked.isEmpty() ? start(d.get()) : -1;
                    refresh(); emit finished(result == 0 ? "VM started with its saved settings." : !blocked.isEmpty() ? blocked : lastError("Start VM after shutdown"), result == 0);
                }
            });
        }
        restartTimer_->start(); emit progress("Waiting for the guest to shut down before starting it with saved settings…"); emit managed(op, true, {{"waiting", true}, {"requestTag", in.value("requestTag")}}); return;
    }
    if (op == "containment.set") {
        const bool on = in["enabled"].toBool();
        const auto config = xmlOf(domain.get());
        if (on) {
            auto verdict = Containment::check(config);
            if (active) { const auto live = Containment::check(xmlOf(domain.get(), true)); auto list = verdict["violations"].toStringList() + live["violations"].toStringList(); list.removeDuplicates(); verdict["violations"] = list; }
            if (!verdict["violations"].toStringList().isEmpty()) { done(false, "Fix these before containing this VM: " + Containment::summary(verdict)); return; }
            // Containment is enforced by OmaWare; libvirt autostart would start the VM without that check.
            virDomainSetAutostart(domain.get(), 0);
        }
        const unsigned flags = VIR_DOMAIN_AFFECT_CONFIG | (active ? VIR_DOMAIN_AFFECT_LIVE : 0);
        if (virDomainSetMetadata(domain.get(), VIR_DOMAIN_METADATA_ELEMENT, on ? "<containment enabled='yes'/>" : nullptr, on ? "containment" : nullptr, Containment::ns, flags) < 0) { done(false, lastError("Update containment")); return; }
        PendingChanges changes(uuid); QString ignored;
        if (!changes.baseline().isEmpty()) changes.saved(xmlOf(domain.get()), ignored);
        done(true, on ? "VM contained: only verified isolated switches, no shared folders, clipboard or passthrough." : "Containment removed. This VM can now be given internet or host access.", {{"uuid", uuid}}); return;
    }
    PendingChanges pending(uuid);
    if (op.startsWith("snapshots.")) {
        const auto checkpointRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + uuid;
        if (!QDir().mkpath(checkpointRoot)) { done(false, "Could not open checkpoint storage."); return; }
        QLockFile checkpointLock(checkpointRoot + "/.operation.lock"); checkpointLock.setStaleLockTime(0);
        if (!checkpointLock.tryLock()) { done(false, "Another OmaWare checkpoint operation is active for this VM."); return; }
        auto report = [&](QString phase, qulonglong completed, qulonglong total) {
            const bool cancellable = phase != "Saving VM memory and device state" && phase != "Restoring saved memory and devices" && phase != "Restarting the VM from the checkpoint" && phase != "Registering the new VM" && phase != "Switching VM configuration" && phase != "Deleting snapshots and checking unused storage";
            emit checkpointProgress({{"active", true}, {"uuid", uuid}, {"operation", op}, {"phase", phase}, {"completed", completed}, {"total", total}, {"cancellable", cancellable}});
            emit progress(phase + "…");
        };
        QString failure;
        if (op == "snapshots.edit") { bool ok = Checkpoints::edit(uuid, in["id"].toString(), in, failure); done(ok, ok ? "Checkpoint details saved." : failure); return; }
        if (op == "snapshots.verify") { bool ok = Checkpoints::verify(uuid, in["id"].toString(), checkpointCancel_, report, failure); done(ok, ok ? "Checkpoint and its disk dependencies verified." : failure); return; }
        if (op == "snapshots.cleanup") { bool ok = Checkpoints::cleanup(conn_, uuid, in["key"].toString(), failure); done(ok, ok ? "Unused checkpoint storage removed." : failure); return; }
        if (op == "snapshots.recover") {
            const auto oldId = virDomainGetID(domain.get());
            bool ok = Checkpoints::recover(conn_, domain.get(), failure);
            Domain current(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
            done(ok, ok ? "Interrupted checkpoint work reconciled. Refresh storage to review retained files." : failure, {{"uuid", uuid}, {"restarted", current && virDomainIsActive(current.get()) == 1 && virDomainGetID(current.get()) != oldId}}); return;
        }
        if (op == "snapshots.clone") {
            const auto copy = Checkpoints::clone(conn_, uuid, in["id"].toString(), in["name"].toString(), checkpointCancel_, report, failure);
            if (!copy.isEmpty()) emit created(copy);
            done(!copy.isEmpty(), copy.isEmpty() ? failure : "VM created from checkpoint, stopped with its adapters disconnected.", {{"uuid", copy}}); return;
        }
        const bool undo = op == "snapshots.undo";
        if (undo) {
            in["id"] = Checkpoints::undoId(uuid); in["safety"] = false;
            if (in["id"].toString().isEmpty()) { done(false, "There is no previous restore to undo."); return; }
            op = "snapshots.restore";
        }
        // Deletion publishes history changes first and reclaims only unused
        // copies; the VM's current working disks are never changed.
        if (op == "snapshots.remove" && !in["id"].toString().isEmpty()) {
            report("Deleting snapshots and checking unused storage", 0, 0);
            bool ok = Checkpoints::remove(conn_, uuid, in["id"].toString(), failure, in["descendants"].toBool(), in["allowPinned"].toBool(), in["expectedIds"].toStringList());
            done(ok, ok ? "Snapshot selection deleted. The current VM is unchanged; disk files still needed by other snapshots are retained in Storage." : failure, {{"uuid", uuid}}); return;
        }
        if (active && op == "snapshots.remove") { done(false, "Shut down the VM before deleting a legacy internal snapshot."); return; }
        QVariantMap selectedPoint;
        for (auto value : Checkpoints::list(uuid)) if (value.toMap()["id"] == in["id"]) selectedPoint = value.toMap();
        const bool savedMemory = !selectedPoint["memory"].toString().isEmpty();
        if (active && op == "snapshots.restore" && !in["allowRestart"].toBool()) { done(false, savedMemory ? "Confirm Restore state to return to the snapshot's memory and disks." : "Confirm Restore & restart: this disk-only snapshot has no saved memory."); return; }
        auto blocker = snapshotBlocker(xml, active && op == "snapshots.create");
        if (!blocker.isEmpty()) { done(false, blocker); return; }
        if (!pending.items(xml).isEmpty() || !pending.matches(xml)) { done(false, "Apply or discard pending settings before managing checkpoints."); return; }
        if (op == "snapshots.create") {
            if (!active && in["memory"].toBool()) { done(false, "A stopped VM has no running memory to save. Start the VM or choose a disk-only snapshot."); return; }
            auto name = in["name"].toString().trimmed();
            if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9 _.-]{0,63}$").match(name).hasMatch()) { done(false, "Choose a checkpoint name using 1–64 letters, numbers, spaces, dots or dashes."); return; }
            Snapshot duplicate(virDomainSnapshotLookupByName(domain.get(), name.toUtf8().constData(), 0), virDomainSnapshotFree);
            if (duplicate) { done(false, "A checkpoint with this name already exists."); return; }
            for (auto v : Checkpoints::list(uuid)) if (v.toMap()["name"] == name) { done(false, "A checkpoint with this name already exists."); return; }
            QString parseError;
            if (virDomainSnapshotNum(domain.get(), 0) != 0) { done(false, "Remove existing internal snapshots before switching to independent disk-and-firmware checkpoints."); return; }
            bool ok;
            if (active) {
                report("Preparing live checkpoint", 0, 0);
                ok = Checkpoints::createLive(domain.get(), xml, name, in["notes"].toString().left(4096), checkpointCancel_, report, parseError, in);
            } else {
                emit progress("Copying stopped VM disks and firmware into an independent checkpoint…");
                ok = Checkpoints::create(domain.get(), xml, name, in["notes"].toString().left(4096), parseError, in, &checkpointCancel_, report);
            }
            done(ok, ok ? (active && in["memory"].toBool() ? "Snapshot saved with memory, CPU/device state and disks. Restore resumes this captured state." : active ? "Disk-only snapshot created. Restoring it requires a fresh boot." : "Disk-only snapshot created from the stopped VM.") : parseError); return;
        }
        if (!in["id"].toString().isEmpty()) {
            QString failure; bool ok = false;
            const auto originalId = virDomainGetID(domain.get());
            if (op == "snapshots.restore" && virDomainSnapshotNum(domain.get(), 0) != 0) { done(false, "Remove existing internal snapshots before restoring independent disk-and-firmware copies."); return; }
            if (op == "snapshots.restore") {
                ok = Checkpoints::restore(conn_, domain.get(), xml, in["id"].toString(), failure, in["allowRestart"].toBool(), report, &checkpointCancel_, in.value("safety", true).toBool());
            }
            Domain current(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
            const bool restarted = current && virDomainIsActive(current.get()) == 1 && virDomainGetID(current.get()) != originalId;
            if (undo) op = "snapshots.undo";
            done(ok, ok ? (savedMemory ? "Saved memory and disks restored without rebooting the guest OS." : active ? "Disk-only snapshot restored. The guest was rebooted because this snapshot has no memory." : "Disk-only snapshot restored. The VM remains stopped.") : failure, {{"uuid", uuid}, {"restarted", restarted}, {"restoredMemory", ok && savedMemory}, {"currentId", ok ? Checkpoints::history(uuid)["currentId"] : QVariant{}}}); return;
        }
        Snapshot snap(virDomainSnapshotLookupByName(domain.get(), in["name"].toString().toUtf8().constData(), 0), virDomainSnapshotFree);
        if (!snap) { done(false, lastError("Find checkpoint")); return; }
        char *raw = virDomainSnapshotGetXMLDesc(snap.get(), 0); QDomDocument doc; doc.setContent(raw ? QString::fromUtf8(raw) : QString{}); free(raw);
        bool internal = doc.documentElement().firstChildElement("state").text() == "shutoff";
        for (auto d = doc.documentElement().firstChildElement("disks").firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk")) if (d.attribute("snapshot") == "external") internal = false;
        if (!internal) { done(false, "Only stopped-VM internal checkpoints can be restored or deleted in this workflow."); return; }
        int result = -1;
        const auto originalId = virDomainGetID(domain.get());
        if (op == "snapshots.restore") {
            int state = 0, reason = 0;
            if (virDomainGetState(domain.get(), &state, &reason, 0) < 0) { done(false, lastError("Read VM state before restoration")); return; }
            if (state != VIR_DOMAIN_SHUTOFF && state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED) { done(false, "Wait until the VM is running, paused or stopped before restoring."); return; }
            unsigned flags = 0;
            if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
                if (!in["allowRestart"].toBool()) { done(false, "Confirm Restore & restart before restoring an active VM."); return; }
                virDomainJobInfo job{};
                if (virDomainGetJobInfo(domain.get(), &job) < 0 || job.type != VIR_DOMAIN_JOB_NONE) { done(false, "Another VM job is active or its status could not be read. Try again after it finishes."); return; }
                flags = state == VIR_DOMAIN_PAUSED ? VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED : VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING;
            }
            emit progress("Restoring the legacy checkpoint and returning the VM to its prior power state…");
            result = virDomainRevertToSnapshot(snap.get(), flags);
        }
        else if (op == "snapshots.remove") {
            if (in["descendants"].toBool()) {
                QSet<QString> deleting{"internal:" + in["name"].toString()};
                virDomainSnapshotPtr *children = nullptr;
                const int count = virDomainSnapshotListAllChildren(snap.get(), &children, VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS);
                if (count < 0) { done(false, lastError("Read snapshot branch")); return; }
                for (int i = 0; i < count; ++i) { deleting.insert("internal:" + QString::fromUtf8(virDomainSnapshotGetName(children[i]))); virDomainSnapshotFree(children[i]); }
                free(children); const auto expected = in["expectedIds"].toStringList();
                if (deleting != QSet<QString>(expected.cbegin(), expected.cend())) { done(false, "This branch changed. Refresh and review its snapshots before deleting it."); return; }
            }
            result = virDomainSnapshotDelete(snap.get(), in["descendants"].toBool() ? VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN : 0);
        }
        failure = result == 0 ? QString{} : lastError("Update checkpoint");
        Domain current(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
        const bool restarted = active && current && virDomainIsActive(current.get()) == 1 && virDomainGetID(current.get()) != originalId;
        done(result == 0, result == 0 ? "Checkpoint operation completed." : failure, {{"uuid", uuid}, {"restarted", restarted}}); return;
    }
    if (in["revision"].toString() != DomainConfig::revision(xml)) { done(false, "The VM configuration changed. Refresh Details and review your changes again."); return; }
    QString failure, updated, newDisk;
    if (op == "hardware.save") {
        virNodeInfo host{}; virNodeGetInfo(conn_, &host);
        if (in.value("cpus", 1).toUInt() > std::max(1u, host.cpus) || in.value("memoryMiB", 256).toULongLong() > host.memory / 1024) { done(false, "The requested CPU or RAM exceeds this host's available capacity."); return; }
        if (in.value("agentRequest").toBool() && in.contains("iso") && !in["iso"].toString().isEmpty()) {
            const QFileInfo iso(in["iso"].toString());
            if (iso.isSymLink() || !iso.isFile() || iso.canonicalPath() != QFileInfo(Paths::isos()).canonicalFilePath() || !iso.fileName().endsWith(".iso", Qt::CaseInsensitive)) { done(false, "Agents can only attach regular ISO files from OmaWare's local library."); return; }
        }
        updated = Configuration::hardware(xml, in, failure);
    } else if (op == "pending.discard") {
        if (pending.baseline().isEmpty() || !pending.matches(xml)) { done(false, "The pending-change record no longer matches this VM. Refresh and reconcile its configuration first."); return; }
        if (in["key"] == "all") updated = pending.baseline();
        else updated = Configuration::revert(pending.baseline(), xml, in["key"].toString(), failure);
    } else if (op == "pending.accept") {
        pending.clear(); done(true, "Current saved settings accepted. They will be used on the next full start."); return;
    } else if (op == "disk.add") {
        int size = in["diskGiB"].toInt();
        if (size < 1 || size > 2048) { done(false, "Choose a disk size of 1–2048 GiB."); return; }
        auto directory = Paths::vmDir(uuid);
        if (!QDir().mkpath(directory) || QStorageInfo(directory).bytesAvailable() < qint64(size) * 1073741824) { done(false, "There is not enough free space for the new disk."); return; }
        auto path = directory + "/disk-" + QUuid::createUuid().toString(QUuid::Id128) + ".qcow2";
        QByteArray output;
        if (!run("qemu-img", {"create", "-f", "qcow2", path, QString::number(size) + "G"}, output, failure)) { done(false, failure); return; }
        newDisk = path;
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
        QDomDocument doc; doc.setContent(xml); auto devices = doc.documentElement().firstChildElement("devices");
        QSet<QString> used;
        for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk")) used.insert(d.firstChildElement("target").attribute("dev"));
        QString target; for (char c = 'a'; c <= 'z'; ++c) if (!used.contains("vd" + QString(QChar(c)))) { target = "vd" + QString(QChar(c)); break; }
        if (target.isEmpty()) { QFile::remove(path); done(false, "No free virtio disk target is available."); return; }
        auto disk = child(doc, devices, "disk"); disk.setAttribute("type", "file"); disk.setAttribute("device", "disk");
        auto driver = child(doc, disk, "driver"); driver.setAttribute("name", "qemu"); driver.setAttribute("type", "qcow2");
        child(doc, disk, "source").setAttribute("file", path); auto t = child(doc, disk, "target"); t.setAttribute("dev", target); t.setAttribute("bus", "virtio");
        updated = doc.toString(-1);
    } else if (op == "disk.detach") {
        QDomDocument doc; doc.setContent(xml); auto devices = doc.documentElement().firstChildElement("devices");
        for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk")) if (d.firstChildElement("target").attribute("dev") == in["target"].toString()) { devices.removeChild(d); updated = doc.toString(-1); break; }
        if (updated.isEmpty()) failure = "The selected disk is no longer attached.";
    } else { done(false, "Unknown VM operation."); return; }
    if (!failure.isEmpty()) { if (!newDisk.isEmpty()) QFile::remove(newDisk); done(false, failure); return; }
    if (Containment::enabled(xml)) {
        // Edits keep the policy marker and must stay within it.
        updated = Containment::withMarker(updated, true);
        const auto verdict = Containment::check(updated);
        if (!verdict["violations"].toStringList().isEmpty()) failure = "This VM is contained: " + Containment::summary(verdict);
    }
    if (!failure.isEmpty()) { if (!newDisk.isEmpty()) QFile::remove(newDisk); done(false, failure); return; }
    if (op == "hardware.save" && in.value("dryRun").toBool()) { done(true, "Validated without changing the VM.", {{"dry_run", true}, {"changes", Configuration::changes(xml, updated)}, {"revision", DomainConfig::revision(xml)}, {"requires_restart", active}}); return; }
    if ((active || !pending.baseline().isEmpty()) && !pending.prepare(xml, failure)) { if (!newDisk.isEmpty()) QFile::remove(newDisk); done(false, failure); return; }
    Domain saved(virDomainDefineXMLFlags(conn_, updated.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE), virDomainFree);
    if (!saved) { auto error = lastError("Save VM configuration"); if (!newDisk.isEmpty()) QFile::remove(newDisk); done(false, error); return; }
    if (!pending.baseline().isEmpty()) {
        const auto canonical = xmlOf(saved.get());
        if (!pending.saved(canonical, failure)) { done(false, "Settings saved, but " + failure); return; }
        if (pending.items(canonical).isEmpty()) pending.clear();
    }
    done(true, active ? "Settings saved for the next full shutdown and start." : "VM settings saved.", {{"revision", DomainConfig::revision(xmlOf(saved.get()))}, {"requires_restart", active}});
}

// ---- Unattended setup (see unattended.h) -----------------------------------------------------
namespace {
QDomElement setupMarker(virDomainPtr d, QDomDocument &doc) {
    char *raw = virDomainGetMetadata(d, VIR_DOMAIN_METADATA_ELEMENT, Unattended::ns, VIR_DOMAIN_AFFECT_CONFIG);
    if (!raw) { virResetLastError(); return {}; }
    doc.setContent(QString::fromUtf8(raw)); free(raw);
    return doc.documentElement();
}
void saveMarker(virDomainPtr d, const QDomDocument &doc) {
    virDomainSetMetadata(d, VIR_DOMAIN_METADATA_ELEMENT, doc.toString(-1).toUtf8().constData(), "omasetup", Unattended::ns, VIR_DOMAIN_AFFECT_CONFIG);
}
}
int VmWorker::start(virDomainPtr d) {
    QDomDocument marker;
    auto setup = setupMarker(d, marker);
    if (setup.isNull()) return virDomainCreate(d);
    const auto kind = setup.attribute("kind");
    const bool first = setup.attribute("state") == "new";
    if (first) { setup.setAttribute("state", "installing"); saveMarker(d, marker); }
    if (kind == "subiquity") {
        // Ubuntu's installer only runs without its "Continue with autoinstall?" question when "autoinstall"
        // is on the kernel command line: boot its kernel directly, for this run only. The saved definition
        // stays as it is, and the installer powers the VM off when it's done.
        QDomDocument doc; doc.setContent(xmlOf(d));
        auto os = doc.documentElement().firstChildElement("os");
        child(doc, os, "kernel", setup.attribute("kernel")); child(doc, os, "initrd", setup.attribute("initrd")); child(doc, os, "cmdline", "autoinstall ---");
        auto live = virDomainCreateXML(conn_, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_NONE);
        if (!live) return -1;
        virDomainFree(live);
        return 0;
    }
    const int result = virDomainCreate(d);
    // Windows on UEFI asks to "Press any key to boot from CD or DVD" the first time. Enter answers that, and
    // also picks "Windows Setup" if a later key lands on the Windows Boot Manager menu (a key stops its countdown).
    if (result == 0 && first && kind == "windows" && setup.attribute("uefi") == "1") {
        char uuid[VIR_UUID_STRING_BUFLEN];
        if (virDomainGetUUIDString(d, uuid) == 0) pressKeys(QString::fromLatin1(uuid), 12);
    }
    return result;
}
void VmWorker::pressKeys(const QString &uuid, int times) {
    if (times <= 0 || !conn_) return;
    QTimer::singleShot(700, this, [this, uuid, times] {
        if (!conn_) return;
        Domain d(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
        if (!d || virDomainIsActive(d.get()) != 1) return;
        unsigned int enter = 28;   // KEY_ENTER
        virDomainSendKey(d.get(), VIR_KEYCODE_SET_LINUX, 60, &enter, 1, 0);
        pressKeys(uuid, times - 1);
    });
}
void VmWorker::finishSetup(const QString &uuid, int detail) {
    if (storageOnly_ || !conn_) return;
    Domain d(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
    if (!d || virDomainIsActive(d.get()) == 1 || !owned(d.get())) { virResetLastError(); return; }
    QDomDocument marker;
    const auto setup = setupMarker(d.get(), marker);
    // Only when the guest itself switched off: the installer when it's done (Linux), or you shutting down a
    // Windows VM that finished setting up. A forced stop leaves everything in place for the next start.
    if (setup.isNull() || setup.attribute("state") != "installing" || detail != VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN) return;
    const auto kind = setup.attribute("kind"), answers = setup.attribute("answers"), media = setup.attribute("media");
    QDomDocument doc; doc.setContent(xmlOf(d.get()));
    auto root = doc.documentElement(), devices = root.firstChildElement("devices");
    // Take out the answers disc and the installation media, so the VM boots its new system.
    auto disks = devices.elementsByTagName("disk");
    for (int i = disks.size() - 1; i >= 0; --i) {
        auto e = disks.at(i).toElement();
        if (e.attribute("device") != "cdrom") continue;
        const auto file = e.firstChildElement("source").attribute("file");
        if (file == answers) devices.removeChild(e);
        else if (file == media) e.removeChild(e.firstChildElement("source"));
    }
    auto metadata = root.firstChildElement("metadata");
    for (auto e = metadata.firstChildElement(); !e.isNull();) { auto next = e.nextSiblingElement(); if (e.namespaceURI() == Unattended::ns || e.tagName().endsWith(":setup")) metadata.removeChild(e); e = next; }
    const auto name = QString::fromUtf8(virDomainGetName(d.get())).remove(QRegularExpression("^omaware-"));
    Domain saved(virDomainDefineXMLFlags(conn_, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE), virDomainFree);
    if (!saved) { emit finished(lastError("Finish the unattended setup of " + name), false); return; }
    for (const auto &attribute : {"answers", "kernel", "initrd"}) if (!setup.attribute(attribute).isEmpty()) QFile::remove(setup.attribute(attribute));
    if (kind == "windows") { emit finished(name + " finished setting up: OmaWare removed its answer disc and the Windows installation media.", true); return; }
    const int started = virDomainCreate(saved.get());
    emit finished(started == 0 ? name + " is installed and starting its new system." : lastError(name + " is installed, but starting it failed"), started == 0);
}
