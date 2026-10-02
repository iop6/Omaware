// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentprovision.h"
#include "paths.h"
#include "configuration.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QJsonObject field(QString type, QString description) { return {{"type", type}, {"description", description}}; }
QJsonObject choice(QStringList values) { return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}}; }
QJsonObject number(int low, int high, QString description) { auto p = field("integer", description); p["minimum"] = low; p["maximum"] = high; return p; }
QVariantMap definition(QString name, QJsonObject properties, QStringList required, bool readOnly, QString description) {
    return QJsonObject{{"name", name}, {"title", name}, {"description", description},
        {"inputSchema", QJsonObject{{"type", "object"}, {"additionalProperties", false}, {"properties", properties}, {"required", QJsonArray::fromStringList(required)}}},
        {"annotations", QJsonObject{{"readOnlyHint", readOnly}, {"destructiveHint", !readOnly}, {"openWorldHint", false}}}}.toVariantMap();
}
bool uuid(const QString &s) { return QRegularExpression("^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$").match(s).hasMatch() && !QUuid(s).isNull(); }
bool name(const QString &s) { return QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]{0,47}$").match(s).hasMatch(); }
bool mediaName(QString kind, QString s) {
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_. -]{0,159}$").match(s).hasMatch()) return false;
    return kind == "iso" ? s.endsWith(".iso", Qt::CaseInsensitive) : kind == "disk" && (s.endsWith(".qcow2", Qt::CaseInsensitive) || s.endsWith(".raw", Qt::CaseInsensitive) || s.endsWith(".ova", Qt::CaseInsensitive));
}
}

QVariantList AgentProvision::tools() {
    const auto request = field("string", "Client-generated lowercase UUID. Reuse the same ID and arguments after a lost response; never resubmit with a new ID. Deduplication lasts for this running app only.");
    const auto dry = field("boolean", "Read-only preparation; no approval, storage writes or worker mutation. Host capacity, disk validity and subnet conflicts are checked at execution.");
    return {
        definition("list_installation_media", {}, {}, true, "List confined, local ISO and qcow2/raw/OVA appliance filenames. OVA execution supports one packaged standalone sparse VMDK only. No downloads or arbitrary paths."),
        definition("list_owned_networks", {}, {}, true, "Owned network UUIDs, revisions, modes and availability for provisioning; no non-owned destinations."),
        definition("provision_status", {{"request_id", request}}, {"request_id"}, true, "Immediate operation status. Poll until succeeded, failed or declined. Failed operations may have partial effects; inspect inventory before any new request. No cancellation of running workers."),
        definition("create_vm", {{"request_id", request}, {"name", field("string", "1–48 letters, digits, dots, underscores or dashes.")}, {"media_kind", choice({"iso", "disk"})}, {"media", field("string", "Exact library filename, not a path.")}, {"cpus", number(1,256,"Default 2; within host capacity.")}, {"memory_mib", number(256,1048576,"Default 4096; within host capacity.")}, {"disk_gib", number(1,2048,"Default 32 for ISO; disk imports retain their source capacity.")}, {"preset", field("string", "Supported libosinfo id, default generic; use win11 for Windows, freebsd for pfSense when supported by host.")}, {"firmware", choice({"bios", "uefi"})}, {"tpm", field("boolean", "Default false. Windows 11 typically needs true and UEFI.")}, {"networks", QJsonObject{{"type","array"},{"items",field("string","Owned available network UUID.")},{"maxItems",4},{"uniqueItems",true}}}, {"dry_run", dry}}, {"request_id","name","media_kind","media"}, false, "Approve in OmaWare; existing vm.create backend creates a STOPPED VM with copied independent disk and fresh MACs. Zero to four owned network UUIDs, default disconnected. Imports copy guest identities/credentials. Does not install Windows/FLARE or configure pfSense; use guest console after explicit start."),
        definition("create_network", {{"request_id",request},{"name",field("string","1–48 letters, digits, underscores or dashes.")},{"mode",choice({"nat","hostonly","isolated"})},{"subnet",field("string","Explicit IPv4 /16–/29 subnet for nat/hostonly.")},{"dhcp",field("boolean","Default false; provide range when true.")},{"dhcp_start",field("string","IPv4 DHCP start.")},{"dhcp_end",field("string","IPv4 DHCP end.")},{"dry_run",dry}}, {"request_id","name","mode"}, false, "Approval-gated networks.save; creates a started, non-autostart owned network. NAT exposes host and internet, hostonly exposes host, isolated is guest-only. Authorization is a separate operation; may need system polkit authorization."),
        definition("authorize_network", {{"request_id",request},{"network",field("string","Owned active network UUID.")},{"revision",field("string","Expected revision from list_owned_networks.")},{"dry_run",dry}}, {"request_id","network","revision"}, false, "Approval-gated networks.authorize using installed trusted helper; separate administrator authorization may be required. No passwords or helper installation through MCP.")
    };
}
bool AgentProvision::handles(const QString &tool) { for (const auto &v : tools()) if (v.toMap()["name"] == tool) return true; return false; }
QString AgentProvision::admission(const QString &id, const QVariantMap &request, const QHash<QString,QVariantMap> &requests, const QHash<QString,QVariantMap> &states) {
    if (requests.contains(id)) return requests[id] == request ? "existing" : "request_conflict";
    if (states.size() >= 128) return "session_limit";
    for (const auto &s : states) if (QStringList{"preparing","awaiting_approval","running"}.contains(s["state"].toString())) return "busy";
    return "new";
}
bool AgentProvision::validate(const QString &tool, const QVariantMap &args, QString &error) {
    QVariantMap spec;
    for (const auto &v : tools()) if (v.toMap()["name"] == tool) spec = v.toMap()["inputSchema"].toMap();
    auto fail = [&] { error = "Invalid provisioning arguments (types, bounds, filenames, IDs or mode-specific fields)."; return false; };
    if (spec.isEmpty()) return fail();
    for (const auto &v : spec["required"].toList()) if (!args.contains(v.toString())) return fail();
    const auto props = spec["properties"].toMap();
    const auto json = QJsonObject::fromVariantMap(args);
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (!props.contains(it.key())) return fail();
        const auto p = QJsonObject::fromVariantMap(props[it.key()].toMap()); const auto type = p["type"].toString(); const auto v = it.value();
        bool ok = type == "string" ? v.isString() && v.toString().size() <= 160 : type == "boolean" ? v.isBool() : type == "array" ? v.isArray() : v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble() && v.toDouble() >= p["minimum"].toDouble() && v.toDouble() <= p["maximum"].toDouble();
        if (!ok || (p.contains("enum") && !p["enum"].toArray().contains(v))) return fail();
    }
    if (args.contains("request_id") && !uuid(args["request_id"].toString())) return fail();
    if (args.contains("name") && (!name(args["name"].toString()) || (tool == "create_network" && args["name"].toString().contains('.')))) return fail();
    if (tool == "create_vm") {
        if (!mediaName(args["media_kind"].toString(),args["media"].toString())) return fail();
        if (args["media_kind"] == "disk" && args.contains("disk_gib")) return fail();
        if (args.contains("preset") && !QRegularExpression("^[a-z][a-z0-9.+-]{1,40}$").match(args["preset"].toString()).hasMatch()) return fail();
        auto nets = args["networks"].toList(); QStringList seen;
        if (nets.size() > 4) return fail();
        for (const auto &v : nets) { if (v.typeId() != QMetaType::QString || !uuid(v.toString()) || seen.contains(v.toString())) return fail(); seen << v.toString(); }
    }
    if (tool == "create_network") {
        if (args["mode"] == "isolated") { for (auto key : {"subnet","dhcp","dhcp_start","dhcp_end"}) if (args.contains(key)) return fail(); }
        else if (!args.contains("subnet")) return fail();
        if (!args.value("dhcp",false).toBool() && (args.contains("dhcp_start") || args.contains("dhcp_end"))) return fail();
        if (args.value("dhcp",false).toBool() && (!args.contains("dhcp_start") || !args.contains("dhcp_end"))) return fail();
        QVariantMap in; QString why;
        for (auto key : {"name","mode","subnet","dhcp"}) in[key] = args.value(key, key == QString("dhcp") ? QVariant(false) : QVariant(QString{}));
        in["dhcpStart"] = args["dhcp_start"]; in["dhcpEnd"] = args["dhcp_end"];
        Configuration::networkXml(in, args["request_id"].toString(), "oma12345678", why);
        if (!why.isEmpty()) return fail();
    }
    if (tool == "authorize_network" && (!uuid(args["network"].toString()) || !QRegularExpression("^[0-9a-f]{64}$").match(args["revision"].toString()).hasMatch())) return fail();
    return true;
}
QString AgentProvision::mediaPath(const QString &kind, const QString &name) { return (kind == "iso" ? Paths::isos() : Paths::root() + "/appliances") + "/" + name; }
int AgentProvision::openMedia(const QString &kind, const QString &name, QVariantMap &identity, QString &error) {
    identity.clear();
    if (!mediaName(kind,name)) { error = "Select a plain local media filename."; return -1; }
    const auto components = mediaPath(kind,name).split('/',Qt::SkipEmptyParts);
    if (!QDir::isAbsolutePath(mediaPath(kind,name)) || components.contains("..") || components.contains(".")) { error = "Unsafe library root."; return -1; }
    int fd = ::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    for (qsizetype i = 0; i < components.size() && fd >= 0; ++i) {
        const bool leaf = i == components.size()-1;
        int next = ::openat(fd,QFile::encodeName(components[i]).constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|(leaf ? O_NONBLOCK : O_DIRECTORY));
        ::close(fd); fd = next;
        struct stat s{};
        if (fd < 0 || ::fstat(fd,&s) != 0 || (leaf ? (!S_ISREG(s.st_mode) || s.st_uid != ::getuid() || s.st_nlink != 1 || s.st_size <= 0) : (!S_ISDIR(s.st_mode) || (s.st_uid != 0 && s.st_uid != ::getuid()))) || (s.st_mode & (S_IWGRP|S_IWOTH))) {
            if (fd >= 0) ::close(fd);
            error = "Media library must contain user-owned regular, non-empty, single-link files; no symlinks or writable shared components."; return -1;
        }
        if (leaf) identity = {{"device",qulonglong(s.st_dev)},{"inode",qulonglong(s.st_ino)},{"bytes",qlonglong(s.st_size)},{"mtime_s",qlonglong(s.st_mtim.tv_sec)},{"mtime_ns",qlonglong(s.st_mtim.tv_nsec)},{"ctime_s",qlonglong(s.st_ctim.tv_sec)},{"ctime_ns",qlonglong(s.st_ctim.tv_nsec)}};
    }
    if (fd < 0) error = "Cannot open media library.";
    return fd;
}
bool AgentProvision::storageRoot(QString &error, bool create) {
    const auto path = Paths::vms();
    if (!QDir::isAbsolutePath(path)) { error = "Storage root must be absolute."; return false; }
    const auto components = path.split('/', Qt::SkipEmptyParts);
    if (components.contains("..") || components.contains(".")) { error = "Unsafe storage root."; return false; }
    int fd = ::open("/", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    for (qsizetype i = 0; i < components.size() && fd >= 0; ++i) {
        const auto part = QFile::encodeName(components[i]);
        int next = ::openat(fd, part.constData(), O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (next < 0 && errno == ENOENT && i == components.size()-1) {
            if (!create) { ::close(fd); return true; }
            if (::mkdirat(fd, part.constData(), 0700) == 0)
                next = ::openat(fd, part.constData(), O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        }
        ::close(fd); fd = next;
        struct stat s{};
        if (fd < 0 || ::fstat(fd,&s) != 0 || !S_ISDIR(s.st_mode) || (s.st_uid != 0 && s.st_uid != ::getuid()) || (s.st_mode & (S_IWGRP|S_IWOTH)) || (i == components.size()-1 && s.st_uid != ::getuid())) {
            if (fd >= 0) ::close(fd);
            error = "Default VM storage must be owned, unshared and free of symlink components."; return false;
        }
    }
    if (fd < 0) { error = "Cannot open default VM storage."; return false; }
    ::close(fd); return true;
}
QVariantList AgentProvision::media() {
    QVariantList out;
    for (const auto &kind : {QString("iso"),QString("disk")}) {
        QDir dir(kind == "iso" ? Paths::isos() : Paths::root() + "/appliances");
        for (const auto &name : dir.entryList(QDir::Files|QDir::NoSymLinks,QDir::Name)) {
            QVariantMap identity; QString error; int fd = openMedia(kind,name,identity,error);
            if (fd < 0) continue;
            ::close(fd); out.append(QVariantMap{{"kind",kind},{"name",name},{"bytes",identity["bytes"]}});
            if (out.size() >= 256) return out;
        }
    }
    return out;
}
bool AgentProvision::prepare(const QString &tool, const QVariantMap &args, const QVariantList &networks, QVariantMap &input, QString &error) {
    input.clear(); if (!validate(tool,args,error)) return false;
    auto owned = [&](QString id) { for (const auto &v : networks) if (v.toMap()["uuid"] == id && v.toMap()["managed"].toBool()) return v.toMap(); return QVariantMap{}; };
    if (tool == "create_vm") {
        if (!storageRoot(error)) return false;
        QVariantMap identity; int fd = openMedia(args["media_kind"].toString(),args["media"].toString(),identity,error);
        if (fd < 0) return false;
        ::close(fd);
        input = {{"name",args["name"]},{"sourceMode",args["media_kind"]},{"source",mediaPath(args["media_kind"].toString(),args["media"].toString())},{"mediaIdentity",identity},{"cpus",args.value("cpus",2)},{"memoryMiB",args.value("memory_mib",4096)},{"diskGiB",args.value("disk_gib",32)},{"preset",args.value("preset","generic")},{"firmware",args.value("firmware","bios")},{"tpm",args.value("tpm",false)},{"networkId","none"}};
        QVariantList adapters, revisions;
        for (const auto &v : args["networks"].toList()) {
            auto n = owned(v.toString());
            if (n.isEmpty() || !n["active"].toBool() || !n["available"].toBool()) { error = "Select an owned, active, authorized network from list_owned_networks."; return false; }
            const auto mac = QCryptographicHash::hash((args["request_id"].toString()+QString::number(adapters.size())).toUtf8(),QCryptographicHash::Sha256).toHex().left(6);
            adapters.append(QVariantMap{{"id","bridge:"+n["bridge"].toString()},{"mac",QString("52:54:00:%1:%2:%3").arg(QString(mac.mid(0,2)),QString(mac.mid(2,2)),QString(mac.mid(4,2)))}});
            revisions.append(QVariantMap{{"uuid",n["uuid"]},{"revision",n["revision"]}});
        }
        input["networks"] = adapters; input["networkRevisions"] = revisions;
    } else if (tool == "create_network") {
        const auto full = "omaware-"+args["name"].toString().toLower();
        for (const auto &v : networks) if (v.toMap()["name"] == full) { error = "Network name already exists."; return false; }
        input = {{"name",args["name"]},{"mode",args["mode"]},{"subnet",args.value("subnet",QString{})},{"dhcp",args.value("dhcp",false)},{"dhcpStart",args.value("dhcp_start",QString{})},{"dhcpEnd",args.value("dhcp_end",QString{})},{"start",true},{"autostart",false},{"authorize",false}};
    } else if (tool == "authorize_network") {
        auto n = owned(args["network"].toString());
        if (n.isEmpty() || !n["active"].toBool() || n["revision"] != args["revision"]) { error = "Owned active network required; revision changed or network unavailable."; return false; }
        input = {{"uuid",args["network"]},{"revision",args["revision"]}};
    } else { error = "Not a provisioning mutation."; return false; }
    input["provisionTool"] = tool; input["provisionArgs"] = args;
    return true;
}

bool AgentProvision::verifyEnvelope(const QString &op, const QVariantMap &input, const QVariantList &networks, QString &error) {
    const auto tool = input["provisionTool"].toString();
    const QString expected = tool == "create_vm" ? "vm.create" : tool == "create_network" ? "networks.save" : tool == "authorize_network" ? "networks.authorize" : "";
    const auto args = input["provisionArgs"].toMap();
    if (expected.isEmpty() || op != expected || input["agentRequest"].typeId() != QMetaType::Bool || !input["agentRequest"].toBool() || !validate(tool,args,error) || args.value("dry_run",false).toBool()) { error = "Invalid provisioning envelope or dry-run mutation."; return false; }
    auto received = input; received.remove("agentRequest"); received.remove("requestTag");
    if (received["provisionEpoch"].typeId() != QMetaType::ULongLong || !(received["provisionEpoch"].toULongLong() & 1)) { error = "Missing provisioning access epoch."; return false; }
    received.remove("provisionEpoch");
    QVariantMap prepared;
    if (!prepare(tool,args,networks,prepared,error) || prepared != received) { error = "Approved provisioning media, arguments or network identity changed."; return false; }
    return true;
}
