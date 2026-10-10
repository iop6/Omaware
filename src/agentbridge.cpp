// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentbridge.h"
#include "agentreply.h"
#include "agentprovision.h"
#include "backend.h"
#include "cloudimages.h"
#include "cloudseed.h"
#include "labplan.h"
#include "labs.h"
#include "labfile.h"
#include "logins.h"
#include "paths.h"
#include "mcpserver.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHostAddress>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <memory>

using AgentReply::failure;
using AgentReply::success;

namespace {
// Passes a worker operation's message on to the agent as the tool's result.
auto relayMessage(AgentBridge::Reply reply) {
    return [reply](bool ok, const QVariantMap &r) {
        reply(ok ? success({{"message", r["message"]}}) : failure(r["message"].toString()));
    };
}

QString shortName(const QString &name) {
    return QString(name).remove(QRegularExpression("^omaware-"));
}

QString randomMac() {
    auto r = [] {
        return QString("%1").arg(QRandomGenerator::system()->bounded(256), 2, 16, QChar('0'));
    };
    return "52:54:00:" + r() + ":" + r() + ":" + r();
}

// Runs ssh-keygen for a new ed25519 key without a passphrase; returns the public key line.
QString makeKey(const QString &path, const QString &comment, QString &error) {
    QFile::remove(path);
    QFile::remove(path + ".pub");
    QProcess p;
    p.start("ssh-keygen", {"-q", "-t", "ed25519", "-N", "", "-C", comment, "-f", path});
    if (!p.waitForStarted(5000) || !p.waitForFinished(20000) || p.exitCode() != 0) {
        error = "Couldn't make an SSH key. Is ssh-keygen (OpenSSH) installed?";
        return {};
    }
    QFile pub(path + ".pub");
    if (!pub.open(QIODevice::ReadOnly)) {
        error = "Couldn't read the new SSH key.";
        return {};
    }
    return QString::fromUtf8(pub.readAll()).trimmed();
}

QString readText(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString{};
}
}

AgentBridge::AgentBridge(Backend *backend, QObject *parent)
    : QObject(parent), backend_(backend), images_(new CloudImages(this)),
      agentWorker_(new VmWorker("qemu:///session", true)) {
    agentWorker_->moveToThread(&agentThread_);
    connect(&agentThread_, &QThread::finished, agentWorker_, &QObject::deleteLater);
    auto finished = [this](QString, bool ok, QVariantMap result) {
        const auto tag = result.value("requestTag").toString();
        if (tag.isEmpty() || !pending_.contains(tag)) return;
        const auto done = pending_.take(tag);
        done(ok, result);
    };
    connect(agentWorker_, &VmWorker::managed, this, finished);
    connect(backend_, &Backend::commandFinished, this, finished);
    connect(backend_, &Backend::linksSet, this, [this](bool, QString) {
        if (!linkWaiters_.isEmpty()) linkWaiters_.takeFirst()();
    });
    connect(images_, &CloudImages::changed, this, &AgentBridge::reportBuild);
    agentThread_.start();
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, &AgentBridge::accept);
    enabled_ = QSettings().value("agent/enabled", false).toBool();
    backend_->setProvisionAccess(enabled_);
    if (enabled_) listen();
}

AgentBridge::~AgentBridge() {
    backend_->setProvisionAccess(false);
    images_->cancelAll();
    server_.close();
    QLocalServer::removeServer(socketPath());
    QMetaObject::invokeMethod(agentWorker_, &VmWorker::stop, Qt::BlockingQueuedConnection);
    agentThread_.quit();
    agentThread_.wait();
}

QString AgentBridge::socketPath() {
    // Only in the user's private runtime folder: in a shared one, another user could take the name first and
    // answer an agent's tool calls.
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return dir.isEmpty() ? QString{} : dir + "/omaware-agent.sock";
}

QString AgentBridge::command() const {
    // The installed launcher sets up the app's libraries; a development build is run directly.
    QString path = QDir::homePath() + "/.local/bin/omaware";
    if (!QFile::exists(path) || !QCoreApplication::applicationDirPath().startsWith(Paths::root())) {
        const auto launcher = QCoreApplication::applicationDirPath() + "/omaware.sh";
        path = QFile::exists(launcher) ? launcher : QCoreApplication::applicationFilePath();
    }
    return "claude mcp add omaware -- " + (path.contains(' ') ? "'" + path + "'" : path) + " mcp";
}

void AgentBridge::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    backend_->setProvisionAccess(on);
    QSettings().setValue("agent/enabled", on);
    if (on)
        listen();
    else {
        // Disabling access revokes unanswered approvals, even if access is enabled
        // again later. Decline while enabled_ is false so callbacks cannot dispatch.
        const auto questions = questions_.keys();
        for (const auto &id : questions)
            answer(id, false);
        revokeGrants();
        if (!proposal_.isEmpty() && !localLab_) decline(proposal_["id"].toString());
        server_.close();
        QLocalServer::removeServer(socketPath());
        for (auto socket : findChildren<QLocalSocket *>())
            socket->abort();
    }
    note(on ? "AI agents can now use OmaWare." : "AI agent access turned off.");
    emit changed();
}

void AgentBridge::stop() {
    setEnabled(false);
}

void AgentBridge::listen() {
    error_.clear();
    if (socketPath().isEmpty()) {
        error_ = "Agents can't connect: this session has no private runtime folder (XDG_RUNTIME_DIR).";
        emit changed();
        return;
    }
    QLocalServer::removeServer(socketPath());
    if (!server_.listen(socketPath()))
        error_ = "Couldn't open the connection for agents: " + server_.errorString();
    else
        QFile::setPermissions(socketPath(), QFile::ReadOwner | QFile::WriteOwner);
    emit changed();
}

void AgentBridge::accept() {
    while (auto socket = server_.nextPendingConnection()) {
        socket->setParent(this);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer] {
            buffer->append(socket->readAll());
            if (buffer->size() > 4 * 1024 * 1024) {
                socket->abort();
                return;
            }
            const auto end = buffer->indexOf('\n');
            if (end < 0) return;
            const auto request = QJsonDocument::fromJson(buffer->left(end)).toVariant().toMap();
            buffer->clear();
            QPointer<QLocalSocket> target(socket);
            handle(request["tool"].toString(), request["args"].toMap(), [target](const QVariantMap &reply) {
                if (!target) return;
                target->write(QJsonDocument::fromVariant(reply).toJson(QJsonDocument::Compact) + "\n");
                target->disconnectFromServer();
            });
        });
    }
}

void AgentBridge::note(const QString &message, bool ok) {
    backend_->note("Agent: " + message, ok);
}

void AgentBridge::markScreen(const QString &uuid, const QString &name) {
    screen_ = {{"uuid", uuid}, {"name", name}, {"at", QDateTime::currentMSecsSinceEpoch()}};
    emit screenChanged();
}

// ---- Calls into OmaWare -----------------------------------------------------------------------
void AgentBridge::call(const QString &op, QVariantMap input, Done done, int attempt) {
    if (!enabled_ && !localLab_) return done(false, {{"message", "AI agent access was turned off."}});
    input["agentRequest"] = true;
    const auto tag = QUuid::createUuid().toString(QUuid::WithoutBraces);
    input["requestTag"] = tag;
    pending_[tag] = done;
    if (backend_->request(op, input)) return;
    pending_.remove(tag);
    // OmaWare runs one change at a time; wait for the current one (for up to two minutes).
    if (attempt >= 300) return done(false, {{"message", "OmaWare is busy with another task. Try again in a minute."}});
    QTimer::singleShot(400, this, [this, op, input, done, attempt] { call(op, input, done, attempt + 1); });
}

void AgentBridge::callAgent(const QString &op, QVariantMap input, Done done) {
    if (!enabled_) return done(false, {{"message", "AI agent access was turned off."}});
    input["agentRequest"] = true;
    const auto tag = QUuid::createUuid().toString(QUuid::WithoutBraces);
    input["requestTag"] = tag;
    pending_[tag] = done;
    QMetaObject::invokeMethod(agentWorker_, [worker = agentWorker_, op, input] { worker->manage(op, input); });
}

QVariantMap AgentBridge::findVm(const QString &name, QString &error) const {
    const auto wanted = name.trimmed();
    if (wanted.isEmpty()) {
        error = "Say which VM (its name or UUID).";
        return {};
    }
    for (const auto &row : backend_->domains()) {
        const auto vm = row.toMap();
        const auto full = vm["name"].toString();
        if (vm["uuid"].toString() != wanted && full != wanted &&
                shortName(full).compare(wanted, Qt::CaseInsensitive) != 0)
            continue;
        if (!vm["owned"].toBool()) {
            error = "“" + wanted + "” wasn't made by OmaWare, so agents can't use it.";
            return {};
        }
        // Contained VMs may run untrusted software, whose screens and output could try to steer an agent.
        if (vm["contained"].toBool()) {
            error = "“" + wanted + "” is contained (it may run untrusted software), so agents can't use it.";
            return {};
        }
        auto out = vm;
        out["short"] = shortName(full);
        return out;
    }
    error = "There's no OmaWare VM called “" + wanted + "”. Use omaware_overview to see the VMs.";
    return {};
}

void AgentBridge::ask(const QString &title, const QString &text, const QString &action, std::function<void(bool)> then,
        const QString &grantKind) {
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // One question at a time; a newer one replaces (and declines) an unanswered one.
    if (!confirmation_.isEmpty()) answer(confirmation_["id"].toString(), false);
    questions_[id] = then;
    confirmation_ = {{"id", id}, {"title", title}, {"text", text}, {"action", action}};
    if (!grantKind.isEmpty()) {
        questionGrants_[id] = grantKind;
        confirmation_["grant"] = grantKind;
        confirmation_["grantLabel"] = "Don't ask again for " + AgentGrants::label(grantKind) +
                                      " until agent access is turned off (at most 8 hours)";
    }
    emit confirmationChanged();
    QTimer::singleShot(300000, this, [this, id] { answer(id, false); });
}

void AgentBridge::answer(const QString &id, bool yes, bool grant) {
    if (!questions_.contains(id)) return;
    const auto then = questions_.take(id);
    const auto kind = questionGrants_.take(id);
    if (confirmation_.value("id") == id) {
        confirmation_.clear();
        emit confirmationChanged();
    }
    // Only the kind this question offered, only while access is on, and never saved.
    if (yes && grant && !kind.isEmpty() && enabled_) {
        grants_[kind] = QDateTime::currentMSecsSinceEpoch() + AgentGrants::lifetimeMs;
        note("you allowed " + AgentGrants::label(kind) + " without asking, for this session");
        emit grantsChanged();
    }
    then(yes);
}

bool AgentBridge::granted(const QString &kind) const {
    return enabled_ && !kind.isEmpty() && grants_.value(kind) > QDateTime::currentMSecsSinceEpoch();
}

QVariantList AgentBridge::grants() const {
    QVariantList out;
    for (auto it = grants_.cbegin(); it != grants_.cend(); ++it)
        if (granted(it.key()))
            out.append(
                    QVariantMap{{"kind", it.key()}, {"label", AgentGrants::label(it.key())}, {"expires", it.value()}});
    return out;
}

void AgentBridge::revokeGrants() {
    if (grants_.isEmpty()) return;
    grants_.clear();
    note("session approvals revoked; every change asks again");
    emit grantsChanged();
}

QString AgentGrants::label(const QString &kind) {
    return kind == privateAdapters ? QString("adapter changes on isolated and host-only networks") : kind;
}

QString AgentGrants::forAdapterChange(const QString &action, const QVariantMap &target) {
    if (action == "remove") return privateAdapters;
    // Never the internet (NAT, the private internet connection), a local network or anything else.
    if ((action == "add" || action == "update") && target["managed"].toBool() && target["kind"] == "bridge" &&
            QStringList{"Isolated", "Host-only"}.contains(target["category"].toString()))
        return privateAdapters;
    return {};
}

// ---- Tools ------------------------------------------------------------------------------------
void AgentBridge::handle(const QString &tool, const QVariantMap &args, Reply reply) {
    if (!enabled_ && tool != "ping") return reply(failure("AI agent access is turned off in OmaWare's Settings."));
    QString error;
    if (!Mcp::validateManagementArguments(tool, args, error)) return reply(failure(error, "invalid_argument"));

    // Tools that aren't about one VM.
    if (tool == "ping") return reply(success({{"version", QCoreApplication::applicationVersion()}}));
    if (tool == "omaware_overview") return overview(reply);
    if (tool == "propose_lab") return proposeLab(args.contains("plan") ? args["plan"].toMap() : args, reply);
    if (tool == "lab_status") return labStatus(args, reply);
    if (tool == "delete_lab") return deleteLab(LabPlan::slug(args.value("lab").toString()), reply);
    if (AgentProvision::handles(tool)) return provisioningTool(tool, args, reply);
    if (tool == "manage_network") return manageNetwork(args, reply);
    if (tool == "get_media") return getMedia(args, reply);

    // Tools about one VM, which must be one agents may use.
    const auto vm = findVm(args.value("vm").toString(), error);
    if (vm.isEmpty()) return reply(failure(error));
    static const QStringList managementTools{
            "vm_details", "diagnose_vm", "update_vm_resources", "clone_vm", "manage_iso", "manage_network_adapter"};
    if (managementTools.contains(tool)) return managementTool(tool, vm, args, reply);
    if (tool == "screenshot") return screenshot(vm, args.value("max_width", 1280).toInt(), reply);
    using VmTool = void (AgentBridge::*)(const QVariantMap &vm, const QVariantMap &args, Reply reply);
    static const QHash<QString, VmTool> vmTools{
            {"wait_for_vm", &AgentBridge::waitForVm},
            {"transfer_file", &AgentBridge::transferFile},
            {"vm_power", &AgentBridge::vmPower},
            {"vm_input", &AgentBridge::input},
            {"type_login", &AgentBridge::typeLogin},
            {"serial_console", &AgentBridge::serialConsole},
            {"run_command", &AgentBridge::runCommand},
            {"set_cable", &AgentBridge::setCable},
            {"list_snapshots", &AgentBridge::listSnapshots},
            {"snapshot_vm", &AgentBridge::snapshotVm},
            {"restore_snapshot", &AgentBridge::restoreSnapshot},
    };
    if (const auto handler = vmTools.value(tool)) return (this->*handler)(vm, args, reply);
    reply(failure("Unknown tool “" + tool + "”."));
}

void AgentBridge::vmPower(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    static const QMap<QString, QString> actions{{"start", "start"}, {"shutdown", "shutdown"},
            {"force_off", "force-off"}, {"pause", "pause"}, {"resume", "resume"}, {"restart", "restart"}};
    const auto action = actions.value(args.value("action").toString());
    if (action.isEmpty()) return reply(failure("action must be start, shutdown, force_off, pause, resume or restart."));
    note(vm["short"].toString() + ": " + args.value("action").toString());
    if (action != "restart") return call("vm.power", {{"uuid", vm["uuid"]}, {"action", action}}, relayMessage(reply));
    call("vm.restart", {{"uuid", vm["uuid"]}}, [reply](bool ok, const QVariantMap &r) {
        if (!ok) return reply(failure(r["message"].toString()));
        reply(success({{"message", r.value("waiting").toBool()
                                           ? "Shutdown requested; the VM starts again once the guest has shut down."
                                           : r["message"]}}));
    });
}

void AgentBridge::listSnapshots(const QVariantMap &vm, const QVariantMap &, Reply reply) {
    call("snapshots.list", {{"uuid", vm["uuid"]}}, [reply](bool ok, const QVariantMap &r) {
        if (!ok) return reply(failure(r["message"].toString()));
        QVariantList items;
        for (const auto &v : r["items"].toList()) {
            const auto s = v.toMap();
            items.append(QVariantMap{{"id", s["id"]}, {"name", s["name"]}, {"time", s["time"]}, {"notes", s["notes"]},
                    {"memory", !s["memory"].toString().isEmpty()}, {"current", s["id"] == r["currentId"]}});
        }
        reply(success({{"snapshots", items}, {"can_snapshot", r["blocker"].toString().isEmpty()},
                {"why_not", r["blocker"]}}));
    });
}

void AgentBridge::snapshotVm(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto name = args.value("name").toString().trimmed();
    const auto notes = args.value("notes").toString();
    // Running or paused: the snapshot can include memory, and does unless the agent says otherwise.
    const bool running = vm["stateCode"].toInt() == 1 || vm["stateCode"].toInt() == 3;
    note(vm["short"].toString() + ": snapshot “" + name + "”");
    call("snapshots.create",
            {{"uuid", vm["uuid"]}, {"name", name},
                    {"notes", "Taken by an AI agent." + (notes.isEmpty() ? QString() : " " + notes)},
                    {"memory", args.value("memory", running).toBool() && running}},
            relayMessage(reply));
}

// Asks the user first: restoring loses everything since the snapshot.
void AgentBridge::restoreSnapshot(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto wanted = args.value("snapshot").toString();
    const auto uuid = vm["uuid"].toString(), name = vm["short"].toString();
    call("snapshots.list", {{"uuid", uuid}}, [this, reply, wanted, uuid, name](bool ok, const QVariantMap &r) {
        if (!ok) return reply(failure(r["message"].toString()));
        QVariantMap found;
        for (const auto &v : r["items"].toList())
            if (v.toMap()["id"] == wanted || v.toMap()["name"] == wanted) found = v.toMap();
        if (found.isEmpty() || found["kind"] == "internal")
            return reply(failure("There's no snapshot “" + wanted + "” on " + name + ". Use list_snapshots."));
        ask("Restore " + name + "?",
                "An AI agent wants to put " + name + " back to its snapshot “" + found["name"].toString() +
                        "”. Changes since then are lost, unless you take a snapshot first.",
                "Restore", [this, reply, uuid, name, found](bool yes) {
                    if (!yes) {
                        note(name + ": restore declined", false);
                        return reply(failure("The user said no to restoring this snapshot."));
                    }
                    note(name + ": restore “" + found["name"].toString() + "”");
                    call("snapshots.restore", {{"uuid", uuid}, {"id", found["id"]}, {"allowRestart", true}},
                            relayMessage(reply));
                });
    });
}

void AgentBridge::overview(Reply reply) {
    call("networks.list", {}, [this, reply](bool ok, const QVariantMap &r) {
        if (!ok) return reply(failure(r["message"].toString()));
        QHash<QString, QString> networkOf; // bridge or libvirt network name -> display name
        QVariantList networks;
        for (const auto &v : r["items"].toList()) {
            const auto n = v.toMap();
            networkOf[n["bridge"].toString()] = networkOf[n["name"].toString()] = shortName(n["name"].toString());
            if (!n["managed"].toBool()) continue;
            networks.append(QVariantMap{{"name", shortName(n["name"].toString())}, {"uuid", n["uuid"]},
                    {"type", n["mode"] == "nat"        ? "internet"
                             : n["mode"] == "hostonly" ? "private"
                                                       : "isolated"},
                    {"subnet", n["cidr"]}, {"active", n["active"]}, {"autostart", n["autostart"]},
                    {"vms", n["users"]}});
        }
        QHash<QString, QVariantMap> topology;
        for (const auto &v : r["topology"].toList())
            topology[v.toMap()["uuid"].toString()] = v.toMap();
        auto vms = std::make_shared<QVariantList>();
        QStringList askAgent; // running VMs with an adapter whose address the host doesn't know
        for (const auto &row : backend_->domains()) {
            const auto vm = row.toMap();
            if (!vm["owned"].toBool() || vm["contained"].toBool()) continue;
            const auto t = topology.value(vm["uuid"].toString());
            QVariantList adapters;
            for (const auto &i : (t["active"].toBool() ? t["liveInterfaces"] : t["interfaces"]).toList()) {
                const auto nic = i.toMap();
                const auto mac = nic["mac"].toString().toLower();
                const auto ips = t["addresses"].toMap().value(mac);
                adapters.append(QVariantMap{
                        {"network", nic["type"] == "user"
                                            ? "internet (private to this VM)"
                                            : networkOf.value(nic["source"].toString(), nic["source"].toString())},
                        {"mac", nic["mac"]}, {"cable", nic["linkUp"].toBool() ? "plugged" : "unplugged"}, {"ips", ips},
                        {"ip_source", ips.toList().isEmpty() ? QString("unknown")
                                                             : t["addressSources"].toMap().value(mac).toString()}});
                // Private internet connections have an address only the guest knows; ask its agent too.
                if (t["active"].toBool() && ips.toList().isEmpty() && !askAgent.contains(vm["uuid"].toString()))
                    askAgent << vm["uuid"].toString();
            }
            const auto lab = vmLab(vm["uuid"].toString());
            const int pending = t["pendingChanges"].toInt();
            vms->append(QVariantMap{{"name", shortName(vm["name"].toString())}, {"uuid", vm["uuid"]},
                    {"state", vm["state"]}, {"cpus", vm["cpus"]}, {"memory_mib", vm["memoryMiB"]},
                    {"lab", lab.value("lab")}, {"user", lab.value("user")}, {"adapters", adapters},
                    {"pending_change_count", pending}, {"restart_needed", pending > 0}});
        }
        QVariantList images;
        for (const auto &id : CloudImages::ids()) {
            const auto local = CloudImages::local(id);
            images.append(
                    QVariantMap{{"os", id}, {"downloaded", !local.isEmpty()}, {"version", local.value("version")}});
        }
        const QVariantMap rest{{"networks", networks}, {"labs", labs()}, {"images", images}};
        // Addresses the host can't see (a guest router's DHCP on an isolated network, for example) come
        // from the QEMU guest agent when it runs, one VM at a time on the agent's own worker.
        auto next = std::make_shared<std::function<void(int)>>();
        *next = [this, vms, askAgent, rest, reply, next](int index) {
            if (index >= askAgent.size()) {
                auto result = rest;
                result["vms"] = *vms;
                reply(success(result));
                QTimer::singleShot(0, this, [next] { *next = nullptr; });
                return;
            }
            callAgent("vm.addresses", {{"uuid", askAgent[index]}},
                    [vms, askAgent, index, next](bool ok, const QVariantMap &r) {
                        if (ok && r["agent"].toBool()) {
                            for (auto &row : *vms) {
                                auto vm = row.toMap();
                                if (vm["uuid"] != askAgent[index]) continue;
                                auto adapters = vm["adapters"].toList();
                                for (auto &a : adapters) {
                                    auto adapter = a.toMap();
                                    if (!adapter["ips"].toList().isEmpty()) continue;
                                    for (const auto &n : r["interfaces"].toList())
                                        if (n.toMap()["mac"].toString().compare(
                                                    adapter["mac"].toString(), Qt::CaseInsensitive) == 0 &&
                                                n.toMap()["ipSource"] == "guest_agent") {
                                            adapter["ips"] = n.toMap()["ips"];
                                            adapter["ip_source"] = "guest_agent";
                                        }
                                    a = adapter;
                                }
                                vm["adapters"] = adapters;
                                row = vm;
                            }
                        }
                        (*next)(index + 1);
                    });
        };
        (*next)(0);
    });
}

void AgentBridge::proposeLab(const QVariantMap &plan, Reply reply, bool local) {
    if (!current_.id.isEmpty())
        return reply(failure(
                "A lab is being built right now. Wait for it to finish (lab_status), then propose the next one."));
    if (!local && localLab_ && !proposal_.isEmpty())
        return reply(failure("The user is reviewing a lab of their own. Try again once they've finished."));
    if (local) localLab_ = true;
    auto go = [this, plan, reply, local] {
        call("networks.list", {}, [this, plan, reply, local](bool ok, const QVariantMap &r) {
            if (!ok) return reply(failure(r["message"].toString()));
            LabPlan::Host host;
            host.cpus = host_.value("cpus", 1).toInt();
            host.memoryMiB = host_.value("memoryMiB").toLongLong();
            host.images = CloudImages::ids();
            for (const auto &row : backend_->domains())
                host.existingVms << shortName(row.toMap()["name"].toString()).toLower();
            for (const auto &n : r["items"].toList())
                host.existingNetworks << n.toMap()["name"].toString();
            auto check = LabPlan::check(plan, host);
            auto problems = check["problems"].toStringList();
            const auto normalized = check["plan"].toMap();
            if (!Labs::load(normalized["slug"].toString()).isEmpty())
                problems << "A lab called “" + normalized["name"].toString() +
                                    "” already exists. Choose another name, or delete_lab it first.";
            if (!problems.isEmpty())
                return reply(failure("The plan needs changes: " + problems.join(" "),
                        {{"problems", problems}, {"warnings", check["warnings"]}}));
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!proposal_.isEmpty()) {
                // A newer plan replaces one the user hasn't answered.
                states_[proposal_["id"].toString()] = {
                        {"state", "replaced"}, {"message", "A newer plan replaced this one."}};
            }
            QVariantList images;
            QStringList seen;
            for (const auto &v : normalized["vms"].toList()) {
                const auto os = v.toMap()["os"].toString();
                if (seen.contains(os)) continue;
                seen << os;
                const auto local = CloudImages::local(os);
                images.append(QVariantMap{{"os", os}, {"ready", !local.isEmpty()}, {"name", local.value("name", os)}});
            }
            proposal_ = {{"id", id}, {"plan", normalized}, {"warnings", check["warnings"]}, {"images", images},
                    {"login", normalized["name"]}, {"user", normalized["user"]}, {"local", local}};
            localLab_ = local;
            states_[id] = {{"state", "waiting"}, {"name", normalized["name"]},
                    {"message", "Waiting for the user to review the plan in OmaWare."}};
            emit proposalChanged();
            note("proposed the lab “" + normalized["name"].toString() + "”");
            reply(success({{"lab_id", id}, {"state", "waiting"}, {"warnings", check["warnings"]},
                    {"message",
                            "OmaWare is showing the plan to the user. They set the VMs' password there (you never see "
                            "it) and click Build. Call lab_status with this lab_id and wait_seconds to follow it."}}));
        });
    };
    if (!host_.isEmpty()) {
        go();
        return;
    }
    call("capabilities", {}, [this, go, reply](bool ok, const QVariantMap &r) {
        if (!ok) return reply(failure(r["message"].toString()));
        host_ = {{"cpus", r["cpus"]}, {"memoryMiB", r["memoryMiB"]}};
        go();
    });
}

void AgentBridge::labStatus(const QVariantMap &args, Reply reply) {
    auto id = args.value("lab_id").toString();
    const auto byName = args.value("lab").toString();
    if (id.isEmpty() && !byName.isEmpty())
        for (auto it = states_.cbegin(); it != states_.cend(); ++it)
            if (it.value()["name"].toString().compare(byName, Qt::CaseInsensitive) == 0) id = it.key();
    auto describe = [this](const QString &id) {
        auto state = states_.value(id);
        if (state["state"] == "ready") {
            const auto lab = Labs::load(state["slug"].toString());
            state["vms"] = lab["vms"];
            state["networks"] = lab["networks"];
            state["user"] = lab["user"];
            state["login"] = lab["login"];
            state["next"] = "The VMs are starting. First-boot setup takes one to three minutes; run_command with "
                            "“cloud-init status --wait” waits for it. "
                            "Use omaware_overview for their addresses, type_login to log in on a screen.";
        }
        state["lab_id"] = id;
        return state;
    };
    if (!states_.contains(id)) {
        const auto lab = Labs::load(LabPlan::slug(byName.isEmpty() ? id : byName));
        if (!lab.isEmpty())
            return reply(success({{"state", lab["state"]}, {"name", lab["name"]}, {"vms", lab["vms"]},
                    {"networks", lab["networks"]}, {"user", lab["user"]}}));
        reply(failure("There's no lab “" + (byName.isEmpty() ? id : byName) + "”."));
        return;
    }
    const int wait = std::clamp(args.value("wait_seconds", 0).toInt(), 0, 120);
    const auto state = states_.value(id)["state"].toString();
    if (wait == 0 || (state != "waiting" && state != "building")) return reply(success(describe(id)));
    // Answer when the state changes, or when the wait is over.
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto respond = [this, id, reply, token, describe] {
        for (int i = statusWaiters_.size() - 1; i >= 0; --i)
            if (statusWaiters_[i].token == token) statusWaiters_.removeAt(i);
        reply(success(describe(id)));
    };
    statusWaiters_.append({id, token, respond});
    QTimer::singleShot(wait * 1000, this, [this, token] {
        for (const auto &waiter : statusWaiters_)
            if (waiter.token == token) {
                const auto answer = waiter.answer;
                answer();
                return;
            }
    });
}

void AgentBridge::wake(const QString &id) {
    QList<std::function<void()>> due;
    for (const auto &waiter : statusWaiters_)
        if (waiter.lab == id) due << waiter.answer;
    for (const auto &answer : due)
        answer();
}

void AgentBridge::screenshot(const QVariantMap &vm, int maxWidth, Reply reply, const QString &summary) {
    const auto uuid = vm["uuid"].toString(), name = vm["short"].toString();
    callAgent("vm.screenshot", {{"uuid", uuid}, {"maxWidth", maxWidth}},
            [this, reply, uuid, name, summary](bool ok, const QVariantMap &r) {
                if (!ok)
                    return reply(failure((summary.isEmpty() ? QString{} : summary + " ") + r["message"].toString()));
                screenSizes_[uuid] = QSize(r["width"].toInt(), r["height"].toInt());
                markScreen(uuid, name);
                QVariantMap result{{"vm", name}, {"width", r["width"]}, {"height", r["height"]},
                        {"note", "Coordinates for vm_input are in this image's pixels (0,0 is the top left)."}};
                if (!summary.isEmpty()) result["actions"] = summary;
                auto out = success(result);
                out["image"] = r["png"];
                reply(out);
            });
}

void AgentBridge::input(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto uuid = vm["uuid"].toString(), name = vm["short"].toString();
    const auto actions = args.value("actions").toList();
    const bool after = args.value("screenshot", true).toBool();
    const int maxWidth = args.value("max_width", 1280).toInt();
    auto send = [this, vm, uuid, name, actions, after, maxWidth, reply] {
        const auto size = screenSizes_.value(uuid);
        // Typed text is never logged, since it may be a password.
        note(name + ": " + QString::number(actions.size()) + " screen action" + (actions.size() == 1 ? "" : "s"));
        callAgent("vm.input",
                {{"uuid", uuid}, {"actions", actions}, {"width", size.width()}, {"height", size.height()}},
                [this, vm, uuid, name, after, maxWidth, reply](bool ok, const QVariantMap &r) {
                    markScreen(uuid, name);
                    if (!ok || !after)
                        return reply(ok ? success({{"message", r["message"]}})
                                        : failure(r["message"].toString(), {{"completed", r["completed"]}}));
                    // Give the guest a moment to draw the result.
                    QTimer::singleShot(700, this, [this, vm, maxWidth, reply, r] {
                        screenshot(vm, maxWidth, reply, r["message"].toString());
                    });
                });
    };
    if (screenSizes_.contains(uuid)) {
        send();
        return;
    }
    // Clicks need the screen's size; take a screenshot first.
    callAgent("vm.screenshot", {{"uuid", uuid}, {"maxWidth", maxWidth}},
            [this, uuid, send, reply](bool ok, const QVariantMap &r) {
                if (!ok) return reply(failure(r["message"].toString()));
                screenSizes_[uuid] = QSize(r["width"].toInt(), r["height"].toInt());
                send();
            });
}

void AgentBridge::typeLogin(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    // Only the login the VM's own lab was built with: never one the agent names.
    const auto login = vmLab(vm["uuid"].toString()).value("login").toString();
    if (login.isEmpty() || !Logins::exists(login))
        return reply(failure(
                "OmaWare has no saved login for " + vm["short"].toString() + ". Ask the user to log in themselves."));
    const auto field = args.value("field", "password").toString();
    if (field != "user" && field != "password") return reply(failure("field must be user or password."));
    QString text = Logins::user(login), error;
    if (field == "password" && !Logins::password(login, text, error)) return reply(failure(error));
    if (args.value("via", "screen") == "serial") {
        note(vm["short"].toString() + ": typed the " + (field == "user" ? "user name" : "password") + " of login “" +
                login + "” on the serial console");
        const auto secret = text;
        callAgent("vm.serial",
                {{"uuid", vm["uuid"]}, {"send", text + (args.value("enter", true).toBool() ? "\n" : "")},
                        {"timeout", 5}},
                [reply, field, login, secret](bool ok, const QVariantMap &r) {
                    if (!ok)
                        return reply(
                                failure(r["message"].toString(), {{"code", r.value("code", "console_unavailable")}}));
                    // A guest that echoes a password would print it; the agent never gets it.
                    auto output = r["output"].toString();
                    if (!secret.isEmpty()) output.replace(secret, "********");
                    reply(success(
                            {{"message", "Typed the " + field + " of login “" + login + "” on the serial console."},
                                    {"output", output}}));
                });
        return;
    }
    QVariantList actions{QVariantMap{{"type", "type"}, {"text", text}}};
    if (args.value("enter", true).toBool()) actions.append(QVariantMap{{"type", "key"}, {"keys", "enter"}});
    note(vm["short"].toString() + ": typed the " + (field == "user" ? "user name" : "password") + " of login “" +
            login + "”");
    input(vm, {{"actions", actions}, {"screenshot", args.value("screenshot", true)}},
            [reply, field, login](const QVariantMap &r) {
                auto out = r;
                if (out["ok"].toBool()) {
                    auto result = out["result"].toMap();
                    result["message"] = "Typed the " + field + " of login “" + login + "”.";
                    out["result"] = result;
                }
                reply(out);
            });
}

void AgentBridge::serialConsole(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto send = args.value("send").toString(), waitFor = args.value("wait_for").toString();
    auto bounded = [&](const char *key, int low, int high) {
        return !args.contains(key) || (args[key].toInt() >= low && args[key].toInt() <= high);
    };
    if (send.size() > 4000 || waitFor.size() > 200 || !bounded("timeout_seconds", 1, 60) ||
            !bounded("max_bytes", 1024, 65536))
        return reply(failure(
                "Send at most 4000 characters, wait_for at most 200, timeout_seconds 1–60 and max_bytes 1024–65536.",
                {{"code", "invalid_argument"}}));
    // What's typed is never logged, since it may be a password.
    note(vm["short"].toString() + ": serial console" +
            (send.isEmpty() ? QString(" (read)") : QString(" (%1 characters typed)").arg(send.size())));
    markScreen(vm["uuid"].toString(), vm["short"].toString());
    callAgent("vm.serial",
            {{"uuid", vm["uuid"]}, {"send", send}, {"waitFor", waitFor}, {"timeout", args.value("timeout_seconds", 10)},
                    {"maxBytes", args.value("max_bytes", 16384)}},
            [reply](bool ok, const QVariantMap &r) {
                if (!ok)
                    return reply(failure(r["message"].toString(), {{"code", r.value("code", "operation_failed")}}));
                reply(success({{"output", r["output"]}, {"matched", r["matched"]}, {"timed_out", r["timedOut"]},
                        {"truncated", r["truncated"]}, {"console_closed", r["closed"]},
                        {"elapsed_ms", r["elapsedMs"]}}));
            });
}

void AgentBridge::runCommand(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto command = args.value("command").toString();
    const int timeout = std::clamp(args.value("timeout_seconds", 120).toInt(), 1, 900);
    if (command.trimmed().isEmpty() || command.size() > 16000)
        return reply(failure("Give a command of up to 16000 characters."));
    note(vm["short"].toString() + ": run “" + command.left(120) + (command.size() > 120 ? "…" : "") + "”");
    callAgent("vm.agentExec", {{"uuid", vm["uuid"]}, {"command", command}, {"timeout", timeout}},
            [this, vm, command, timeout, reply](bool ok, const QVariantMap &r) {
                if (ok)
                    return reply(success({{"exit_code", r["exitCode"]}, {"stdout", r["stdout"]},
                            {"stderr", r["stderr"]}, {"via", "QEMU guest agent (as root)"}}));
                if (!r.value("noAgent").toBool()) return reply(failure(r["message"].toString()));
                const auto lab = Labs::load(vmLab(vm["uuid"].toString()).value("slug").toString());
                if (lab.isEmpty())
                    return reply(failure(
                            "The QEMU guest agent isn't running in " + vm["short"].toString() +
                                    ", and it wasn't built as part of a lab (no SSH login), so OmaWare can't run "
                                    "commands in it. "
                                    "The guest agent doesn't need a network: once qemu-guest-agent is installed "
                                    "and running in the guest (OmaWare's VMs already have its channel), "
                                    "run_command works even on isolated networks. "
                                    "Until then, use the VM's screen (screenshot and vm_input).",
                            {{"code", "no_transport"}}));
                runOverSsh(vm, lab, command, timeout, reply);
            });
}

void AgentBridge::runOverSsh(
        const QVariantMap &vm, const QVariantMap &lab, const QString &command, int timeout, Reply reply) {
    callAgent("vm.addresses", {{"uuid", vm["uuid"]}},
            [this, vm, lab, command, timeout, reply](bool ok, const QVariantMap &r) {
                if (!ok) return reply(failure(r["message"].toString()));
                if (!r["active"].toBool())
                    return reply(failure(vm["short"].toString() + " isn't running. Start it first."));
                QString address;
                for (const auto &v : r["interfaces"].toList()) {
                    const auto nic = v.toMap();
                    if (nic["type"] == "user" || !nic["linkUp"].toBool()) continue;
                    for (const auto &ip : nic["ips"].toList())
                        if (address.isEmpty()) address = ip.toString();
                }
                if (address.isEmpty())
                    return reply(failure(
                            vm["short"].toString() +
                            " has no address OmaWare can reach yet. It may still be starting (wait a minute), or "
                            "it's only on isolated networks; then use its screen."));
                QString eligibilityError;
                if (!enabled_ || findVm(vm["uuid"].toString(), eligibilityError).isEmpty())
                    return reply(failure("Agent access or VM eligibility changed."));
                const auto slug = lab["slug"].toString(), user = lab["user"].toString();
                if (!CloudSeed::validUser(user) || !QRegularExpression("^[0-9.]+$").match(address).hasMatch())
                    return reply(failure("The lab's login details are damaged."));
                auto process = new QProcess(this);
                process->start("ssh", {"-i", Labs::keyPath(slug), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                                              "-o", "ConnectTimeout=10", "-o", "StrictHostKeyChecking=yes", "-o",
                                              "UserKnownHostsFile=\"" + Labs::folderOf(slug) + "/known_hosts\"", "-o",
                                              "HostKeyAlias=" + vm["uuid"].toString(), "-o", "LogLevel=ERROR", "-o",
                                              "RequestTTY=no", user + "@" + address, command});
                auto timer = new QTimer(process);
                timer->setSingleShot(true);
                connect(timer, &QTimer::timeout, process, [process] {
                    process->setProperty("timedOut", true);
                    process->kill();
                });
                timer->start(timeout * 1000);
                connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError e) {
                    if (e == QProcess::FailedToStart) emit process->finished(-1, QProcess::CrashExit);
                });
                connect(process, &QProcess::finished, this,
                        [process, reply, user, address, timeout](int code, QProcess::ExitStatus status) {
                            process->deleteLater();
                            const auto out = QString::fromUtf8(process->readAllStandardOutput().right(65536)),
                                       err = QString::fromUtf8(process->readAllStandardError().right(65536));
                            if (process->property("timedOut").toBool())
                                return reply(
                                        failure(QString("The command didn't finish within %1 seconds and was stopped.")
                                                        .arg(timeout),
                                                {{"stdout", out}, {"stderr", err}}));
                            if (code == -1 && status == QProcess::CrashExit && out.isEmpty())
                                return reply(failure("Couldn't start ssh. Is OpenSSH installed?"));
                            if (code == 255)
                                return reply(failure("Couldn't connect over SSH to " + address + ": " + err.trimmed()));
                            reply(success({{"exit_code", code}, {"stdout", out}, {"stderr", err},
                                    {"via", "SSH as " + user + " (sudo works without a password)"}}));
                        });
            });
}

// Plugs or pulls one of a VM's existing cables. Pulling is a safety action and never asks; plugging in a cable
// that leads beyond OmaWare's isolated and host-only networks asks first, as connecting a new adapter there does.
void AgentBridge::setCable(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto wanted = args.value("network").toString().toLower();
    const bool plugged = args.value("plugged", true).toBool();
    call("networks.list", {}, [this, vm, wanted, plugged, reply](bool ok, const QVariantMap &r) {
        if (!ok) return reply(failure(r["message"].toString()));
        QVariantMap network;
        for (const auto &v : r["items"].toList()) {
            const auto n = v.toMap();
            if (shortName(n["name"].toString()) == wanted || n["name"].toString() == wanted) network = n;
        }
        const auto bridge = network["bridge"].toString(), name = network["name"].toString();
        QString mac;
        for (const auto &v : r["topology"].toList())
            if (v.toMap()["uuid"] == vm["uuid"])
                for (const auto &i : v.toMap()["interfaces"].toList()) {
                    const auto nic = i.toMap();
                    if ((wanted == "internet" && nic["type"] == "user") ||
                            (!bridge.isEmpty() && nic["source"] == bridge) ||
                            (!name.isEmpty() && nic["source"] == name))
                        mac = nic["mac"].toString();
                }
        if (mac.isEmpty()) return reply(failure(vm["short"].toString() + " has no adapter on “" + wanted + "”."));
        const auto apply = [this, vm, wanted, plugged, mac, reply] {
            note(vm["short"].toString() + ": " + (plugged ? "plug in" : "pull") + " the cable to " + wanted);
            linkWaiters_.append([this, reply, plugged, vm] {
                // The new revision, so the next change doesn't need a fresh list.
                callAgent("vm.details", {{"uuid", vm["uuid"]}}, [reply, plugged](bool ok, const QVariantMap &r) {
                    QVariantMap result{{"message", plugged ? "Cable plugged in." : "Cable pulled."}};
                    if (ok) result["revision"] = r["revision"];
                    reply(success(result));
                });
            });
            backend_->setLinks({QVariantMap{{"uuid", vm["uuid"]}, {"mac", mac}}}, plugged);
        };
        const bool privateNetwork = wanted != "internet" && network["managed"].toBool() &&
                                    QStringList{"isolated", "hostonly"}.contains(network["mode"].toString());
        if (!plugged || privateNetwork) return apply();
        ask("Plug in " + vm["short"].toString() + "'s cable?",
                "An AI agent wants to plug in " + vm["short"].toString() + "'s cable to “" + wanted +
                        "”, which can reach the internet or your local network.",
                "Plug in", [this, vm, apply, reply](bool yes) {
                    if (yes) return apply();
                    note(vm["short"].toString() + ": plugging in a cable declined", false);
                    reply(failure("The user said no to plugging in this cable.", "declined"));
                });
    });
}

// ---- Deleting a lab ---------------------------------------------------------------------------
void AgentBridge::deleteLab(const QString &slug, Reply reply) {
    const auto lab = Labs::load(slug);
    if (lab.isEmpty()) return reply(failure("There's no lab called “" + slug + "”."));
    if (current_.lab.value("slug") == slug) return reply(failure("This lab is still being built."));
    QStringList vmNames, netNames;
    for (const auto &v : lab["vms"].toList())
        vmNames << v.toMap()["name"].toString();
    for (const auto &n : lab["networks"].toList())
        netNames << n.toMap()["name"].toString();
    const auto text = QString("An AI agent wants to delete the lab “%1”: its VMs (%2) with their disks and snapshots, "
                              "and its networks (%3). This can't be undone. The saved login is kept.")
                              .arg(lab["name"].toString(), vmNames.isEmpty() ? "none" : vmNames.join(", "),
                                      netNames.isEmpty() ? "none" : netNames.join(", "));
    ask("Delete " + lab["name"].toString() + "?", text, "Delete lab", [this, lab, slug, reply](bool yes) {
        if (!yes) {
            note("deleting “" + lab["name"].toString() + "” declined", false);
            reply(failure("The user said no to deleting this lab."));
            return;
        }
        note("delete lab “" + lab["name"].toString() + "”");
        auto steps = std::make_shared<QList<std::function<void(std::function<void()>)>>>();
        auto problems = std::make_shared<QStringList>();
        for (const auto &v : lab["vms"].toList()) {
            const auto vm = v.toMap();
            const auto uuid = vm["uuid"].toString();
            steps->append([this, uuid](std::function<void()> next) {
                call("vm.power", {{"uuid", uuid}, {"action", "force-off"}},
                        [next](bool, const QVariantMap &) { next(); });
            });
            steps->append([this, uuid, vm, problems](std::function<void()> next) {
                call("vm.power", {{"uuid", uuid}, {"action", "remove"}},
                        [vm, uuid, problems, next](bool ok, const QVariantMap &r) {
                            if (!ok && !r["message"].toString().contains("Find VM")) {
                                *problems << vm["name"].toString() + ": " + r["message"].toString();
                                next();
                                return;
                            }
                            // The lab made this folder for the VM; only a folder inside OmaWare's VM folder is deleted.
                            const auto dir = QDir::cleanPath(vm["dir"].toString());
                            if (!dir.isEmpty() && QFileInfo(dir).absolutePath() == QDir::cleanPath(Paths::vms()) &&
                                    QFileInfo(dir).fileName().startsWith("omaware-"))
                                QDir(dir).removeRecursively();
                            if (!QUuid(uuid).isNull())
                                QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                        "/checkpoints/" + uuid)
                                        .removeRecursively();
                            next();
                        });
            });
        }
        for (const auto &n : lab["networks"].toList()) {
            const auto uuid = n.toMap()["uuid"].toString();
            for (const QString op : {"networks.stop", "networks.remove"})
                steps->append([this, uuid, op, problems, n](std::function<void()> next) {
                    // Network changes check the network's current revision.
                    call("networks.list", {}, [this, uuid, op, problems, n, next](bool, const QVariantMap &r) {
                        QString revision;
                        bool exists = false, active = false;
                        for (const auto &v : r["items"].toList())
                            if (v.toMap()["uuid"] == uuid) {
                                exists = true;
                                revision = v.toMap()["revision"].toString();
                                active = v.toMap()["active"].toBool();
                            }
                        if (!exists || (op == "networks.stop" && !active)) return next();
                        call(op, {{"uuid", uuid}, {"revision", revision}},
                                [problems, n, next](bool ok, const QVariantMap &r) {
                                    if (!ok) *problems << n.toMap()["name"].toString() + ": " + r["message"].toString();
                                    next();
                                });
                    });
                });
        }
        auto run = std::make_shared<std::function<void(int)>>();
        *run = [this, steps, run, problems, slug, lab, reply](int i) {
            if (i < steps->size()) {
                (*steps)[i]([run, i] { (*run)(i + 1); });
                return;
            }
            if (problems->isEmpty()) Labs::remove(slug);
            emit labsChanged();
            if (problems->isEmpty()) {
                note("lab “" + lab["name"].toString() + "” deleted");
                reply(success({{"message", "Lab deleted."}}));
            } else
                reply(failure("Parts of the lab couldn't be deleted: " + problems->join(" ")));
            // Breaks the function's reference to itself, once it has returned.
            QTimer::singleShot(0, this, [run] { *run = nullptr; });
        };
        (*run)(0);
    });
}

// ---- Proposals and building -------------------------------------------------------------------
void AgentBridge::decline(const QString &id) {
    if (proposal_.value("id") != id) return;
    states_[id] = {{"state", "declined"}, {"name", proposal_["plan"].toMap()["name"]},
            {"message", "The user declined the plan."}};
    note("lab “" + proposal_["plan"].toMap()["name"].toString() + "” declined", false);
    proposal_.clear();
    localLab_ = false;
    emit proposalChanged();
    wake(id);
}

void AgentBridge::approve(
        const QString &id, const QString &login, const QString &user, const QString &password, bool saved) {
    if (proposal_.value("id") != id || !current_.id.isEmpty()) return;
    QString error, actualUser = user, actualPassword = password;
    if (saved) {
        actualUser = Logins::user(login);
        if (!Logins::password(login, actualPassword, error)) {
            build_ = {{"id", id}, {"state", "error"}, {"message", error}};
            emit buildChanged();
            return;
        }
    } else {
        QString store;
        if (!CloudSeed::validUser(user))
            error = "Choose a user name of lower-case letters, digits, dashes or underscores.";
        else if (password.size() < 8)
            error = "Use a password of at least 8 characters.";
        else
            Logins::save(login, user, password, store, error); // sets error on failure
        if (!error.isEmpty()) {
            build_ = {{"id", id}, {"state", "error"}, {"message", error}};
            emit buildChanged();
            return;
        }
    }
    current_ = {};
    current_.id = id;
    current_.plan = proposal_["plan"].toMap();
    proposal_.clear();
    emit proposalChanged();
    startBuild(login, actualUser, actualPassword);
}

void AgentBridge::reportBuild() {
    if (current_.id.isEmpty()) return;
    QStringList labels;
    for (const auto &step : current_.steps)
        labels << step.first;
    const int index = build_.value("step").toInt();
    auto progress = images_->progress();
    QString message = labels.value(index);
    for (auto it = progress.cbegin(); it != progress.cend(); ++it)
        message += QString(" · %1 %2%").arg(it.key()).arg(int(it.value().toDouble() * 100));
    build_ = {{"id", current_.id}, {"name", current_.plan["name"]}, {"state", "building"}, {"step", index},
            {"steps", labels}, {"message", message}};
    emit buildChanged();
}

void AgentBridge::startBuild(const QString &login, const QString &user, const QString &password) {
    current_.login = login;
    current_.user = user;
    current_.passwordHash = CloudSeed::hashPassword(password);
    const auto plan = current_.plan;
    const auto slug = plan["slug"].toString();
    current_.lab = {{"id", current_.id}, {"name", plan["name"]}, {"slug", slug}, {"user", user}, {"login", login},
            {"plan", plan}, {"state", "building"}, {"created", QDateTime::currentDateTime().toString(Qt::ISODate)},
            {"networks", QVariantList{}}, {"vms", QVariantList{}}};
    states_[current_.id] = {{"state", "building"}, {"name", plan["name"]}, {"slug", slug}, {"message", "Starting."}};
    note("building the lab “" + plan["name"].toString() + "” (approved by the user)");
    if (current_.passwordHash.isEmpty()) {
        finishBuild(false, "The password couldn't be prepared.");
        return;
    }
    Labs::save(current_.lab);
    auto &steps = current_.steps;
    steps.clear();
    using Next = std::function<void(const QString &)>;
    // 1. Images.
    QStringList oses;
    for (const auto &v : plan["vms"].toList())
        if (!oses.contains(v.toMap()["os"].toString())) oses << v.toMap()["os"].toString();
    for (const auto &os : oses)
        steps.append({"Getting the " + os + " image", [this, os](Next next) {
                          images_->ensure(
                                  os, [next](bool ok, const QString &message) { next(ok ? QString{} : message); });
                      }});
    // 2. Networks, and permission for VMs to join them (one password prompt).
    for (const auto &v : plan["networks"].toList()) {
        const auto net = v.toMap();
        steps.append(
                {"Creating the network " + net["name"].toString(), [this, net](Next next) {
                     QVariantMap input{
                             {"name", net["fullName"]}, {"mode", net["mode"]}, {"autostart", true}, {"start", true}};
                     if (net["mode"] != "isolated" && !net["subnet"].toString().isEmpty()) {
                         const auto subnet = QHostAddress::parseSubnet(net["subnet"].toString());
                         const quint32 base = subnet.first.toIPv4Address(), hosts = (1u << (32 - subnet.second)) - 2;
                         input["subnet"] = net["subnet"];
                         input["dhcpStart"] =
                                 QHostAddress(base + (subnet.second <= 24 ? 100 : std::max(2u, hosts / 2))).toString();
                         input["dhcpEnd"] = QHostAddress(base + (subnet.second <= 24 ? 200 : hosts)).toString();
                     }
                     call("networks.save", input, [this, net, next](bool ok, const QVariantMap &r) {
                         if (!ok) return next(r["message"].toString());
                         auto list = current_.lab["networks"].toList();
                         list.append(QVariantMap{{"name", net["name"]},
                                 {"fullName", "omaware-" + net["fullName"].toString()}, {"type", net["type"]},
                                 {"uuid", r["uuid"]},
                                 {"subnet", r["subnet"].toString().isEmpty() ? net["subnet"] : r["subnet"]}});
                         current_.lab["networks"] = list;
                         Labs::save(current_.lab);
                         next({});
                     });
                 }});
    }
    if (!plan["networks"].toList().isEmpty())
        steps.append({"Letting the VMs join the networks (asks for your password)", [this](Next next) {
                          call("networks.list", {}, [this, next](bool ok, const QVariantMap &r) {
                              if (!ok) return next(r["message"].toString());
                              QVariantList uuids;
                              auto list = current_.lab["networks"].toList();
                              for (auto &v : list) {
                                  auto net = v.toMap();
                                  for (const auto &item : r["items"].toList())
                                      if (item.toMap()["uuid"] == net["uuid"]) net["bridge"] = item.toMap()["bridge"];
                                  uuids << net["uuid"];
                                  v = net;
                              }
                              current_.lab["networks"] = list;
                              Labs::save(current_.lab);
                              call("networks.authorizeMany", {{"uuids", uuids}}, [next](bool ok, const QVariantMap &r) {
                                  next(ok ? QString{} : r["message"].toString());
                              });
                          });
                      }});
    // 3. Keys: one for OmaWare to log in with, and each VM's own host key.
    steps.append({"Making SSH keys", [this, slug](Next next) {
                      QString error;
                      const auto dir = Labs::folderOf(slug);
                      QDir().mkpath(dir + "/hostkeys");
                      const auto agentKey = makeKey(Labs::keyPath(slug), "omaware-lab-" + slug, error);
                      if (agentKey.isEmpty()) return next(error);
                      current_.lab["agentKey"] = agentKey;
                      QVariantMap hostKeys;
                      for (const auto &v : current_.plan["vms"].toList()) {
                          const auto name = v.toMap()["name"].toString();
                          const auto path = dir + "/hostkeys/" + name.toLower();
                          const auto pub = makeKey(path, name.toLower(), error);
                          if (pub.isEmpty()) return next(error);
                          hostKeys[name] = QVariantMap{{"public", pub}, {"private", readText(path)}};
                      }
                      current_.plan["hostKeys"] = hostKeys;
                      Labs::save(current_.lab);
                      next({});
                  }});
    // 4. VMs.
    for (const auto &v : plan["vms"].toList()) {
        const auto spec = v.toMap();
        steps.append(
                {"Creating " + spec["name"].toString(), [this, spec, slug](Next next) {
                     const auto image = CloudImages::local(spec["os"].toString());
                     if (image.isEmpty()) return next("The " + spec["os"].toString() + " image is missing.");
                     QHash<QString, QString> bridges;
                     for (const auto &n : current_.lab["networks"].toList())
                         bridges[n.toMap()["name"].toString()] = n.toMap()["bridge"].toString();
                     CloudSeed::Settings seed;
                     seed.instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                     seed.hostname = CloudSeed::hostname(spec["name"].toString());
                     seed.user = current_.user;
                     seed.passwordHash = current_.passwordHash;
                     seed.sshKey = current_.lab["agentKey"].toString();
                     const auto hostKey = current_.plan["hostKeys"].toMap().value(spec["name"].toString()).toMap();
                     seed.hostKeyPrivate = hostKey["private"].toString();
                     seed.hostKeyPublic = hostKey["public"].toString();
                     seed.packages = spec["packages"].toStringList();
                     seed.commands = spec["setup"].toStringList();
                     seed.guestAgent = spec["internet"].toBool();
                     QVariantList networks;
                     for (const auto &n : spec["nics"].toList()) {
                         const auto mac = randomMac();
                         const auto bridge = bridges.value(n.toMap()["network"].toString());
                         if (bridge.isEmpty()) return next("A network for " + spec["name"].toString() + " is missing.");
                         networks.append(QVariantMap{{"id", "bridge:" + bridge}, {"mac", mac}});
                         seed.nics.append({mac, n.toMap()["ip"].toString()});
                     }
                     QVariantMap input{{"name", spec["name"]}, {"cpus", spec["cpus"]}, {"memoryMiB", spec["memoryMiB"]},
                             {"diskGiB", spec["diskGiB"]}, {"preset", image["preset"]}, {"firmware", "bios"},
                             {"sourceMode", "cloud"}, {"source", image["path"]}, {"networks", networks},
                             {"seed", QVariantMap{{"userData", CloudSeed::userData(seed)},
                                              {"metaData", CloudSeed::metaData(seed)},
                                              {"networkConfig", CloudSeed::networkConfig(seed)}}},
                             {"lab", QVariantMap{{"name", current_.plan["name"]}, {"slug", slug},
                                             {"login", current_.login}, {"user", current_.user}}}};
                     call("vm.create", input, [this, spec, slug, hostKey, image, next](bool ok, const QVariantMap &r) {
                         if (!ok) return next(spec["name"].toString() + ": " + r["message"].toString());
                         auto list = current_.lab["vms"].toList();
                         list.append(QVariantMap{{"name", spec["name"]}, {"uuid", r["uuid"]}, {"dir", r["storage"]},
                                 {"os", image["name"]}, {"reachable", spec["reachable"]}});
                         current_.lab["vms"] = list;
                         Labs::save(current_.lab);
                         // Pins the VM's host key under its UUID for OmaWare's SSH connections.
                         QFile known(Labs::folderOf(slug) + "/known_hosts");
                         if (known.open(QIODevice::Append))
                             known.write((r["uuid"].toString() + " " + hostKey["public"].toString().section(' ', 0, 1) +
                                          "\n")
                                             .toUtf8());
                         next({});
                     });
                 }});
    }
    // 5. A starting point to go back to, then start everything.
    for (const auto &v : plan["vms"].toList()) {
        const auto name = v.toMap()["name"].toString();
        steps.append(
                {"Saving a starting point for " + name, [this, name](Next next) {
                     QString uuid;
                     for (const auto &vm : current_.lab["vms"].toList())
                         if (vm.toMap()["name"] == name) uuid = vm.toMap()["uuid"].toString();
                     call("snapshots.create",
                             {{"uuid", uuid}, {"name", "Lab built"},
                                     {"notes", "Freshly built; first-boot setup runs again after restoring this."},
                                     {"memory", false}},
                             [next](bool ok, const QVariantMap &r) { next(ok ? QString{} : r["message"].toString()); });
                 }});
    }
    for (const auto &v : plan["vms"].toList()) {
        const auto name = v.toMap()["name"].toString();
        steps.append(
                {"Starting " + name, [this, name](Next next) {
                     QString uuid;
                     for (const auto &vm : current_.lab["vms"].toList())
                         if (vm.toMap()["name"] == name) uuid = vm.toMap()["uuid"].toString();
                     call("vm.power", {{"uuid", uuid}, {"action", "start"}},
                             [next](bool ok, const QVariantMap &r) { next(ok ? QString{} : r["message"].toString()); });
                 }});
    }
    buildStep(0);
}

void AgentBridge::buildStep(int index) {
    if (current_.id.isEmpty()) return;
    if (index >= current_.steps.size()) {
        finishBuild(true, "The lab is ready.");
        return;
    }
    build_["step"] = index;
    states_[current_.id]["message"] = current_.steps[index].first;
    reportBuild();
    wake(current_.id);
    current_.steps[index].second([this, index](const QString &error) {
        if (!error.isEmpty())
            finishBuild(false, current_.steps.value(index).first + ": " + error);
        else
            buildStep(index + 1);
    });
}

void AgentBridge::finishBuild(bool ok, const QString &message) {
    const auto id = current_.id;
    current_.lab["state"] = ok ? "ready" : "failed";
    current_.lab["message"] = message;
    Labs::save(current_.lab);
    // Each VM's private host key now lives only on its own setup disc; the lab keeps the public keys in known_hosts.
    QDir(Labs::folderOf(current_.lab["slug"].toString()) + "/hostkeys").removeRecursively();
    states_[id] = {{"state", ok ? "ready" : "failed"}, {"name", current_.lab["name"]}, {"slug", current_.lab["slug"]},
            {"message", ok ? message : message + " What was already made is kept; delete_lab removes it."}};
    build_ = {{"id", id}, {"name", current_.lab["name"]}, {"state", ok ? "ready" : "failed"},
            {"message", states_[id]["message"]}};
    note(ok ? "lab “" + current_.lab["name"].toString() + "” built"
            : "building “" + current_.lab["name"].toString() + "” failed: " + message,
            ok);
    current_ = {};
    localLab_ = false;
    emit buildChanged();
    emit labsChanged();
    wake(id);
}

// ---- Lab files --------------------------------------------------------------------------------
QString AgentBridge::labFile(const QString &slug) const {
    const auto lab = Labs::load(slug);
    return lab.isEmpty() || lab["plan"].toMap().isEmpty() ? QString() : LabFile::write(lab["plan"].toMap());
}

bool AgentBridge::exportLab(const QString &slug, const QString &url) const {
    const auto text = labFile(slug);
    if (text.isEmpty()) return false;
    QSaveFile file(url.startsWith("file:") ? QUrl(url).toLocalFile() : url);
    return file.open(QIODevice::WriteOnly) && file.write(text.toUtf8()) >= 0 && file.commit();
}

QString AgentBridge::importLab(const QString &url) {
    QFile file(url.startsWith("file:") ? QUrl(url).toLocalFile() : url);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) return "The lab file couldn't be read.";
    QString error;
    const auto plan = LabFile::read(QString::fromUtf8(file.readAll()), error);
    if (plan.isEmpty()) return error;
    if (!current_.id.isEmpty()) return "A lab is being built right now. Import this one when it's finished.";
    proposeLab(
            plan,
            [this](const QVariantMap &r) {
                const bool ok = r["ok"].toBool();
                if (!ok) localLab_ = !proposal_.isEmpty() && proposal_.value("local").toBool();
                // The checks are worded for agents; a person importing a file fixes it themselves.
                emit labImported(
                        ok, ok ? "Review the lab, set its password and click Build."
                               : r["error"].toString()
                                            .replace(" Ask the user what they want.", "")
                                            .replace(", or delete_lab it first", ", or delete that lab first"));
            },
            true);
    return {};
}

// ---- For the interface ------------------------------------------------------------------------
QVariantList AgentBridge::labs() const {
    QVariantList out;
    for (const auto &v : Labs::list()) {
        const auto lab = v.toMap();
        QStringList vms, nets;
        for (const auto &vm : lab["vms"].toList())
            vms << vm.toMap()["name"].toString();
        for (const auto &n : lab["networks"].toList())
            nets << n.toMap()["name"].toString();
        out.append(QVariantMap{{"name", lab["name"]}, {"slug", lab["slug"]}, {"state", lab["state"]},
                {"user", lab["user"]}, {"login", lab["login"]}, {"vms", vms}, {"networks", nets}});
    }
    return out;
}

QVariantList AgentBridge::logins() const {
    return Logins::list();
}

QString AgentBridge::generatePassword() const {
    return Logins::generate();
}

QVariantMap AgentBridge::vmLab(const QString &uuid) const {
    for (const auto &v : Labs::list()) {
        const auto lab = v.toMap();
        for (const auto &vm : lab["vms"].toList())
            if (vm.toMap()["uuid"] == uuid)
                return {{"lab", lab["name"]}, {"slug", lab["slug"]}, {"login", lab["login"]}, {"user", lab["user"]}};
    }
    return {};
}

QString AgentBridge::revealPassword(const QString &login) const {
    QString password, error;
    return Logins::password(login, password, error) ? password : QString{};
}
