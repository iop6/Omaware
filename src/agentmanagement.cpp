// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentbridge.h"
#include "agenttransfer.h"
#include "labs.h"
#include "paths.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <memory>

namespace {
QVariantMap good(QVariantMap result) { return {{"ok", true}, {"result", result}}; }
QVariantMap bad(QString message, QString code = "operation_failed") { return {{"ok", false}, {"error", message}, {"result", QVariantMap{{"code", code}}}}; }
QVariantMap publicDetails(const QVariantMap &r) {
    QVariantMap out;
    for (const auto &key : {"uuid", "name", "revision", "active", "vcpus", "currentMemoryMiB", "memoryMiB", "cpuMode", "cpuModel", "cpuTopology", "architecture", "machine", "firmware", "secureBoot", "bootOrder", "disks", "displays", "agentConfigured", "agentConnected", "interfaces", "liveInterfaces", "networkOptions", "pendingConflict", "diskDiagnostics"})
        if (r.contains(key)) out[key] = r[key];
    QVariantList disks;
    for (const auto &v : r.value("disks").toList()) {
        const auto d = v.toMap(); QVariantMap disk;
        for (const auto &key : {"target", "device", "bus", "format", "readOnly", "bootOrder"}) if (d.contains(key)) disk[key] = d[key];
        disk["media_present"] = !d["source"].toString().isEmpty();
        disks.append(disk);
    }
    out["disks"] = disks;
    out["pending_change_count"] = r.value("changes").toList().size();
    return out;
}
bool integer(const QVariantMap &args, const QString &key, int low, int high) {
    bool ok = false; const double n = args.value(key).toDouble(&ok);
    return ok && args.value(key).typeId() != QMetaType::Bool && n >= low && n <= high && n == int(n);
}
QVariantList localIsos() {
    QVariantList out;
    QDir dir(Paths::isos());
    for (const auto &f : dir.entryInfoList({"*.iso", "*.ISO"}, QDir::Files | QDir::NoSymLinks, QDir::Name))
        if (f.isReadable()) out.append(QVariantMap{{"name", f.fileName()}, {"bytes", f.size()}});
    return out;
}
}

void AgentBridge::managementTool(const QString &tool, const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto uuid = vm["uuid"].toString();
    if (tool == "manage_iso" && args["action"] == "list") { reply(good({{"items", localIsos()}})); return; }
    callAgent("vm.details", {{"uuid", uuid}}, [this, tool, vm, uuid, args, reply](bool ok, const QVariantMap &details) {
        if (!ok) { reply(bad(details["message"].toString())); return; }
        auto view = publicDetails(details);
        if (tool == "vm_details" || tool == "diagnose_vm") {
            const bool ssh = !vmLab(uuid).isEmpty();
            view["transports"] = QVariantMap{{"guest_agent_connected", details["agentConnected"]}, {"lab_ssh_configured", ssh}, {"ssh_reachability_verified", false}, {"transfer", "Linux guest with python3; guest agent or lab SSH"}};
            view["capabilities"] = QVariantMap{{"clone_modes", QStringList{"full"}}, {"clone_source", "existing independent checkpoint"}, {"transfer_max_bytes", AgentTransfer::limit}, {"transfer_directory", Paths::root() + "/transfers"}};
            view["restrictions"] = QStringList{"Owned, persistent, non-contained VMs only", "Hardware and ISO changes apply on next full start", "Adapter edits may apply live", "Mutations require approval in OmaWare", "No linked clones, host shell, arbitrary host paths, credentials or raw XML"};
            if (tool == "diagnose_vm") {
                QVariantList checks;
                checks.append(QVariantMap{{"check", "running"}, {"ok", details["active"]}});
                checks.append(QVariantMap{{"check", "guest_agent_connected"}, {"ok", details["agentConnected"]}});
                checks.append(QVariantMap{{"check", "configuration_consistent"}, {"ok", !details["pendingConflict"].toBool()}});
                view["checks"] = checks;
                view["scope"] = "Configuration, guest-agent connection and local disk/filesystem metadata checks; no guest commands, raw logs, repairs or connectivity probes.";
            }
            reply(good(view)); return;
        }
        QString op; QVariantMap input{{"uuid", uuid}, {"revision", details["revision"]}};
        if (args.contains("revision") && args["revision"] != details["revision"]) { reply(bad("The configuration changed; refresh vm_details.", "stale_revision")); return; }
        if (tool == "update_vm_resources") {
            if ((!args.contains("cpus") && !args.contains("memory_mib")) || (args.contains("cpus") && !integer(args, "cpus", 1, 256)) || (args.contains("memory_mib") && !integer(args, "memory_mib", 256, 1048576))) { reply(bad("Supply integer cpus (1–256) and/or memory_mib (256–1048576).", "invalid_argument")); return; }
            op = "hardware.save";
            input["cpus"] = args.value("cpus", details["vcpus"]);
            input["memoryMiB"] = args.value("memory_mib", details["currentMemoryMiB"]);
        } else if (tool == "manage_iso") {
            if (args["action"] != "attach" && args["action"] != "eject") { reply(bad("action must be list, attach or eject.", "invalid_argument")); return; }
            op = "hardware.save"; input["iso"] = QString{};
            if (args["action"] == "attach") {
                bool found = false;
                for (const auto &v : localIsos()) if (v.toMap()["name"] == args["iso"]) found = true;
                if (!found) { reply(bad("Choose an ISO name from manage_iso list.", "invalid_argument")); return; }
                input["iso"] = Paths::isos() + "/" + args["iso"].toString();
            }
        } else if (tool == "manage_network_adapter") {
            if (args["action"] == "list") { reply(good(view)); return; }
            const auto action = args["action"].toString();
            if (!QStringList{"add", "remove", "update"}.contains(action)) { reply(bad("action must be list, add, remove or update.", "invalid_argument")); return; }
            QVariantMap nic;
            for (const auto &v : details["interfaces"].toList()) if (v.toMap()["mac"].toString().compare(args["mac"].toString(), Qt::CaseInsensitive) == 0) nic = v.toMap();
            if (action != "add" && nic.isEmpty()) { reply(bad("Select an existing adapter by MAC.", "invalid_argument")); return; }
            if (action == "add" && !args["mac"].toString().isEmpty()) { reply(bad("New adapters receive an automatically generated MAC.", "invalid_argument")); return; }
            op = "adapter.save";
            input["mac"] = action == "add" ? QString{} : nic["mac"].toString();
            input["networkId"] = args.value("network_id", nic["networkId"]);
            input["model"] = args.value("model", nic.value("model", "virtio"));
            input["linkUp"] = args.value("plugged", nic.value("linkUp", true));
            input["remove"] = action == "remove";
            if (action != "remove") {
                bool found = false;
                for (const auto &v : details["networkOptions"].toList()) if (v.toMap()["id"] == input["networkId"] && v.toMap()["available"].toBool()) found = true;
                if (!found) { reply(bad("Choose an available network_id from the adapter list.", "invalid_argument")); return; }
            }
        } else if (tool == "clone_vm") {
            if (args.value("mode", "full") != "full") { reply(bad("Only independent full clones from an existing checkpoint are supported.", "unsupported")); return; }
            if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]{0,47}$").match(args["name"].toString()).hasMatch() || !QRegularExpression("^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$").match(args["snapshot"].toString()).hasMatch()) { reply(bad("Supply a checkpoint id and a new VM name (1–48 letters, digits, dots, dashes).", "invalid_argument")); return; }
            op = "snapshots.clone"; input["id"] = args["snapshot"]; input["name"] = args["name"];
        }
        auto execute = [this, op, input, reply](bool yes) {
            if (!yes) { reply(bad("The user declined the operation.", "declined")); return; }
            note(op + ": approved VM management operation");
            call(op, input, [reply](bool ok, const QVariantMap &r) { auto result = r; result.remove("requestTag"); reply(ok ? good(result) : bad(r["message"].toString())); });
        };
        if (op == "hardware.save") {
            auto preview = input; preview["dryRun"] = true;
            call("hardware.save", preview, [this, args, vm, reply, execute](bool valid, const QVariantMap &r) {
                if (!valid) { reply(bad(r["message"].toString())); return; }
                auto result = r; result.remove("requestTag");
                if (args.value("dry_run", false).toBool()) { reply(good(result)); return; }
                ask("Change " + vm["short"].toString() + "?", "An AI agent requests these saved configuration changes (effective on next full start):\n" + QString::fromUtf8(QJsonDocument::fromVariant(r["changes"]).toJson()), "Save changes", execute);
            });
        } else {
            ask("Change " + vm["short"].toString() + "?", "An AI agent requests " + tool + ":\n" + QString::fromUtf8(QJsonDocument::fromVariant(args).toJson()) + (tool == "clone_vm" ? "\nCreates a stopped independent copy, with new MACs and disconnected cables. Guest identities and credentials are copied." : "\nThis may interrupt the VM's network immediately; unsupported live edits remain pending."), "Approve", execute);
        }
    });
}

void AgentBridge::waitForVm(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto condition = args.value("condition", "running").toString();
    auto options = args; if (!options.contains("timeout_seconds")) options["timeout_seconds"] = 60;
    if (!QStringList{"running", "guest_agent", "ssh", "cloud_init"}.contains(condition) || !integer(options, "timeout_seconds", 0, 120)) { reply(bad("Choose running, guest_agent, ssh or cloud_init and timeout_seconds 0–120.", "invalid_argument")); return; }
    struct Wait { QElapsedTimer clock; bool finished = false; };
    auto state = std::make_shared<Wait>(); state->clock.start();
    const int limit = options["timeout_seconds"].toInt() * 1000;
    auto finish = [state, reply, condition](bool ready) {
        if (state->finished) return;
        state->finished = true;
        reply(good({{"condition", condition}, {"ready", ready}, {"timed_out", !ready}, {"elapsed_ms", state->clock.elapsed()}}));
    };
    auto poll = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = poll;
    *poll = [this, vm, condition, state, limit, finish, weak, reply] {
        auto keep = weak.lock(); if (!keep || state->finished) return;
        QString error;
        auto current = findVm(vm["uuid"].toString(), error);
        if (!enabled_ || current.isEmpty()) { state->finished = true; reply(bad("Agent access or VM eligibility changed.", "access_denied")); return; }
        auto observed = [this, state, limit, finish, keep](bool ready) {
            if (state->finished) return;
            if (ready || state->clock.elapsed() >= limit) { finish(ready); return; }
            QTimer::singleShot(1000, this, [keep] { (*keep)(); });
        };
        if (condition == "running" || condition == "guest_agent") {
            callAgent("vm.readiness", {{"uuid", vm["uuid"]}, {"probeAgent", condition == "guest_agent"}}, [observed, condition](bool ok, const QVariantMap &r) { observed(ok && r[condition].toBool()); }); return;
        }
        const auto lab = Labs::load(vmLab(vm["uuid"].toString()).value("slug").toString());
        auto command = condition == "ssh" ? QString("true") : QString("cloud-init status --format json");
        auto checked = [observed, condition](const QVariantMap &r) {
            const auto result = r["result"].toMap();
            bool ready = r["ok"].toBool() && result.value("exit_code", -1).toInt() == 0;
            if (condition == "cloud_init") ready = ready && QJsonDocument::fromJson(result["stdout"].toString().toUtf8()).toVariant().toMap()["status"] == "done";
            observed(ready);
        };
        if (condition == "ssh") {
            if (lab.isEmpty()) { state->finished = true; reply(bad("SSH readiness requires a lab VM.", "unsupported")); return; }
            runOverSsh(vm, lab, command, 5, checked);
        } else runCommand(vm, {{"command", command}, {"timeout_seconds", 5}}, checked);
    };
    // The response deadline does not wait for queued probes. No new probes are sent afterwards.
    QTimer::singleShot(limit, this, [finish] { finish(false); });
    (*poll)();
}

void AgentBridge::transferFile(const QVariantMap &vm, const QVariantMap &args, Reply reply) {
    const auto direction = args["direction"].toString(), file = args["file"].toString(), guest = args["guest_path"].toString();
    if (!QStringList{"upload", "download"}.contains(direction) || !AgentTransfer::validName(file) || !AgentTransfer::validGuestPath(guest)) { reply(bad("Choose upload/download, a plain transfer file name, and an absolute non-secret guest path.", "invalid_argument")); return; }
    const bool overwrite = args.value("overwrite", false).toBool();
    ask("Transfer a file?", QString("An AI agent requests %1 of %2 %3 %4 in %5. Maximum 32 KiB. %6").arg(direction, file, direction == "upload" ? "to" : "from", guest, vm["short"].toString(), overwrite ? "Replacing the destination is allowed." : "Existing destinations will not be replaced."), "Transfer", [this, vm, reply, direction, file, guest, overwrite](bool yes) {
        if (!yes || !enabled_) { reply(bad("Transfer declined or agent access disabled.", "declined")); return; }
        QByteArray data; QString error;
        if (direction == "upload" && !AgentTransfer::read(file, data, error)) { reply(bad(error)); return; }
        const auto command = AgentTransfer::command(direction, guest, data, overwrite);
        auto completed = [this, reply, direction, file, overwrite, data](const QVariantMap &r) {
            if (!enabled_) { reply(bad("Agent access was turned off.", "access_denied")); return; }
            if (!r["ok"].toBool() || r["result"].toMap().value("exit_code", -1).toInt() != 0) { reply(bad("Guest transfer failed. Requires Linux, python3, access to the file, and a non-existing destination unless overwrite is set.")); return; }
            QByteArray bytes = data; QString error;
            if (direction == "download") {
                const auto encoded = r["result"].toMap()["stdout"].toString().trimmed().toLatin1();
                const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded || decoded.decoded.size() > AgentTransfer::limit) { reply(bad("Invalid or oversized guest transfer response.")); return; }
                bytes = decoded.decoded;
                if (!AgentTransfer::write(file, bytes, overwrite, error)) { reply(bad(error)); return; }
            }
            reply(good({{"direction", direction}, {"file", file}, {"bytes", bytes.size()}, {"directory", Paths::root() + "/transfers"}}));
        };
        note(vm["short"].toString() + ": approved file transfer (contents not logged)");
        callAgent("vm.agentExec", {{"uuid", vm["uuid"]}, {"command", command}, {"timeout", 30}}, [this, vm, command, completed](bool ok, const QVariantMap &r) {
            if (ok) { completed(good({{"exit_code", r["exitCode"]}, {"stdout", r["stdout"]}})); return; }
            if (!r["noAgent"].toBool()) { completed(bad("Guest agent transfer failed.")); return; }
            const auto lab = Labs::load(vmLab(vm["uuid"].toString()).value("slug").toString());
            if (lab.isEmpty()) { completed(bad("No transfer transport available.")); return; }
            runOverSsh(vm, lab, command, 30, completed);
        });
    });
}
