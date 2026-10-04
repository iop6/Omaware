// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcpserver.h"
#include "agentprovision.h"
#include "agentbridge.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

namespace {
const char *instructions = R"(OmaWare runs virtual machines (QEMU/KVM) on the user's computer. With these tools you can see its VMs and networks, build labs (networks plus VMs set up from cloud images), use a VM's screen (screenshot, click, type), run commands inside VMs, take and restore snapshots, and plug or pull virtual network cables.

How to work with it:
- Start with omaware_overview.
- For local appliances/installers, use list_installation_media, create_network, list_owned_networks, authorize_network and create_vm. Mutations return a request_id immediately; poll provision_status (a failure has result.code and result.reason). Approval remains in OmaWare; no desktop focus is needed by the agent. Created VMs remain stopped. Retry lost responses with the SAME request_id and arguments, never a fresh ID; after app restart inspect inventory first. These tools do not install FLARE or configure guests.
- create_vm networks is an ordered list of owned network UUIDs and "user" (the VM's private internet connection); the first entry is the guest's first NIC. For a router VM, put its WAN first.
- manage_network starts, stops, edits, deletes or sets autostart for networks OmaWare created, after the user approves. manage_network_adapter with apply "restart_if_needed" also restarts the VM (clean guest shutdown, then start) when a change can't be applied live.
- Change responses include the new revision; use it for the next change instead of listing again.
- Missing an installer? get_media lists the OS Shop's official ISOs and VM images (with versions); download asks the user, then poll status. Attach a finished ISO with manage_iso or use it with create_vm.
- To build something, write a lab plan and call propose_lab. Ask the user what VM user name they want first. Never ask the user for a password and never put one in a plan: OmaWare asks for the password in its own window when the user approves the plan, and type_login types it for you when a login screen needs it.
- After propose_lab, call lab_status with wait_seconds until the lab is ready, failed or declined. Building downloads images (once), creates networks (the user may be asked for their computer password once), creates the VMs and starts them.
- run_command works on any network, isolated ones included, when the QEMU guest agent runs in the VM (it uses a virtio-serial channel, not the network). Without the agent it falls back to SSH, only for lab VMs on "internet" or "private" networks. Otherwise use the VM's screen.
- First boot setup takes one to three minutes; run_command "cloud-init status --wait" waits for it.
- For routers and appliances without a guest agent, prefer serial_console (text in, text out) to the screen when the guest has a serial console.
- For screens: take a screenshot, then send vm_input actions using that screenshot's pixel coordinates. vm_input returns a new screenshot. Use these instead of virsh send-key or virsh screenshot, which can hang while OmaWare shows the console.
- Deleting a lab and restoring a snapshot ask the user to confirm in OmaWare; if they say no, don't retry.)";

QJsonObject schema(const QJsonObject &properties, const QStringList &required = {}) {
    QJsonObject s{{"type", "object"}, {"properties", properties}};
    if (!required.isEmpty()) s["required"] = QJsonArray::fromStringList(required);
    return s;
}
QJsonObject prop(const QString &type, const QString &description) { return {{"type", type}, {"description", description}}; }
QJsonObject tool(const QString &name, const QString &title, const QString &description, const QJsonObject &input, bool readOnly = false, bool destructive = false) {
    return {{"name", name}, {"title", title}, {"description", description}, {"inputSchema", input},
        {"annotations", QJsonObject{{"readOnlyHint", readOnly}, {"destructiveHint", destructive}, {"openWorldHint", false}}}};
}
QJsonArray toolList() {
    const auto vm = prop("string", "The VM's name (as in omaware_overview) or UUID.");
    const QJsonObject plan{
        {"type", "object"},
        {"description", "The lab. Example: {\"name\": \"Web lab\", \"user\": \"alex\", \"networks\": [{\"name\": \"dmz\", \"type\": \"internet\"}, {\"name\": \"lan\", \"type\": \"isolated\"}], "
            "\"vms\": [{\"name\": \"fw\", \"os\": \"debian\", \"networks\": [\"dmz\", {\"network\": \"lan\", \"ip\": \"172.30.1.1\"}]}, {\"name\": \"web1\", \"os\": \"ubuntu\", \"memory_mib\": 2048, \"networks\": [\"dmz\"], \"packages\": [\"nginx\"]}]}"},
        {"properties", QJsonObject{
            {"name", prop("string", "Lab name, 1–32 letters, numbers, spaces or dashes. Networks are named <lab>-<network>.")},
            {"user", prop("string", "The user name to create in every VM (ask the user). Lower-case; not root, admin, ubuntu, debian or fedora.")},
            {"networks", QJsonObject{{"type", "array"}, {"description", "Up to 8 networks."}, {"items", schema({
                {"name", prop("string", "1–20 lower-case letters, digits or dashes.")},
                {"type", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"internet", "private", "isolated"}},
                    {"description", "internet: VMs reach each other, this computer and the internet (NAT, DHCP). private: VMs reach each other and this computer, no internet (DHCP). isolated: VMs only reach each other; fixed addresses, and OmaWare can only use these VMs through their screens."}}},
                {"subnet", prop("string", "Optional IPv4 subnet such as 10.20.0.0/24. Needed to give VMs fixed addresses on internet or private networks; isolated networks default to 172.30.N.0/24.")}}, {"name", "type"})}}},
            {"vms", QJsonObject{{"type", "array"}, {"description", "1 to 12 VMs."}, {"items", schema({
                {"name", prop("string", "1–31 letters, digits or dashes; also the VM's host name.")},
                {"os", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"ubuntu", "debian", "fedora"}}, {"description", "Cloud image: newest Ubuntu LTS server, Debian 13 or Fedora Cloud. Default ubuntu."}}},
                {"cpus", prop("integer", "Default 2.")},
                {"memory_mib", prop("integer", "Default 2048; at least 512.")},
                {"disk_gib", prop("integer", "Default 16; 8–512.")},
                {"networks", QJsonObject{{"type", "array"}, {"description", "Up to 4: a network name, or {\"network\": name, \"ip\": \"10.20.0.5\"} for a fixed address. On internet and private /24 networks, avoid .100–.200 (DHCP)."},
                    {"items", QJsonObject{{"anyOf", QJsonArray{QJsonObject{{"type", "string"}}, schema({{"network", prop("string", "Network name.")}, {"ip", prop("string", "Fixed IPv4 address.")}}, {"network"})}}}}}},
                {"packages", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", "Packages to install on first boot (needs an internet network)."}}},
                {"setup", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", "Shell commands run once as root at the end of first boot."}}}}, {"name"})}}}}},
        {"required", QJsonArray{"name", "user", "vms"}}};
    const QJsonObject action{{"type", "object"}, {"properties", QJsonObject{
        {"type", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"click", "double_click", "move", "drag", "scroll", "type", "key", "wait"}}}},
        {"x", prop("number", "Screenshot pixel column.")}, {"y", prop("number", "Screenshot pixel row.")},
        {"to_x", prop("number", "drag: end column.")}, {"to_y", prop("number", "drag: end row.")},
        {"button", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"left", "right", "middle"}}}},
        {"amount", prop("integer", "scroll: clicks of the wheel, positive scrolls down.")},
        {"text", prop("string", "type: text to type (US keyboard layout; \\n presses Enter).")},
        {"keys", prop("string", "key: a key or combination such as enter, esc, tab, ctrl+alt+t, ctrl+alt+delete, win+r, f2.")},
        {"seconds", prop("number", "wait: up to 10 seconds.")}}}, {"required", QJsonArray{"type"}}};
    return {
        tool("vm_details", "VM details", "Saved and live configuration, revision, actual capabilities, transports and restrictions. No raw XML or credentials.", schema({{"vm", vm}}, {"vm"}), true),
        tool("diagnose_vm", "Diagnose VM", "Safe configuration diagnostics only; no guest logs, repairs or shell probes.", schema({{"vm", vm}}, {"vm"}), true),
        tool("wait_for_vm", "Wait for VM", "Bounded readiness observation; returns ready and timed_out. SSH requires a lab VM; cloud-init requires a Linux guest. Does not start the VM.", schema({{"vm", vm}, {"condition", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"running", "guest_agent", "ssh", "cloud_init"}}}}, {"timeout_seconds", prop("integer", "0–120; default 60. Response deadline, not cancellation of an in-flight probe.")}}, {"vm"}), true),
        tool("transfer_file", "Transfer file", "Upload/download a file (max 32 KiB) between a Linux guest with python3 and OmaWare's private transfers directory. Requires UI approval. No arbitrary host paths, symlinks, hardlinks or credential filenames; contents are not returned or logged.", schema({{"vm", vm}, {"direction", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"upload", "download"}}}}, {"file", prop("string", "Plain filename in the transfers directory reported by vm_details; upload files must be placed there by the user.")}, {"guest_path", prop("string", "Absolute non-secret guest file path; symlink components refused.")}, {"overwrite", prop("boolean", "Default false. Replacement must also be approved in OmaWare.")}}, {"vm", "direction", "file", "guest_path"}), false, true),
        tool("update_vm_resources", "Update resources", "Validate CPU/RAM and preview or approve saved settings. Changes apply on next full start, never hotplug. Host capacity and advanced configuration checks apply; revision is checked again after approval.", schema({{"vm", vm}, {"cpus", prop("integer", "1–256, within host capacity.")}, {"memory_mib", prop("integer", "256–1048576, within host capacity.")}, {"revision", prop("string", "Optional expected revision from vm_details.")}, {"dry_run", prop("boolean", "Validate and return changes without prompting or saving.")}}, {"vm"}), false, true),
        tool("clone_vm", "Clone VM", "Independent full clone of an existing checkpoint using OmaWare's clone backend, after approval. New VM stays stopped with new MACs and cables down. Guest identities/credentials remain copied. No linked cloning or automatic start.", schema({{"vm", vm}, {"snapshot", prop("string", "Checkpoint id from list_snapshots, not a legacy internal snapshot.")}, {"name", prop("string", "New VM name; 1–48 letters, digits, dots or dashes.")}, {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"full"}}}}}, {"vm", "snapshot", "name"})),
        tool("manage_iso", "Manage ISO", "List local ISO library or attach/eject the first optical drive using saved hardware settings and approval. No arbitrary paths; get new ISOs with get_media. Effective on next full start.", schema({{"vm", vm}, {"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"list", "attach", "eject"}}}}, {"iso", prop("string", "Exact filename from list, for attach.")}, {"revision", prop("string", "Expected configuration revision.")}, {"dry_run", prop("boolean", "Validate attach/eject without saving or approval.")}}, {"vm", "action"}), false, true),
        tool("manage_network_adapter", "Manage adapter", "List adapters and available network IDs; add, remove or update through the existing revision-protected network backend after UI approval. Applies live when the guest supports hot-plug; otherwise the change stays pending until a full shutdown and start (pending_change_count), unless apply is restart_if_needed. Returns the new revision.", schema({{"vm", vm}, {"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"list", "add", "remove", "update"}}}}, {"mac", prop("string", "Existing adapter MAC for update/remove; omit for add.")}, {"network_id", prop("string", "Available ID from list (\"user\" is the private internet connection); required for add.")}, {"model", prop("string", "Supported adapter model, default virtio for add.")}, {"plugged", prop("boolean", "Cable state; default true for add.")}, {"revision", prop("string", "Expected configuration revision.")},
            {"apply", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"live", "restart_if_needed"}}, {"description", "live (default): apply to the running VM when possible, else leave the change pending. restart_if_needed: if the change is still pending afterwards, resume a paused VM, ask the guest to shut down cleanly, wait up to restart_timeout_seconds and start it again; never forces power off. Covered by the same approval."}}},
            {"restart_timeout_seconds", prop("integer", "10–300, default 120: how long to wait for the guest to shut down.")}}, {"vm", "action"}), false, true),
        tool("manage_network", "Manage network", "Start, stop, set_autostart, edit or delete a network OmaWare created, after the user approves in OmaWare. stop, edit and delete are refused while any VM uses the network; edit needs it stopped; delete stops it first. Lab networks are deleted with delete_lab. Returns the new revision.", schema({{"network", prop("string", "Owned network UUID or name from list_owned_networks.")}, {"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"start", "stop", "set_autostart", "edit", "delete"}}}}, {"revision", prop("string", "Expected revision from list_owned_networks or the last change.")},
            {"autostart", prop("boolean", "set_autostart: start the network when the computer starts.")}, {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"nat", "hostonly", "isolated"}}, {"description", "edit: new mode (default: unchanged)."}}}, {"subnet", prop("string", "edit: IPv4 /16–/29 subnet for nat and hostonly.")}, {"dhcp", prop("boolean", "edit: hand out addresses (needs dhcp_start and dhcp_end).")}, {"dhcp_start", prop("string", "edit: first DHCP address.")}, {"dhcp_end", prop("string", "edit: last DHCP address.")}}, {"network", "action", "revision"}), false, true),
        tool("get_media", "Get media", "The OS Shop: official operating system installers (ISOs) and ready-made VM images. catalog lists them with their versions, languages and how each download is verified (checked against the publisher's checksum, or unverified when the publisher gives none). download fetches one from its publisher after the user approves in OmaWare; only sources from the catalog, never arbitrary URLs. status reports progress and the finished files (attach ISOs with manage_iso).",
            schema({{"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"catalog", "download", "status"}}}}, {"source", prop("string", "Source id from catalog, for download and status.")},
                {"version", prop("string", "download: a version from the source's versions (default: the newest).")}, {"language", prop("string", "download: a language from the source's languages (Windows only).")}}, {"action"}), false, false),
        tool("omaware_overview", "Overview", "OmaWare's VMs (state, adapters with IP addresses and where each came from, pending changes that need a restart, lab), networks (with autostart), labs and cloud images. Start here.", schema({}), true),
        tool("propose_lab", "Propose a lab", "Shows a lab plan (networks and VMs) to the user in OmaWare. Nothing is built until the user approves it there and sets the VMs' password. Returns a lab_id for lab_status, or the problems to fix in the plan.",
            schema({{"plan", plan}}, {"plan"})),
        tool("lab_status", "Lab status", "Where a proposed lab is: waiting (for the user), building (with the current step), ready (with its VMs), failed or declined. wait_seconds (up to 120) waits for the next change.",
            schema({{"lab_id", prop("string", "From propose_lab.")}, {"lab", prop("string", "Or the lab's name.")}, {"wait_seconds", prop("integer", "0–120.")}}), true),
        tool("delete_lab", "Delete a lab", "Deletes a lab's VMs (with their disks and snapshots) and networks, after the user confirms in OmaWare.",
            schema({{"lab", prop("string", "The lab's name.")}}, {"lab"}), false, true),
        tool("vm_power", "Power", "Starts, shuts down (asks the guest), force_off (pulls the plug), pauses, resumes or restarts a VM.",
            schema({{"vm", vm}, {"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"start", "shutdown", "force_off", "pause", "resume", "restart"}}}}}, {"vm", "action"})),
        tool("screenshot", "Screenshot", "The VM's screen as an image. Coordinates for vm_input are in this image's pixels.",
            schema({{"vm", vm}, {"max_width", prop("integer", "Scale wider screens down to this width (default 1280).")}}, {"vm"}), true),
        tool("vm_input", "Use the screen", "Clicks, types and presses keys on the VM's screen, in order, then returns a new screenshot (unless screenshot is false).",
            schema({{"vm", vm}, {"actions", QJsonObject{{"type", "array"}, {"items", action}, {"description", "1 to 50 actions."}}}, {"screenshot", prop("boolean", "Return a screenshot afterwards (default true).")}}, {"vm", "actions"})),
        tool("type_login", "Type the login", "Types the user name or the password of the VM's saved login where the cursor is (for example at a login prompt), then presses Enter. You never see the password.",
            schema({{"vm", vm}, {"field", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"user", "password"}}}}, {"enter", prop("boolean", "Press Enter afterwards (default true).")},
                {"via", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"screen", "serial"}}, {"description", "screen (default) or the serial console."}}}}, {"vm", "field"})),
        tool("serial_console", "Serial console", "Sends text to the VM's serial console and returns what the guest printed, as text: for routers and appliances without a guest agent (pfSense, OPNsense, VyOS) or before a VM has a network. Needs no network. pfSense must use its serial console (System > Advanced > Admin Access > Serial Terminal, or the serial installer). Fails while someone else has the console open. Not for contained VMs.",
            schema({{"vm", vm}, {"send", prop("string", "Text to type, up to 4000 characters; \\n presses Enter. Omit to only read.")}, {"wait_for", prop("string", "Return as soon as this text appears (for example a prompt). Without it, returns after the guest is quiet for a second.")},
                {"timeout_seconds", prop("integer", "1–60, default 10.")}, {"max_bytes", prop("integer", "1024–65536, default 16384: keep at most this much of the newest output.")}}, {"vm"})),
        tool("run_command", "Run a command", "Runs a shell command in a running VM and returns its exit code and output. Through the QEMU guest agent (as root) when it runs in the guest; this doesn't use the network, so it works on isolated networks too. Without the agent, only lab VMs fall back to SSH as the lab user (sudo without a password), which needs an internet or private network. Not for contained VMs.",
            schema({{"vm", vm}, {"command", prop("string", "Shell command (sh -c).")}, {"timeout_seconds", prop("integer", "1–900, default 120.")}}, {"vm", "command"})),
        tool("list_snapshots", "Snapshots", "A VM's snapshots.", schema({{"vm", vm}}, {"vm"}), true),
        tool("snapshot_vm", "Take a snapshot", "Saves a snapshot of a VM (disks, and memory if it's running) to go back to later.",
            schema({{"vm", vm}, {"name", prop("string", "1–64 letters, numbers, spaces, dots or dashes.")}, {"notes", prop("string", "Optional notes.")}, {"memory", prop("boolean", "Include memory for a running VM (default true).")}}, {"vm", "name"})),
        tool("restore_snapshot", "Restore a snapshot", "Puts a VM back to a snapshot, after the user confirms in OmaWare. Changes since the snapshot are lost.",
            schema({{"vm", vm}, {"snapshot", prop("string", "Snapshot name or id.")}}, {"vm", "snapshot"}), false, true),
        tool("set_cable", "Plug or pull a cable", "Plugs in or pulls the virtual network cable between a VM and one of its networks (\"internet\" for a VM's private internet connection).",
            schema({{"vm", vm}, {"network", prop("string", "The network's name.")}, {"plugged", prop("boolean", "true plugs in, false pulls.")}}, {"vm", "network", "plugged"})),
    };
}
// Sends one tool call to the running app and waits for its answer.
QVariantMap forwardToApp(const QString &name, const QVariantMap &args) {
    QLocalSocket socket;
    socket.connectToServer(AgentBridge::socketPath());
    if (!socket.waitForConnected(3000))
        return {{"ok", false}, {"error", "OmaWare isn't open, or AI agent access is off. Ask the user to open OmaWare and turn on Settings → AI agents."}, {"result", QVariantMap{{"code", "app_unavailable"}}}};
    socket.write(QJsonDocument(QJsonObject{{"tool", name}, {"args", QJsonObject::fromVariantMap(args)}}).toJson(QJsonDocument::Compact) + "\n");
    socket.flush();
    QByteArray answer;
    QElapsedTimer clock; clock.start();
    while (!answer.contains('\n') && clock.elapsed() < 1000LL * 1000) {
        if (socket.state() != QLocalSocket::ConnectedState && socket.bytesAvailable() == 0) break;
        socket.waitForReadyRead(1000);
        answer += socket.readAll();
    }
    if (!answer.contains('\n')) return {{"ok", false}, {"error", "OmaWare closed the connection without answering."}};
    return QJsonDocument::fromJson(answer.left(answer.indexOf('\n'))).toVariant().toMap();
}
}

QVariantList Mcp::tools() { auto list = toolList().toVariantList(); list.append(AgentProvision::tools()); return list; }

bool Mcp::validateManagementArguments(const QString &name, const QVariantMap &args, QString &error) {
    if (AgentProvision::handles(name)) return AgentProvision::validate(name, args, error);
    if (!QStringList{"vm_details", "diagnose_vm", "wait_for_vm", "transfer_file", "update_vm_resources", "clone_vm", "manage_iso", "manage_network_adapter", "manage_network", "serial_console", "get_media"}.contains(name)) return true;
    QJsonObject input;
    for (const auto &t : toolList()) if (t.toObject()["name"] == name) input = t.toObject()["inputSchema"].toObject();
    const auto properties = input["properties"].toObject();
    for (const auto &v : input["required"].toArray()) if (!args.contains(v.toString())) { error = "Missing required argument: " + v.toString(); return false; }
    const auto json = QJsonObject::fromVariantMap(args);
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (!properties.contains(it.key())) { error = "Unknown argument: " + it.key(); return false; }
        const auto p = properties[it.key()].toObject(); const auto type = p["type"].toString(); const auto v = it.value();
        bool valid = (type == "string" && v.isString()) || (type == "boolean" && v.isBool()) || (type == "integer" && v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble());
        if (valid && p.contains("enum")) valid = p["enum"].toArray().contains(v);
        if (!valid) { error = "Invalid type or value for argument: " + it.key(); return false; }
    }
    return true;
}


QByteArray Mcp::respond(const QByteArray &message, const std::function<QVariantMap(const QString &, const QVariantMap &)> &forward) {
    const auto request = QJsonDocument::fromJson(message).object();
    if (!request.contains("id")) return {};   // notifications need no answer
    const QJsonValue id = request["id"];
    const QJsonObject params = request["params"].toObject();
    const auto method = request["method"].toString();
    auto answer = [&](const QJsonObject &result) { return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).toJson(QJsonDocument::Compact); };
    auto error = [&](int code, const QString &text) { return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", text}}}}).toJson(QJsonDocument::Compact); };
    if (method == "initialize") {
        static const QStringList known{"2025-06-18", "2025-03-26", "2024-11-05"};
        const auto asked = params["protocolVersion"].toString();
        return answer({{"protocolVersion", known.contains(asked) ? asked : known.first()}, {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
            {"serverInfo", QJsonObject{{"name", "omaware"}, {"title", "OmaWare"}, {"version", QCoreApplication::applicationVersion()}}}, {"instructions", instructions}});
    }
    if (method == "ping") return answer({});
    if (method == "tools/list") return answer({{"tools", QJsonArray::fromVariantList(tools())}});
    if (method == "tools/call") {
        const auto name = params["name"].toString();
        bool known = false;
        for (const auto &t : tools()) known |= t.toMap()["name"].toString() == name;
        if (!known) return error(-32602, "Unknown tool: " + name);
        if (AgentProvision::handles(name)) {
            QString why;
            if (!params["arguments"].isObject() || !AgentProvision::validate(name, params["arguments"].toObject().toVariantMap(), why))
                return error(-32602, why.isEmpty() ? "Arguments must be an object." : why);
        }
        const auto reply = forward(name, params["arguments"].toObject().toVariantMap());
        QJsonArray content;
        const bool ok = reply.value("ok").toBool();
        const auto result = QJsonObject::fromVariantMap(reply.value("result").toMap());
        QString text = ok ? QString{} : reply.value("error", "The tool failed.").toString();
        if (!result.isEmpty()) text += (text.isEmpty() ? "" : "\n") + QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
        content.append(QJsonObject{{"type", "text"}, {"text", text}});
        if (reply.contains("image")) content.append(QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", reply["image"].toString()}});
        auto structured = result;
        if (!ok) { structured["error"] = reply.value("error", "The tool failed.").toString(); if (!structured.contains("code")) structured["code"] = "operation_failed"; }
        return answer({{"content", content}, {"isError", !ok}, {"structuredContent", structured}});
    }
    return error(-32601, "Method not found: " + method);
}

int Mcp::run() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const auto out = respond(QByteArray::fromStdString(line), forwardToApp);
        if (!out.isEmpty()) { std::cout << out.toStdString() << '\n'; std::cout.flush(); }
    }
    return 0;
}
