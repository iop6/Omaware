// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentbridge.h"
#include "backend.h"
#include "agentprovision.h"
#include <QJsonDocument>
#include <QTimer>

namespace {
QVariantMap good(QVariantMap result) { return {{"ok",true},{"result",result}}; }
QVariantMap bad(QString error, QString code) { return {{"ok",false},{"error",error},{"result",QVariantMap{{"code",code}}}}; }
QVariantMap publicNetwork(const QVariantMap &n) {
    QVariantMap out;
    for (auto key : {"uuid","name","mode","cidr","bridge","active","available","reason","revision"}) if (n.contains(key)) out[key] = n[key];
    return out;
}
}
void AgentBridge::provisioningTool(const QString &tool, const QVariantMap &args, Reply reply) {
    QString why;
    if (!enabled_ || !AgentProvision::validate(tool,args,why)) { reply(bad(why.isEmpty() ? "Agent access disabled." : why,"invalid_argument")); return; }
    if (tool == "list_installation_media") { reply(good({{"items",AgentProvision::media()},{"limit",256}})); return; }
    if (tool == "list_owned_networks") {
        call("networks.list",{},[reply](bool ok, const QVariantMap &r) {
            if (!ok) { reply(bad(r["message"].toString(),"operation_failed")); return; }
            QVariantList items; for (const auto &v : r["items"].toList()) if (v.toMap()["managed"].toBool()) items.append(publicNetwork(v.toMap()));
            reply(good({{"items",items}}));
        }); return;
    }
    const auto id = args["request_id"].toString();
    if (tool == "provision_status") {
        reply(provisioningStates_.contains(id) ? good(provisioningStates_[id]) : bad("Unknown request ID. If OmaWare restarted, inspect inventory; do not blindly retry a mutation.","unknown_request")); return;
    }
    // A dry run never reserves a mutation ID, asks for approval or sends vm.create/networks.save.
    if (args.value("dry_run",false).toBool()) {
        call("networks.list",{},[reply,tool,args](bool ok,const QVariantMap &r) {
            QVariantMap input; QString error;
            if (!ok || !AgentProvision::prepare(tool,args,r["items"].toList(),input,error)) { reply(bad(ok ? error : r["message"].toString(),"invalid_argument")); return; }
            auto preview = args; preview["stopped"] = tool == "create_vm";
            reply(good({{"dry_run",true},{"validated_request",preview},{"execution_checks_pending",QStringList{"host capacity and OS support","standalone disk format and free storage","fresh media identities and network ownership/revisions","subnet conflicts and administrator authorization"}}}));
        }); return;
    }
    const QVariantMap request{{"tool",tool},{"args",args}};
    // Never evict deduplication records while the app lives. Bound both memory and active queue.
    const auto decision = AgentProvision::admission(id,request,provisioningRequests_,provisioningStates_);
    if (decision == "existing") { reply(good(provisioningStates_[id])); return; }
    if (decision == "request_conflict") { reply(bad("This request_id was already used with different arguments.",decision)); return; }
    if (decision == "session_limit") { reply(bad("Provisioning session limit reached; inspect completed operations before restarting OmaWare.",decision)); return; }
    if (decision == "busy") { reply(bad("Another provisioning request is pending. Poll its status first.",decision)); return; }
    provisioningRequests_[id] = request;
    provisioningStates_[id] = {{"request_id",id},{"tool",tool},{"state","preparing"}};
    const auto epoch = backend_->provisionEpoch();
    reply(good(provisioningStates_[id])); // Reply BEFORE any potentially long worker or approval wait.
    QTimer::singleShot(0,this,[this,tool,args,id,epoch] {
        auto fail = [this,id](QString error, QString state = "failed") { provisioningStates_[id]["state"] = state; provisioningStates_[id]["message"] = error; };
        call("networks.list",{},[this,tool,args,id,fail,epoch](bool ok,const QVariantMap &r) {
            QVariantMap input; QString error;
            if (!enabled_ || backend_->provisionEpoch() != epoch || !ok || !AgentProvision::prepare(tool,args,r["items"].toList(),input,error)) { fail(ok ? (error.isEmpty() ? "Agent access changed before preparation." : error) : r["message"].toString()); return; }
            provisioningStates_[id]["state"] = "awaiting_approval";
            QVariantList destinations;
            for (const auto &v : r["items"].toList()) {
                const auto n = v.toMap();
                if (args["networks"].toList().contains(n["uuid"]) || args["network"] == n["uuid"]) destinations.append(publicNetwork(n));
            }
            const auto destinationText = destinations.isEmpty() ? QString{} : "\nSelected network names, modes, subnets and revisions:\n" + QString::fromUtf8(QJsonDocument::fromVariant(destinations).toJson());
            const auto effects = tool == "create_vm" ? "Creates a stopped VM; disk imports copy guest identities and credentials into an independent disk. Network cables are connected to the selected owned destinations on first start. No installer, guest configuration or automatic boot." : tool == "create_network" ? "Creates and starts a host network, without autostart. NAT permits host/internet access; hostonly permits host access; isolated permits guest-only traffic. Administrator authorization may be required." : "Lets session VMs use this owned network via OmaWare's trusted helper. A separate administrator prompt may appear.";
            ask("Approve background provisioning?",tool+"\n"+QString::fromUtf8(QJsonDocument::fromVariant(args).toJson())+destinationText+"\n"+effects,"Approve",[this,tool,args,id,input,fail,epoch](bool yes) {
                if (!yes || !enabled_) { fail("Declined or agent access disabled.","declined"); return; }
                // Re-read inventory and media after approval; preserve the exact approved revisions/identity.
                call("networks.list",{},[this,tool,args,id,input,fail,epoch](bool ok,const QVariantMap &r) {
                    QVariantMap current; QString error;
                    if (!enabled_ || backend_->provisionEpoch() != epoch || !ok || !AgentProvision::prepare(tool,args,r["items"].toList(),current,error) || current != input) { fail("Approved media/network state changed or agent access disabled. Refresh inventory." ); return; }
                    provisioningStates_[id]["state"] = "running";
                    const auto op = tool == "create_vm" ? "vm.create" : tool == "create_network" ? "networks.save" : "networks.authorize";
                    note(tool+": approved background provisioning");
                    auto authorized = input; authorized["provisionEpoch"] = qulonglong(epoch);
                    call(op,authorized,[this,id](bool ok,const QVariantMap &r) {
                        auto &state = provisioningStates_[id]; state["state"] = ok ? "succeeded" : "failed";
                        // Explicit public allowlist: no XML, paths, subprocess output or credentials.
                        QVariantMap result;
                        for (auto key : {"uuid","subnet","authorized"}) if (r.contains(key)) result[key] = r[key];
                        state["result"] = result;
                        state["message"] = ok ? "Backend operation completed. Created VMs remain stopped." : "Backend operation failed; it may have partial effects. Check inventory and OmaWare activity before submitting a new request.";
                    });
                });
            });
        });
    });
}
