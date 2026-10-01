// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcpserver.h"
#include "agentbridge.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <cstdio>
#include <iostream>
#include <string>

namespace {
const char *instructions = R"(OmaWare runs virtual machines (QEMU/KVM) on the user's computer. With these tools you can see its VMs and networks, build labs (networks plus VMs set up from cloud images), use a VM's screen (screenshot, click, type), run commands inside VMs, take and restore snapshots, and plug or pull virtual network cables.

How to work with it:
- Start with omaware_overview.
- To build something, write a lab plan and call propose_lab. Ask the user what VM user name they want first. Never ask the user for a password and never put one in a plan: OmaWare asks for the password in its own window when the user approves the plan, and type_login types it for you when a login screen needs it.
- After propose_lab, call lab_status with wait_seconds until the lab is ready, failed or declined. Building downloads images (once), creates networks (the user may be asked for their computer password once), creates the VMs and starts them.
- VMs on "internet" or "private" networks can run commands with run_command (over SSH as the lab user, with passwordless sudo, or through the QEMU guest agent). VMs only on "isolated" networks can only be used through their screen.
- First boot setup takes one to three minutes; run_command "cloud-init status --wait" waits for it.
- For screens: take a screenshot, then send vm_input actions using that screenshot's pixel coordinates. vm_input returns a new screenshot.
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
        tool("omaware_overview", "Overview", "OmaWare's VMs (state, adapters, IP addresses, lab), networks, labs and cloud images. Start here.", schema({}), true),
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
            schema({{"vm", vm}, {"field", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"user", "password"}}}}, {"enter", prop("boolean", "Press Enter afterwards (default true).")}}, {"vm", "field"})),
        tool("run_command", "Run a command", "Runs a shell command in a running VM and returns its exit code and output: through the QEMU guest agent (as root) when it runs, otherwise over SSH as the lab user, who can use sudo without a password. Not for VMs only on isolated networks.",
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
        return {{"ok", false}, {"error", "OmaWare isn't open, or AI agent access is off. Ask the user to open OmaWare and turn on Settings → AI agents."}};
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

QVariantList Mcp::tools() { return toolList().toVariantList(); }

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
    if (method == "tools/list") return answer({{"tools", toolList()}});
    if (method == "tools/call") {
        const auto name = params["name"].toString();
        bool known = false;
        for (const auto &t : toolList()) known |= t.toObject()["name"].toString() == name;
        if (!known) return error(-32602, "Unknown tool: " + name);
        const auto reply = forward(name, params["arguments"].toObject().toVariantMap());
        QJsonArray content;
        const bool ok = reply.value("ok").toBool();
        const auto result = QJsonObject::fromVariantMap(reply.value("result").toMap());
        QString text = ok ? QString{} : reply.value("error", "The tool failed.").toString();
        if (!result.isEmpty()) text += (text.isEmpty() ? "" : "\n") + QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
        content.append(QJsonObject{{"type", "text"}, {"text", text}});
        if (reply.contains("image")) content.append(QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", reply["image"].toString()}});
        return answer({{"content", content}, {"isError", !ok}});
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
