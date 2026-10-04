// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentbridge.h"
#include "agentreply.h"
#include "backend.h"
#include "agentprovision.h"
#include <QJsonDocument>
#include <QTimer>

using AgentReply::failure;
using AgentReply::success;

namespace {
QVariantMap publicNetwork(const QVariantMap &n) {
    QVariantMap out;
    for (auto key : {"uuid", "name", "mode", "cidr", "bridge", "active", "available", "reason", "revision"})
        if (n.contains(key)) out[key] = n[key];
    return out;
}
}

void AgentBridge::provisioningTool(const QString &tool, const QVariantMap &args, Reply reply) {
    QString why;
    if (!enabled_ || !AgentProvision::validate(tool, args, why))
        return reply(failure(why.isEmpty() ? "Agent access disabled." : why, "invalid_argument"));
    if (tool == "list_installation_media") return reply(success({{"items", AgentProvision::media()}, {"limit", 256}}));
    if (tool == "list_owned_networks") {
        call("networks.list", {}, [reply](bool ok, const QVariantMap &r) {
            if (!ok) return reply(failure(r["message"].toString(), "operation_failed"));
            QVariantList items;
            for (const auto &v : r["items"].toList())
                if (v.toMap()["managed"].toBool()) items.append(publicNetwork(v.toMap()));
            reply(success({{"items", items}}));
        });
        return;
    }
    const auto id = args["request_id"].toString();
    if (tool == "provision_status")
        return reply(provisioningStates_.contains(id)
                             ? success(provisioningStates_[id])
                             : failure("Unknown request ID. If OmaWare restarted, inspect inventory; do "
                                       "not blindly retry a mutation.",
                                       "unknown_request"));
    // A dry run never reserves a mutation ID, asks for approval or sends vm.create/networks.save.
    if (args.value("dry_run", false).toBool()) {
        call("networks.list", {}, [reply, tool, args](bool ok, const QVariantMap &r) {
            QVariantMap input;
            QString error;
            if (!ok || !AgentProvision::prepare(tool, args, r["items"].toList(), input, error))
                return reply(failure(ok ? error : r["message"].toString(), "invalid_argument"));
            auto preview = args;
            preview["stopped"] = tool == "create_vm";
            reply(success({{"dry_run", true}, {"validated_request", preview},
                    {"execution_checks_pending",
                            QStringList{"host capacity and OS support", "standalone disk format and free storage",
                                    "fresh media identities and network ownership/revisions",
                                    "subnet conflicts and administrator authorization"}}}));
        });
        return;
    }
    const QVariantMap request{{"tool", tool}, {"args", args}};
    // Never evict deduplication records while the app lives. Bound both memory and active queue.
    const auto decision = AgentProvision::admission(id, request, provisioningRequests_, provisioningStates_);
    if (decision == "existing") return reply(success(provisioningStates_[id]));
    if (decision == "request_conflict")
        return reply(failure("This request_id was already used with different arguments.", "request_conflict"));
    if (decision == "session_limit")
        return reply(
                failure("Provisioning session limit reached; inspect completed operations before restarting OmaWare.",
                        "session_limit"));
    if (decision == "busy")
        return reply(failure("Another provisioning request is pending. Poll its status first.", "busy"));
    provisioningRequests_[id] = request;
    provisioningStates_[id] = {{"request_id", id}, {"tool", tool}, {"state", "preparing"}};
    const auto epoch = backend_->provisionEpoch();
    reply(success(provisioningStates_[id])); // Reply BEFORE any potentially long worker or approval wait.
    QTimer::singleShot(0, this, [this, tool, args, id, epoch] {
        auto fail = [this, id](QString error, QString state = "failed", QString code = "precondition_failed") {
            provisioningStates_[id]["state"] = state;
            provisioningStates_[id]["message"] = error;
            provisioningStates_[id]["result"] = QVariantMap{{"code", state == "declined" ? QString("declined") : code}};
        };
        call("networks.list", {}, [this, tool, args, id, fail, epoch](bool ok, const QVariantMap &r) {
            QVariantMap input;
            QString error;
            if (!enabled_ || backend_->provisionEpoch() != epoch || !ok ||
                    !AgentProvision::prepare(tool, args, r["items"].toList(), input, error)) {
                fail(ok ? (error.isEmpty() ? "Agent access changed before preparation." : error)
                        : r["message"].toString());
                return;
            }
            provisioningStates_[id]["state"] = "awaiting_approval";
            QVariantList destinations;
            for (const auto &v : r["items"].toList()) {
                const auto n = v.toMap();
                if (args["networks"].toList().contains(n["uuid"]) || args["network"] == n["uuid"])
                    destinations.append(publicNetwork(n));
            }
            auto destinationText =
                    destinations.isEmpty()
                            ? QString{}
                            : "\nSelected network names, modes, subnets and revisions:\n" +
                                      QString::fromUtf8(QJsonDocument::fromVariant(destinations).toJson());
            if (tool == "create_vm" && !args["networks"].toList().isEmpty()) {
                // The order matters to the guest (its first NIC is the first adapter), so show it.
                QStringList order;
                int index = 0;
                for (const auto &v : args["networks"].toList()) {
                    QString label = v.toString() == "user" ? QString("Internet · private to this VM") : v.toString();
                    for (const auto &n : r["items"].toList())
                        if (n.toMap()["uuid"] == v)
                            label = n.toMap()["name"].toString() + " (" + n.toMap()["mode"].toString() + ")";
                    order << QString("%1. %2").arg(++index).arg(label);
                }
                destinationText += "\nNetwork adapters, in the order the guest sees them:\n" + order.join("\n");
            }
            const auto effects =
                    tool == "create_vm"
                            ? "Creates a stopped VM; disk imports copy guest identities and credentials into an "
                              "independent disk. Network cables are connected to the selected owned networks (and the "
                              "private internet connection, if chosen), in the order listed, on first start. No "
                              "installer, guest configuration or automatic boot."
                    : tool == "create_network"
                            ? QString(args.value("autostart", true).toBool()
                                              ? "Creates and starts a host network that also starts with the computer."
                                              : "Creates and starts a host network, without autostart (it is stopped "
                                                "after a reboot).") +
                                      " NAT permits host/internet access; hostonly permits host access; isolated "
                                      "permits guest-only traffic. Administrator authorization may be required."
                            : "Lets session VMs use this owned network via OmaWare's trusted helper. A separate "
                              "administrator prompt may appear.";
            ask("Approve background provisioning?",
                    tool + "\n" + QString::fromUtf8(QJsonDocument::fromVariant(args).toJson()) + destinationText +
                            "\n" + effects,
                    "Approve", [this, tool, args, id, input, fail, epoch](bool yes) {
                        if (!yes || !enabled_) {
                            fail("Declined or agent access disabled.", "declined");
                            return;
                        }
                        // Re-read inventory and media after approval; preserve the exact approved revisions/identity.
                        call("networks.list", {},
                                [this, tool, args, id, input, fail, epoch](bool ok, const QVariantMap &r) {
                                    QVariantMap current;
                                    QString error;
                                    if (!enabled_ || backend_->provisionEpoch() != epoch || !ok ||
                                            !AgentProvision::prepare(tool, args, r["items"].toList(), current, error) ||
                                            current != input) {
                                        fail("Approved media/network state changed or agent access disabled. Refresh "
                                             "inventory.",
                                                "failed", "state_changed");
                                        return;
                                    }
                                    provisioningStates_[id]["state"] = "running";
                                    const auto op = tool == "create_vm"        ? "vm.create"
                                                    : tool == "create_network" ? "networks.save"
                                                                               : "networks.authorize";
                                    note(tool + ": approved background provisioning");
                                    auto authorized = input;
                                    authorized["provisionEpoch"] = qulonglong(epoch);
                                    call(op, authorized, [this, id](bool ok, const QVariantMap &r) {
                                        auto &state = provisioningStates_[id];
                                        state["state"] = ok ? "succeeded" : "failed";
                                        // Never r["message"], which can hold helper output for the user.
                                        const auto shown = AgentProvision::publicResult(ok, r);
                                        state["result"] = shown["result"];
                                        state["message"] = shown["message"];
                                    });
                                });
                    });
        });
    });
}
