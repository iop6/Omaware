// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentbridge.h"
#include "agenttransfer.h"
#include "isolibrary.h"
#include "configuration.h"
#include "labs.h"
#include "paths.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
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
                view["scope"] = "Configuration, guest-agent connection, address and local disk/filesystem metadata checks; no guest commands, raw logs, repairs or connectivity probes.";
                // Hints need the VM's addresses, read on the agent's worker.
                callAgent("vm.addresses", {{"uuid", uuid}}, [this, details, view, reply](bool ok, const QVariantMap &r) mutable {
                    view["hints"] = AgentDiagnosis::hints(details, ok ? r : QVariantMap{});
                    reply(good(view));
                });
                return;
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
            if (args.contains("restart_timeout_seconds") && !integer(args, "restart_timeout_seconds", 10, 300)) { reply(bad("restart_timeout_seconds must be 10–300.", "invalid_argument")); return; }
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
        const bool restart = tool == "manage_network_adapter" && args.value("apply", "live") == "restart_if_needed";
        const int restartTimeout = args.value("restart_timeout_seconds", 120).toInt();
        auto execute = [this, op, input, reply, vm, restart, restartTimeout](bool yes, const QString &by) {
            if (!yes) { reply(bad("The user declined the operation.", "declined")); return; }
            note(op + (by == "session_grant" ? QString(": auto-approved by the session approval for ") + AgentGrants::label(AgentGrants::privateAdapters) : QString(": approved VM management operation")));
            // Every answer says who approved it, including after a restart.
            Reply tagged = [reply, by](const QVariantMap &out) { auto r = out; auto result = r["result"].toMap(); result["approved_by"] = by; r["result"] = result; reply(r); };
            call(op, input, [this, reply = tagged, vm, restart, restartTimeout](bool ok, const QVariantMap &r) {
                auto result = r; result.remove("requestTag");
                if (!ok) { reply(bad(r["message"].toString())); return; }
                if (result.contains("pending_change_count")) result["restart_needed"] = result["active"].toBool() && result["pending_change_count"].toInt() > 0;
                if (restart && result["restart_needed"].toBool()) { restartForPending(vm, result, restartTimeout, reply); return; }
                if (restart) result["restart"] = QVariantMap{{"state", "not_needed"}};
                reply(good(result));
            });
        };
        if (op == "hardware.save") {
            auto preview = input; preview["dryRun"] = true;
            call("hardware.save", preview, [this, args, vm, reply, execute](bool valid, const QVariantMap &r) {
                if (!valid) { reply(bad(r["message"].toString())); return; }
                auto result = r; result.remove("requestTag");
                if (args.value("dry_run", false).toBool()) { reply(good(result)); return; }
                ask("Change " + vm["short"].toString() + "?", "An AI agent requests these saved configuration changes (effective on next full start):\n" + QString::fromUtf8(QJsonDocument::fromVariant(r["changes"]).toJson()), "Save changes", [execute](bool yes) { execute(yes, "user"); });
            });
        } else {
            QString effect = tool == "clone_vm" ? "\nCreates a stopped independent copy, with new MACs and disconnected cables. Guest identities and credentials are copied." : "\nThis may interrupt the VM's network immediately; unsupported live edits remain pending.";
            if (restart) effect = QString("\nThis may interrupt the VM's network immediately. If the change can't be applied to the running VM, OmaWare then restarts %1: it resumes it if paused, asks the guest to shut down, waits up to %2 seconds and starts it again. It never forces power off.").arg(vm["short"].toString()).arg(restartTimeout);
            // Low-risk adapter changes the user allowed for this session skip the question; the worker still
            // checks revision, ownership, availability and containment as for any change.
            QString grantKind;
            if (tool == "manage_network_adapter") {
                QVariantMap target;
                for (const auto &v : details["networkOptions"].toList()) if (v.toMap()["id"] == input["networkId"]) target = v.toMap();
                grantKind = AgentGrants::forAdapterChange(args["action"].toString(), args["action"] == "remove" ? QVariantMap{} : target);
            }
            if (granted(grantKind)) { execute(true, "session_grant"); return; }
            ask("Change " + vm["short"].toString() + "?", "An AI agent requests " + tool + ":\n" + QString::fromUtf8(QJsonDocument::fromVariant(args).toJson()) + effect, "Approve", [execute](bool yes) { execute(yes, "user"); }, grantKind);
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

QVariantList AgentDiagnosis::hints(const QVariantMap &details, const QVariantMap &addresses) {
    QVariantList hints;
    auto hint = [&](const QString &code, const QString &text) { hints.append(QVariantMap{{"code", code}, {"hint", text}}); };
    const bool active = details["active"].toBool();
    if (!details["agentConfigured"].toBool())
        hint("no_guest_agent_channel", "The VM has no QEMU guest agent channel, so run_command, guest addresses and file transfers can't use the agent.");
    else if (active && !details["agentConnected"].toBool())
        hint("guest_agent_not_running", "The guest agent channel is there but no agent answers. Install and start qemu-guest-agent in the guest; run_command then works without any network, isolated ones included.");
    if (!details["changes"].toList().isEmpty() && active)
        hint("restart_needed", "Saved changes wait for a full shutdown and start. manage_network_adapter with apply restart_if_needed, or vm_power restart, applies them.");
    if (!active || addresses.isEmpty()) return hints;
    bool plugged = false, anyAddress = false;
    for (const auto &v : addresses["interfaces"].toList()) {
        const auto nic = v.toMap();
        if (nic["linkUp"].toBool()) plugged = true;
        if (!nic["ips"].toList().isEmpty()) anyAddress = true;
    }
    if (!plugged || anyAddress) return hints;
    if (addresses["agent"].toBool())
        hint("guest_has_no_ipv4", "The guest agent reports no IPv4 address on any of the VM's network cards: the guest hasn't configured them. Imported appliances often still name their old NIC (REMnux's netplan matched VMware's ens33, while the card is enp6s0 here) or expect a DHCP server that isn't on this network. Check the guest's network settings with run_command or on its screen.");
    else
        hint("no_ipv4_seen", "OmaWare sees no IPv4 address for any of the VM's network cards. That's normal on isolated networks (the host isn't on them) and while the guest boots. Otherwise the guest may not have configured its cards: imported appliances often still name their old NIC (for example netplan matching VMware's ens33 while the card is enp6s0 here). Check on its screen.");
    return hints;
}

void AgentBridge::restartForPending(const QVariantMap &vm, const QVariantMap &saved, int timeoutSeconds, Reply reply) {
    const auto uuid = vm["uuid"].toString(), name = vm["short"].toString();
    struct Wait { QElapsedTimer clock; bool finished = false; };
    auto state = std::make_shared<Wait>();
    auto finish = [this, state, reply, saved, uuid](bool ok, const QString &phase, const QString &message, const QString &code = {}) {
        if (state->finished) return;
        state->finished = true;
        // The current revision and pending count, whatever happened.
        callAgent("vm.details", {{"uuid", uuid}}, [reply, saved, state, ok, phase, message, code](bool read, const QVariantMap &d) {
            QVariantMap result{{"message", saved["message"].toString() + " " + message}, {"revision", read ? d["revision"] : saved["revision"]},
                {"pending_change_count", read ? d["changes"].toList().size() : saved["pending_change_count"].toInt()},
                {"restart", QVariantMap{{"state", phase}, {"waited_ms", state->clock.isValid() ? state->clock.elapsed() : 0}}}, {"saved", true}};
            if (ok) { reply(good(result)); return; }
            result["code"] = code;
            reply({{"ok", false}, {"error", message}, {"result", result}});
        });
    };
    note(name + ": restart to apply a pending adapter change");
    auto poll = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = poll;
    *poll = [this, uuid, name, state, finish, weak, timeoutSeconds] {
        auto keep = weak.lock(); if (!keep || state->finished) return;
        QString error;
        if (!enabled_ || findVm(uuid, error).isEmpty()) { finish(false, "failed", "Agent access or VM eligibility changed during the restart. The change is saved and still pending.", "access_denied"); return; }
        callAgent("vm.readiness", {{"uuid", uuid}}, [this, uuid, name, state, finish, keep, timeoutSeconds](bool ok, const QVariantMap &r) {
            if (state->finished) return;
            if (ok && !r["active"].toBool()) {
                call("vm.power", {{"uuid", uuid}, {"action", "start"}}, [finish](bool started, const QVariantMap &s) {
                    if (started) finish(true, "completed", "Restarted: the guest shut down cleanly and the VM started again with the change applied.");
                    else finish(false, "start_failed", "The guest shut down, but starting it again failed: " + s["message"].toString(), "restart_failed");
                });
                return;
            }
            if (state->clock.elapsed() >= timeoutSeconds * 1000LL) {
                finish(false, "timed_out", QString("%1 didn't shut down within %2 seconds, so it wasn't restarted. Nothing was forced: the VM keeps running, and the change is saved and still pending. "
                    "The guest may ignore ACPI shutdown requests (check its screen), or shut it down from inside the guest and start it with vm_power.").arg(name).arg(timeoutSeconds), "restart_timed_out");
                return;
            }
            QTimer::singleShot(1000, this, [keep] { (*keep)(); });
        });
    };
    auto shutdown = [this, uuid, state, finish, poll] {
        call("vm.power", {{"uuid", uuid}, {"action", "shutdown"}}, [state, finish, poll](bool ok, const QVariantMap &r) {
            if (!ok) { finish(false, "failed", "Asking the guest to shut down failed: " + r["message"].toString() + " The change is saved and still pending.", "restart_failed"); return; }
            state->clock.start();
            (*poll)();
        });
    };
    callAgent("vm.readiness", {{"uuid", uuid}}, [this, uuid, finish, shutdown](bool ok, const QVariantMap &r) {
        if (!ok) { finish(false, "failed", r["message"].toString(), "restart_failed"); return; }
        if (!r["active"].toBool()) { finish(true, "not_needed", "The VM is stopped; it uses the change on its next start."); return; }
        // A paused guest can't see the shutdown request.
        if (!r["paused"].toBool()) { shutdown(); return; }
        call("vm.power", {{"uuid", uuid}, {"action", "resume"}}, [finish, shutdown](bool resumed, const QVariantMap &s) {
            if (!resumed) { finish(false, "failed", "Resuming the paused VM failed: " + s["message"].toString() + " The change is saved and still pending.", "restart_failed"); return; }
            shutdown();
        });
    });
}

void AgentBridge::manageNetwork(const QVariantMap &args, Reply reply) {
    const auto action = args["action"].toString(), wanted = args["network"].toString().trimmed();
    if (action == "set_autostart" && args.value("autostart").typeId() != QMetaType::Bool) { reply(bad("set_autostart needs autostart true or false.", "invalid_argument")); return; }
    for (const auto &key : {"autostart", "mode", "subnet", "dhcp", "dhcp_start", "dhcp_end"})
        if (args.contains(key) && action != (QString(key) == "autostart" ? "set_autostart" : "edit")) { reply(bad(QString(key) + " only applies to " + (QString(key) == "autostart" ? "set_autostart." : "edit."), "invalid_argument")); return; }
    // Finds the owned network by UUID or name in a networks.list answer.
    auto find = [wanted](const QVariantMap &r) {
        for (const auto &v : r["items"].toList()) {
            const auto n = v.toMap();
            const auto name = n["name"].toString();
            if (n["managed"].toBool() && (n["uuid"] == wanted || name == wanted || name.compare("omaware-" + wanted, Qt::CaseInsensitive) == 0)) return n;
        }
        return QVariantMap{};
    };
    call("networks.list", {}, [this, args, action, wanted, find, reply](bool ok, const QVariantMap &r) {
        if (!ok) { reply(bad(r["message"].toString())); return; }
        const auto n = find(r);
        if (n.isEmpty()) { reply(bad("OmaWare didn't create a network called “" + wanted + "”. Use list_owned_networks.", "not_found")); return; }
        if (args["revision"] != n["revision"]) { reply(bad("The network changed; get its current revision from list_owned_networks.", "stale_revision")); return; }
        const auto uuid = n["uuid"].toString(), shown = QString(n["name"].toString()).remove(QRegularExpression("^omaware-"));
        const auto users = n["users"].toStringList() + n["systemUsers"].toStringList();
        if (QStringList{"stop", "edit", "delete"}.contains(action) && !users.isEmpty()) { reply(bad("VMs still use “" + shown + "”: " + users.join(", ") + ". Remove or move their adapters first.", "in_use")); return; }
        if (action == "edit" && n["active"].toBool()) { reply(bad("Stop “" + shown + "” before editing it (manage_network stop).", "network_active")); return; }
        if (action == "start" && n["active"].toBool()) { reply(good({{"message", "The network is already running."}, {"revision", n["revision"]}, {"active", true}, {"autostart", n["autostart"]}})); return; }
        if (action == "stop" && !n["active"].toBool()) { reply(good({{"message", "The network is already stopped."}, {"revision", n["revision"]}, {"active", false}, {"autostart", n["autostart"]}})); return; }
        if (action == "delete")
            for (const auto &lab : Labs::list())
                for (const auto &ln : lab.toMap()["networks"].toList())
                    if (ln.toMap()["uuid"] == uuid) { reply(bad("“" + shown + "” belongs to the lab “" + lab.toMap()["name"].toString() + "”; delete it with delete_lab.", "lab_network")); return; }
        QVariantMap settings;
        if (action == "edit") {
            const auto title = n["title"].toString().isEmpty() ? shown : n["title"].toString();
            settings = {{"uuid", uuid}, {"revision", n["revision"]}, {"name", title}, {"mode", args.value("mode", n["mode"])}, {"autostart", n["autostart"]}, {"start", false}, {"authorize", false}};
            const bool isolated = settings["mode"] == "isolated";
            if (isolated) { for (const auto &key : {"subnet", "dhcp", "dhcp_start", "dhcp_end"}) if (args.contains(key)) { reply(bad("Isolated networks have no subnet or DHCP.", "invalid_argument")); return; } }
            else {
                // Unchanged values are kept, so an agent can change just one of them. A new subnet needs a new
                // DHCP range when DHCP stays on; networkXml says so.
                settings["subnet"] = args.value("subnet", n["cidr"]);
                settings["dhcp"] = args.value("dhcp", n["dhcp"].toBool());
                settings["dhcpStart"] = args.value("dhcp_start", args.contains("subnet") ? QString{} : n["dhcpStart"].toString());
                settings["dhcpEnd"] = args.value("dhcp_end", args.contains("subnet") ? QString{} : n["dhcpEnd"].toString());
            }
            QString why; Configuration::networkXml(settings, uuid, n["bridge"].toString(), why);
            if (settings["subnet"].toString().isEmpty() && !isolated) why = "Give a subnet for nat and hostonly networks.";
            if (!why.isEmpty()) { reply(bad(why, "invalid_argument")); return; }
        }
        static const QMap<QString, QString> effects{
            {"start", "Starts the network. VMs on it can then reach what its mode allows."},
            {"stop", "Stops the network. No VM uses it."},
            {"set_autostart", "Changes whether the network starts when the computer starts."},
            {"edit", "Changes the network's mode and addresses. It stays stopped; no VM uses it. Starting it later exposes what the new mode allows (NAT: host and internet; hostonly: host)."},
            {"delete", "Stops and deletes the network. No VM uses it. This can't be undone."}};
        auto text = QString("An AI agent wants to %1 the network “%2” (%3%4).\n%5").arg(QString(action).replace('_', ' '), shown, n["mode"].toString(), n["cidr"].toString().isEmpty() ? QString{} : ", " + n["cidr"].toString(), effects.value(action));
        if (action == "set_autostart") text += args["autostart"].toBool() ? "\nNew setting: starts with the computer." : "\nNew setting: stays stopped after a reboot until started.";
        if (action == "edit") text += "\nNew settings:\n" + QString::fromUtf8(QJsonDocument::fromVariant(QVariantMap{{"mode", settings["mode"]}, {"subnet", settings["subnet"]}, {"dhcp", settings["dhcp"]}, {"dhcp_start", settings["dhcpStart"]}, {"dhcp_end", settings["dhcpEnd"]}}).toJson());
        ask((action == "delete" ? "Delete " : "Change ") + shown + "?", text, action == "delete" ? "Delete network" : "Approve", [this, args, action, n, uuid, shown, settings, find, reply](bool yes) {
            if (!yes || !enabled_) { reply(bad("The user declined the operation.", "declined")); return; }
            // Check again after approval: the network must be exactly the one the user saw.
            call("networks.list", {}, [this, args, action, n, uuid, shown, settings, find, reply](bool ok, const QVariantMap &r) {
                const auto now = ok ? find(r) : QVariantMap{};
                if (now.isEmpty() || now["revision"] != n["revision"] || now["uuid"] != uuid) { reply(bad("The network changed while waiting for approval. List it again.", "stale_revision")); return; }
                note("network " + shown + ": " + action);
                auto after = [this, uuid, find, reply](bool ok, const QVariantMap &r) {
                    if (!ok) { reply(bad(r["message"].toString())); return; }
                    call("networks.list", {}, [r, uuid, find, reply](bool, const QVariantMap &list) {
                        QVariantMap result{{"message", r["message"]}, {"uuid", uuid}};
                        for (const auto &v : list["items"].toList()) if (v.toMap()["uuid"] == uuid) {
                            const auto m = v.toMap();
                            result["revision"] = m["revision"]; result["active"] = m["active"]; result["autostart"] = m["autostart"]; result["mode"] = m["mode"]; result["subnet"] = m["cidr"];
                        }
                        if (!result.contains("revision")) result["deleted"] = true;
                        reply(good(result));
                    });
                };
                const QVariantMap base{{"uuid", uuid}, {"revision", n["revision"]}};
                if (action == "start") call("networks.start", base, after);
                else if (action == "stop") call("networks.stop", base, after);
                else if (action == "set_autostart") { auto in = base; in["autostart"] = args["autostart"]; call("networks.autostart", in, after); }
                else if (action == "edit") call("networks.save", settings, after);
                else if (action == "delete") {
                    auto remove = [this, base, after] { call("networks.remove", base, after); };
                    if (!n["active"].toBool()) remove();
                    else call("networks.stop", base, [remove, after](bool ok, const QVariantMap &r) { if (ok) remove(); else after(false, r); });
                }
            });
        });
    });
}

// ---- get_media ---------------------------------------------------------------------------------
// The OS Shop for agents: its catalogue, and downloads of its own sources only (never a URL an agent
// supplies), each approved by the user and checked like any shop download.
void AgentBridge::setMedia(IsoLibrary *library) { media_ = library; }
namespace {
QVariantMap mediaEntry(const QVariantMap &s) {
    const bool busy = QStringList{"downloading", "unpacking", "starting"}.contains(s["status"].toString());
    QVariantMap out{{"source", s["id"]}, {"name", s["name"]}, {"category", s["category"]},
        {"kind", s["kind"] == "page" ? "website_only" : s["media"] == "image" ? "vm_image" : "installer"},
        {"state", busy ? s["status"] : s["status"] == "ready" ? "available" : s["status"]}};
    if (s["kind"] == "page") { out["note"] = "Downloaded by the user on the publisher's website, not by OmaWare."; return out; }
    out["verification"] = s["trust"].toString().isEmpty() ? QVariant() : s["trust"];
    if (!s["trustNote"].toString().isEmpty()) out["verification_note"] = s["trustNote"];
    out["versions"] = s["versions"]; out["have_versions"] = s["haveVersions"];
    if (s.contains("languages")) { out["languages"] = s["languages"]; out["language"] = s["language"]; }
    if (!s["error"].toString().isEmpty()) out["error"] = s["error"];
    if (s["selectedSize"].toDouble() > 0) out["size_bytes"] = qint64(s["selectedSize"].toDouble());
    if (busy) { out["received_bytes"] = qint64(s["received"].toDouble()); if (s["total"].toDouble() > 0) out["total_bytes"] = qint64(s["total"].toDouble()); out["version"] = s["selectedVersion"]; }
    return out;
}
}
void AgentBridge::getMedia(const QVariantMap &args, Reply reply) {
    if (!media_) { reply(bad("The OS Shop isn't available in this OmaWare window.", "unavailable")); return; }
    const auto action = args["action"].toString();
    const auto sources = media_->sources();
    auto findSource = [&](const QString &id) { for (const auto &v : sources) if (v.toMap()["id"] == id) return v.toMap(); return QVariantMap{}; };
    if (action == "catalog") {
        bool unchecked = false;
        QVariantList list;
        for (const auto &v : sources) { list.append(mediaEntry(v.toMap())); if (v.toMap()["status"] == "unknown") unchecked = true; }
        // Reading the publishers' release lists takes a few seconds; versions fill in as they arrive.
        if (unchecked && !media_->checking() && media_->property("autoCheck").toBool()) media_->check();
        reply(good({{"sources", list}, {"checking", media_->checking()},
            {"next", media_->checking() ? "Release lists are still loading: call catalog again in a few seconds for versions." : "download with a source (and optionally version or language), then poll status. Installers are attached with manage_iso; VM images are imported by the user (Import VM)."}}));
        return;
    }
    const auto id = args["source"].toString();
    const auto source = findSource(id);
    if (source.isEmpty()) { reply(bad("Unknown source. Use get_media with action catalog.", "not_found")); return; }
    auto files = [this, id] {
        QVariantList out;
        for (const auto &v : media_->files()) {
            const auto f = v.toMap();
            if (f["source"] == id) out.append(QVariantMap{{"name", f["name"]}, {"version", f["version"]}, {"type", f["type"] == "disk" ? "vm_image" : "iso"}, {"verification", f["check"].toString().isEmpty() ? QString("added_by_user") : f["check"].toString()}});
        }
        return out;
    };
    if (action == "status") { auto out = mediaEntry(source); out["files"] = files(); reply(good(out)); return; }
    if (action != "download") { reply(bad("action must be catalog, download or status.", "invalid_argument")); return; }
    if (source["kind"] == "page") { reply(bad(source["name"].toString() + " comes from the publisher's website; ask the user to download it there and save it in OmaWare's ISO folder.", "website_only")); return; }
    if (QStringList{"downloading", "unpacking", "starting"}.contains(source["status"].toString())) { auto out = mediaEntry(source); out["code"] = "already_downloading"; reply(good(out)); return; }
    if (source["status"] != "ready") {
        if (!media_->checking() && media_->property("autoCheck").toBool()) media_->check();
        reply(bad("The publisher's release list isn't loaded yet" + (source["error"].toString().isEmpty() ? QString() : " (" + source["error"].toString() + ")") + ". Try again in a few seconds.", "not_ready"));
        return;
    }
    const auto versions = source["versions"].toStringList(), languages = source["languages"].toStringList();
    const auto version = args.value("version", versions.value(0)).toString(), language = args.value("language", source["language"]).toString();
    if (!versions.contains(version)) { reply(bad("Choose a version from the catalog: " + versions.join(", ") + ".", "invalid_argument")); return; }
    if (args.contains("language") && !languages.contains(language)) { reply(bad("Choose a language from the catalog.", "invalid_argument")); return; }
    if (source["haveVersions"].toStringList().contains(version) && (!args.contains("language") || language == source["language"])) {
        reply(good({{"state", "already_have"}, {"source", id}, {"version", version}, {"files", files()}}));
        return;
    }
    const bool unverified = source["trust"] == "unverified";
    const double size = version == source["selectedVersion"].toString() ? source["selectedSize"].toDouble() : 0;
    const auto text = "An AI agent wants to download " + source["name"].toString() + " " + version + (languages.isEmpty() ? QString() : " (" + language + ")")
        + (size > 0 ? QString(", about %1 GB,").arg(size / 1073741824.0, 0, 'f', 1) : QString()) + " from the publisher into OmaWare's media folder.\n\n"
        + (unverified ? "Unverified download: " + source["trustNote"].toString() : QString("It's checked against the publisher's checksum and thrown away if it doesn't match."))
        + (source["media"] == "image" ? "\n\nIt's a ready-made VM disk; you import it yourself with Import VM." : QString());
    ask("Download " + source["name"].toString() + "?", text, "Download", [this, id, version, language, languages, versions, reply, files](bool yes) {
        if (!yes) { reply(bad("The user declined the download.", "declined")); return; }
        if (!media_) { reply(bad("The OS Shop isn't available in this OmaWare window.", "unavailable")); return; }
        note("downloading " + id + " " + version + ": approved by the user");
        media_->setVersion(id, version == versions.value(0) ? QString() : version);
        // A new language (Microsoft's evaluation copies) needs the link looked up again first.
        auto start = std::make_shared<std::function<void(int)>>();
        *start = [this, id, version, reply, start, files](int tries) {
            if (!media_) { reply(bad("The OS Shop isn't available in this OmaWare window.", "unavailable")); return; }
            QVariantMap s;
            for (const auto &v : media_->sources()) if (v.toMap()["id"] == id) s = v.toMap();
            if (s["status"] == "checking" && tries < 60) { QTimer::singleShot(500, this, [start, tries] { (*start)(tries + 1); }); return; }
            if (s["selectedVersion"] != version || !media_->download(id)) {
                QVariantMap now;
                for (const auto &v : media_->sources()) if (v.toMap()["id"] == id) now = v.toMap();
                reply(bad("The download couldn't start" + (now["error"].toString().isEmpty() ? QString(".") : ": " + now["error"].toString()), "download_failed"));
                return;
            }
            auto out = mediaEntry(s); out["state"] = "started"; out["version"] = version;
            out["next"] = "Poll get_media status for progress. When it's done, the file is in files.";
            reply(good(out));
        };
        if (!languages.isEmpty()) media_->setLanguage(id, language);
        (*start)(0);
    });
}
