// SPDX-License-Identifier: GPL-3.0-or-later
// VmWorker::manage(): the request plumbing shared by every operation, its dispatch, and statistics.
// The operations themselves live in hostnetworks.cpp, vmcreation.cpp, vmoperations.cpp and
// snapshotoperations.cpp.
#include "management.h"
#include "agentprovision.h"
#include "containment.h"
#include "domainconfig.h"
#include "networkcatalog.h"
#include "workspace.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <unistd.h>

namespace Management {
bool run(const QString &program, const QStringList &args, QByteArray &output, QString &error, int timeout) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(5000)) {
        error = "Cannot start " + program + ". Check that it is installed.";
        return false;
    }
    if (!process.waitForFinished(timeout)) {
        process.kill();
        process.waitForFinished(3000);
        error = program + " did not finish within the operation timeout.";
        return false;
    }
    output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = program + ": " + QString::fromUtf8(process.readAllStandardError()).left(6000);
        return false;
    }
    return true;
}

QDomElement child(QDomDocument &doc, QDomElement parent, const QString &name, const QString &content) {
    auto e = doc.createElement(name);
    if (!content.isNull()) e.appendChild(doc.createTextNode(content));
    parent.appendChild(e);
    return e;
}

bool managedNetwork(const QString &xml) {
    QDomDocument doc;
    if (!doc.setContent(xml, true)) return false;
    return doc.documentElement().firstChildElement("name").text().startsWith("omaware-") &&
           doc.elementsByTagNameNS("https://omaware.org/xmlns/network/1", "managed").count() == 1;
}

QHash<QString, QStringList> dhcpLeases(virConnectPtr system) {
    QHash<QString, QStringList> leases;
    virNetworkPtr *networks = nullptr;
    const int count = system ? virConnectListAllNetworks(system, &networks, VIR_CONNECT_LIST_NETWORKS_ACTIVE) : 0;
    for (int i = 0; i < count; ++i) {
        virNetworkDHCPLeasePtr *list = nullptr;
        const int n = virNetworkGetDHCPLeases(networks[i], nullptr, &list, 0);
        for (int j = 0; j < n; ++j) {
            if (list[j]->mac && list[j]->ipaddr && list[j]->type == VIR_IP_ADDR_TYPE_IPV4)
                leases[QString::fromUtf8(list[j]->mac).toLower()] << QString::fromUtf8(list[j]->ipaddr);
            virNetworkDHCPLeaseFree(list[j]);
        }
        free(list);
        virNetworkFree(networks[i]);
    }
    free(networks);
    return leases;
}
}

namespace {
// Host networks as the provisioning checks see them: identity, ownership, state and whether VMs may join.
bool provisioningNetworks(virConnectPtr system, virConnectPtr session, QVariantList &networks) {
    QString status;
    const auto choices = NetworkCatalog::discover(session, status);
    virNetworkPtr *all = nullptr;
    const int count = virConnectListAllNetworks(system, &all, 0);
    if (count < 0) return false;
    for (int i = 0; i < count; ++i) {
        const auto xml = Virt::networkXml(all[i]);
        auto network = NetworkCatalog::describe(xml);
        char uuid[VIR_UUID_STRING_BUFLEN];
        virNetworkGetUUIDString(all[i], uuid);
        network["uuid"] = QString::fromUtf8(uuid);
        network["managed"] = Management::managedNetwork(xml);
        network["active"] = virNetworkIsActive(all[i]) == 1;
        network["revision"] = DomainConfig::revision(xml);
        for (const auto &v : choices)
            if (v.toMap()["id"] == "bridge:" + network["bridge"].toString())
                network["available"] = v.toMap()["available"];
        networks.append(network);
        virNetworkFree(all[i]);
    }
    free(all);
    return true;
}

// Flattens libvirt typed parameters (bulk domain or node statistics) into numbers and strings.
QHash<QString, QVariant> typedParams(virTypedParameterPtr params, int count) {
    QHash<QString, QVariant> values;
    for (int i = 0; i < count; ++i) {
        const auto &param = params[i];
        const QString key = QString::fromUtf8(param.field);
        switch (param.type) {
        case VIR_TYPED_PARAM_STRING:
            values[key] = QString::fromUtf8(param.value.s);
            break;
        case VIR_TYPED_PARAM_ULLONG:
            values[key] = double(param.value.ul);
            break;
        case VIR_TYPED_PARAM_LLONG:
            values[key] = double(param.value.l);
            break;
        case VIR_TYPED_PARAM_UINT:
            values[key] = double(param.value.ui);
            break;
        case VIR_TYPED_PARAM_INT:
            values[key] = double(param.value.i);
            break;
        default:
            break;
        }
    }
    return values;
}

// The sum of one counter over every device of a kind ("block", "net").
double sumOf(const QHash<QString, QVariant> &params, const QString &prefix, const QString &field) {
    double total = 0;
    for (int i = 0; i < params.value(prefix + ".count").toInt(); ++i)
        total += params.value(prefix + "." + QString::number(i) + "." + field).toDouble();
    return total;
}

// One pass over /proc maps each QEMU process's -uuid to its uptime, so a fleet sample costs one scan
// instead of one scan per VM.
QHash<QString, double> qemuUptimes() {
    QHash<QString, double> result;
    QFile uptime("/proc/uptime");
    if (!uptime.open(QIODevice::ReadOnly)) return result;
    const double now = uptime.readAll().split(' ').value(0).toDouble();
    const double ticksPerSecond = double(sysconf(_SC_CLK_TCK));
    for (const auto &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!pid.front().isDigit()) continue;
        QFile cmd("/proc/" + pid + "/cmdline");
        if (!cmd.open(QIODevice::ReadOnly)) continue;
        const auto args = cmd.read(65536).split('\0');
        if (args.isEmpty() || args.first().isEmpty() ||
                !QFileInfo(QString::fromLocal8Bit(args.first())).fileName().startsWith("qemu-system-"))
            continue;
        const int flag = args.indexOf("-uuid");
        if (flag < 0) continue;
        QFile stat("/proc/" + pid + "/stat");
        if (!stat.open(QIODevice::ReadOnly)) continue;
        const auto line = stat.readAll();
        const auto fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
        bool ok;
        const double ticks = fields.value(19).toDouble(&ok);
        if (ok) result[QString::fromUtf8(args.value(flag + 1))] = std::max(0.0, now - ticks / ticksPerSecond);
    }
    return result;
}
}

void VmWorker::Request::done(bool ok, const QString &message, QVariantMap result) const {
    result["message"] = message;
    if (in.contains("requestTag")) result["requestTag"] = in["requestTag"];
    if (!query) {
        if (!worker.storageOnly_) worker.refresh();
        emit worker.finished(message, ok);
    }
    emit worker.managed(op, ok, result);
}

bool VmWorker::Request::stillAuthorized() const {
    if (!provisioning) return true;
    // Turning agent access off (or off and on again) changes the epoch, which revokes every earlier approval.
    const auto approvedEpoch = envelope["provisionEpoch"];
    const auto revoked = [&] {
        return !(worker.provisionEpoch() & 1) || approvedEpoch.toULongLong() != worker.provisionEpoch();
    };
    if (approvedEpoch.typeId() != QMetaType::ULongLong || revoked()) {
        fail("Agent provisioning authorization was revoked.");
        return false;
    }
    const auto tool = envelope["provisionTool"].toString();
    const auto expectedOp = tool == "create_vm"           ? "vm.create"
                            : tool == "create_network"    ? "networks.save"
                            : tool == "authorize_network" ? "networks.authorize"
                                                          : "";
    const auto args = envelope["provisionArgs"].toMap();
    QString error;
    if (!envelope["agentRequest"].toBool() || op != expectedOp || !AgentProvision::validate(tool, args, error) ||
            args.value("dry_run", false).toBool()) {
        fail("Invalid provisioning envelope or dry-run mutation.");
        return false;
    }
    Connection system(virConnectOpenReadOnly("qemu:///system"), virConnectClose);
    if (!system) {
        fail("Cannot recheck owned host networks.");
        return false;
    }
    QVariantList networks;
    if (!provisioningNetworks(system.get(), worker.conn_, networks)) {
        fail("Cannot recheck network inventory.");
        return false;
    }
    if (!AgentProvision::verifyEnvelope(op, envelope, networks, error)) {
        fail(error);
        return false;
    }
    // The checks take time; a revocation meanwhile must still win.
    if (revoked()) {
        fail("Agent provisioning authorization was revoked.");
        return false;
    }
    return true;
}

void VmWorker::cancelRestart() {
    if (restartUuid_.isEmpty()) return;
    if (restartTimer_) restartTimer_->stop();
    restartUuid_.clear();
    emit finished(
            "Automatic start cancelled. The shutdown request already sent to the guest cannot be withdrawn.", true);
}

void VmWorker::manage(QString op, QVariantMap in) {
    const bool query = op.endsWith(".list") || op.startsWith("stats") || op == "capabilities";
    // The envelope is copied before anything reads the input (see Request::in).
    Request request{*this, op, in, query, in.contains("provisionTool"), in, -1};
    if (op == "media.list") {
        // A folder problem belongs to this picker, not the global connection.
        return request.done(true, "ISO library refreshed", Workspace::mediaFiles(in["folder"].toString()));
    }
    // A storage-only worker has no event thread or close callback (see open()), so it notices a connection the
    // daemon dropped here and opens a new one instead of failing every request until the app restarts.
    if (storageOnly_ && conn_ && virConnectIsAlive(conn_) != 1) {
        virConnectClose(conn_);
        conn_ = nullptr;
    }
    if (!conn_ && storageOnly_) conn_ = virConnectOpen("qemu:///session");
    if (!conn_) return request.fail("Reconnect to the local VM session first.");

    const auto closeMedia = qScopeGuard([&] {
        if (request.mediaFd >= 0) ::close(request.mediaFd);
    });
    if (!request.stillAuthorized()) return;
    if (op == "vm.create" && in["agentRequest"].toBool() && in["sourceMode"] != "cloud" && !request.provisioning)
        return request.fail("Agent local VM creation requires a provisioning envelope.");
    if (request.provisioning && op == "vm.create") {
        // Pin the approved media file now, so it can't be swapped for another before it is copied.
        const auto args = request.envelope.value("provisionArgs").toMap();
        QVariantMap identity;
        QString error;
        request.mediaFd =
                AgentProvision::openMedia(args["media_kind"].toString(), args["media"].toString(), identity, error);
        if (request.mediaFd < 0 || identity != request.envelope.value("mediaIdentity").toMap())
            return request.fail("Approved media changed before worker dispatch.");
    }

    if (op == "stats.all") return fleetStats(request);
    if (op == "capabilities") return capabilities(request);
    if (op.startsWith("networks.")) return manageHostNetworks(request);
    if (op == "vm.create") return createVm(request);
    manageVm(request);
}

// Requests about one VM: looks it up, checks what may be done to it, and hands it to the operation's handler.
void VmWorker::manageVm(const Request &request) {
    const auto uuid = request.in["uuid"].toString();
    Domain domain(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
    if (!domain) return request.fail(Virt::lastError("Find VM"));
    if (request.fromAgent() && (!owned(domain.get()) || virDomainIsPersistent(domain.get()) != 1 ||
                                       Containment::enabled(Virt::definition(domain.get()))))
        return request.fail("Agents require an owned, persistent, non-contained VM.");
    Target vm{uuid, domain.get(), virDomainIsActive(domain.get()) == 1, {}};

    // Statistics and the snapshot list also work for VMs OmaWare didn't create.
    if (request.op == "stats") return vmStats(request, vm);
    vm.xml = Virt::definition(domain.get());
    if (request.op == "snapshots.list") return listSnapshots(request, vm);
    if (!owned(domain.get()) || virDomainIsPersistent(domain.get()) != 1)
        return request.fail("This operation requires an OmaWare-managed persistent VM.");
    // Agent requests are checked against the VM's current definition, not just the cached inventory.
    if (request.fromAgent() && Containment::enabled(vm.xml))
        return request.fail("Contained VMs cannot be accessed by agents.");

    using Handler = void (VmWorker::*)(const Request &, const Target &);
    static const QHash<QString, Handler> handlers{
            {"vm.readiness", &VmWorker::vmReadiness},
            {"vm.details", &VmWorker::vmDetails},
            {"adapter.save", &VmWorker::saveAdapter},
            {"vm.delete", &VmWorker::deleteVm},
            {"vm.power", &VmWorker::vmPower},
            {"vm.serial", &VmWorker::serialConsole},
            {"vm.screenshot", &VmWorker::screenshot},
            {"vm.input", &VmWorker::sendInput},
            {"vm.agentExec", &VmWorker::guestExec},
            {"vm.addresses", &VmWorker::guestAddresses},
            {"vm.restart", &VmWorker::restartVm},
            {"containment.set", &VmWorker::setContainment},
    };
    if (const auto handler = handlers.value(request.op)) return (this->*handler)(request, vm);
    if (request.op.startsWith("snapshots.")) return manageSnapshots(request, vm);
    editConfiguration(request, vm);
}

// "stats.all": one sample of every running VM and of the host. Rates are derived in the UI from
// consecutive samples.
void VmWorker::fleetStats(const Request &request) {
    QVariantList vms;
    virDomainStatsRecordPtr *records = nullptr;
    const unsigned groups = VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL | VIR_DOMAIN_STATS_BALLOON |
                            VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_INTERFACE | VIR_DOMAIN_STATS_BLOCK;
    const int count = virConnectGetAllDomainStats(conn_, groups, &records, VIR_CONNECT_GET_ALL_DOMAINS_STATS_ACTIVE);
    const auto uptimes = qemuUptimes();
    for (int i = 0; i < count; ++i) {
        const auto p = typedParams(records[i]->params, records[i]->nparams);
        const auto uuid = Virt::uuidOf(records[i]->dom);
        QVariantMap vm{{"uuid", uuid}, {"name", QString::fromUtf8(virDomainGetName(records[i]->dom))},
                {"state", p.value("state.state")}, {"cpuTime", p.value("cpu.time")}, {"vcpus", p.value("vcpu.current")},
                {"rdBytes", sumOf(p, "block", "rd.bytes")}, {"wrBytes", sumOf(p, "block", "wr.bytes")},
                {"rxBytes", sumOf(p, "net", "rx.bytes")}, {"txBytes", sumOf(p, "net", "tx.bytes")}};
        // Each adapter's counters, so the network map can show traffic per cable. `name` is the host tap
        // (matching the adapter's target), absent for some backends; `index` is libvirt's order.
        QVariantList nics;
        for (int n = 0; n < p.value("net.count").toInt(); ++n) {
            const auto key = "net." + QString::number(n) + ".";
            const double errors = p.value(key + "rx.errs").toDouble() + p.value(key + "tx.errs").toDouble() +
                                  p.value(key + "rx.drop").toDouble() + p.value(key + "tx.drop").toDouble();
            nics.append(QVariantMap{{"index", n}, {"name", p.value(key + "name").toString()},
                    {"rxBytes", p.value(key + "rx.bytes")}, {"txBytes", p.value(key + "tx.bytes")},
                    {"rxPkts", p.value(key + "rx.pkts")}, {"txPkts", p.value(key + "tx.pkts")}, {"errors", errors}});
        }
        vm["nics"] = nics;
        static const QList<std::pair<QString, QString>> balloon{{"balloon.current", "balloonKiB"},
                {"balloon.available", "availableKiB"}, {"balloon.unused", "unusedKiB"}, {"balloon.rss", "rssKiB"}};
        for (const auto &[field, key] : balloon)
            if (p.contains(field)) vm[key] = p.value(field);
        vm["uptimeSeconds"] = uptimes.value(uuid, -1);
        vms.append(vm);
    }
    if (records) virDomainStatsRecordListFree(records);

    QVariantMap host{{"sampledAt", double(QDateTime::currentMSecsSinceEpoch())}};
    virNodeInfo node{};
    if (virNodeGetInfo(conn_, &node) == 0) {
        host["cpus"] = node.cpus;
        host["memoryKiB"] = double(node.memory);
    }
    int n = 0;
    if (virNodeGetCPUStats(conn_, VIR_NODE_CPU_STATS_ALL_CPUS, nullptr, &n, 0) == 0 && n > 0) {
        std::vector<virNodeCPUStats> cpu(n);
        if (virNodeGetCPUStats(conn_, VIR_NODE_CPU_STATS_ALL_CPUS, cpu.data(), &n, 0) == 0)
            for (const auto &c : cpu)
                host["cpu_" + QString::fromUtf8(c.field)] = double(c.value);
    }
    n = 0;
    if (virNodeGetMemoryStats(conn_, VIR_NODE_MEMORY_STATS_ALL_CELLS, nullptr, &n, 0) == 0 && n > 0) {
        std::vector<virNodeMemoryStats> memory(n);
        if (virNodeGetMemoryStats(conn_, VIR_NODE_MEMORY_STATS_ALL_CELLS, memory.data(), &n, 0) == 0)
            for (const auto &m : memory)
                host["mem_" + QString::fromUtf8(m.field)] = double(m.value);
    }
    request.done(count >= 0, count >= 0 ? "Fleet sample" : Virt::lastError("Read VM statistics"),
            {{"vms", vms}, {"host", host}});
}

// "stats": a detailed sample of one VM, for its Monitor view.
void VmWorker::vmStats(const Request &request, const Target &vm) {
    QVariantMap result{{"uuid", vm.uuid}, {"active", vm.active}};
    virDomainInfo info{};
    if (!vm.active || virDomainGetInfo(vm.domain, &info) != 0) {
        cpuSamples_.remove(vm.uuid);
    } else {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto previous = cpuSamples_.value(vm.uuid);
        if (previous.second > 0 && now > previous.second && info.cpuTime >= previous.first)
            result["cpuPercent"] = std::clamp(double(info.cpuTime - previous.first) / double(now - previous.second) /
                                                      10000 / std::max(1u, unsigned(info.nrVirtCpu)),
                    0.0, 100.0);
        cpuSamples_[vm.uuid] = {info.cpuTime, now};

        virDomainMemoryStatStruct memory[VIR_DOMAIN_MEMORY_STAT_NR];
        const int n = virDomainMemoryStats(vm.domain, memory, VIR_DOMAIN_MEMORY_STAT_NR, 0);
        qulonglong total = 0, unused = 0;
        bool hasUnused = false;
        for (int i = 0; i < n; ++i) {
            const double value = double(memory[i].val);
            switch (memory[i].tag) {
            case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
                total = memory[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_UNUSED:
                unused = memory[i].val;
                hasUnused = true;
                break;
            case VIR_DOMAIN_MEMORY_STAT_RSS:
                result["hostRssMiB"] = value / 1024;
                break;
            case VIR_DOMAIN_MEMORY_STAT_AVAILABLE:
                result["guestTotalMiB"] = value / 1024;
                break;
            case VIR_DOMAIN_MEMORY_STAT_DISK_CACHES:
                result["cacheMiB"] = value / 1024;
                break;
            case VIR_DOMAIN_MEMORY_STAT_SWAP_IN:
                result["swapInKiB"] = value;
                break;
            case VIR_DOMAIN_MEMORY_STAT_SWAP_OUT:
                result["swapOutKiB"] = value;
                break;
            case VIR_DOMAIN_MEMORY_STAT_MAJOR_FAULT:
                result["majorFaults"] = value;
                break;
            default:
                break;
            }
        }
        if (total && hasUnused && unused <= total) {
            result["usedMiB"] = double(total - unused) / 1024;
            result["freeMiB"] = double(unused) / 1024;
        }
        if (total) result["balloonMiB"] = double(total) / 1024;
        result["uptimeSeconds"] = qemuUptimes().value(vm.uuid, -1);

        // Cumulative device counters; the UI derives rates from consecutive samples.
        virDomainPtr list[] = {vm.domain, nullptr};
        virDomainStatsRecordPtr *records = nullptr;
        const unsigned groups = VIR_DOMAIN_STATS_BLOCK | VIR_DOMAIN_STATS_INTERFACE | VIR_DOMAIN_STATS_VCPU;
        if (virDomainListGetStats(list, groups, &records, 0) > 0 && records[0]) {
            const auto p = typedParams(records[0]->params, records[0]->nparams);
            // One row per device, with the requested counters ("rd.bytes" becomes "rd_bytes").
            auto rows = [&](const QString &prefix, const QStringList &fields) {
                QVariantList out;
                for (int i = 0; i < p.value(prefix + ".count").toInt(); ++i) {
                    const auto base = prefix + "." + QString::number(i) + ".";
                    QVariantMap row;
                    for (const auto &field : fields)
                        if (p.contains(base + field)) row[QString(field).replace('.', '_')] = p[base + field];
                    out.append(row);
                }
                return out;
            };
            result["blocks"] = rows("block",
                    {"name", "rd.bytes", "wr.bytes", "rd.reqs", "wr.reqs", "allocation", "capacity", "physical"});
            result["nets"] = rows("net",
                    {"name", "rx.bytes", "tx.bytes", "rx.pkts", "tx.pkts", "rx.errs", "tx.errs", "rx.drop", "tx.drop"});
            QVariantList vcpus;
            for (int i = 0; i < p.value("vcpu.maximum").toInt(); ++i) {
                const auto base = "vcpu." + QString::number(i) + ".";
                if (p.contains(base + "time"))
                    vcpus.append(
                            QVariantMap{{"id", i}, {"time", p[base + "time"]}, {"state", p.value(base + "state")}});
            }
            result["vcpus"] = vcpus;
        }
        if (records) virDomainStatsRecordListFree(records);
    }
    result["sampledAt"] = double(QDateTime::currentMSecsSinceEpoch());
    virNodeInfo host{};
    if (virNodeGetInfo(conn_, &host) == 0) {
        result["hostCpus"] = host.cpus;
        result["hostMemoryMiB"] = double(host.memory) / 1024;
    }
    if (const auto freeBytes = virNodeGetFreeMemory(conn_)) result["hostFreeMiB"] = double(freeBytes) / 1048576;
    request.done(true, "Live resource sample", result);
}
