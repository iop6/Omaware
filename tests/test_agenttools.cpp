// SPDX-License-Identifier: GPL-3.0-or-later
#include "agenttransfer.h"
#include "agentprovision.h"
#include "agentbridge.h"
#include "backend.h"
#include "bridgehelper.h"
#include "mcpserver.h"
#include "paths.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <unistd.h>
#include <sys/stat.h>

// No Backend, libvirt connection, app socket or VM is created by these tests.
class AgentToolsTest : public QObject {
    Q_OBJECT
private slots:
    void paths() {
        for (const auto &name : {"../a", "/tmp/a", ".env", "id_rsa", "secret.txt", "passwords.txt", "a/b", "cert.pem", ""}) QVERIFY(!AgentTransfer::validName(name));
        QVERIFY(AgentTransfer::validName("report-1.txt"));
        for (const auto &path : {"relative", "/tmp/../etc/shadow", "/root/.ssh/config", "/etc/shadow", "/proc/self/environ", "/tmp//a", "/"}) QVERIFY(!AgentTransfer::validGuestPath(path));
        QVERIFY(AgentTransfer::validGuestPath("/tmp/report.txt"));
    }
    void hostTransfer() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto old = qgetenv("XDG_DATA_HOME"); qputenv("XDG_DATA_HOME", dir.path().toUtf8());
        const auto restore = qScopeGuard([&] { if (old.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", old); });
        QString error; QByteArray bytes;
        QVERIFY(AgentTransfer::write("report.bin", QByteArray("a\0b", 3), false, error));
        QVERIFY(AgentTransfer::read("report.bin", bytes, error)); QCOMPARE(bytes, QByteArray("a\0b", 3));
        QVERIFY(!AgentTransfer::write("report.bin", "new", false, error));
        QVERIFY(AgentTransfer::write("report.bin", "new", true, error));
        QVERIFY(AgentTransfer::read("report.bin", bytes, error)); QCOMPARE(bytes, QByteArray("new"));
        QVERIFY(!AgentTransfer::write("large.bin", QByteArray(AgentTransfer::limit + 1, 'a'), false, error));
        const auto folder = Paths::root() + "/transfers/";
        QVERIFY(QFile::link(folder + "report.bin", folder + "symlink.bin"));
        QVERIFY(!AgentTransfer::read("symlink.bin", bytes, error));
        QVERIFY(!AgentTransfer::write("symlink.bin", "bad", true, error));
        QCOMPARE(::link(QFile::encodeName(folder + "report.bin").constData(), QFile::encodeName(folder + "hardlink.bin").constData()), 0);
        QVERIFY(!AgentTransfer::read("hardlink.bin", bytes, error));
        QVERIFY(!AgentTransfer::write("hardlink.bin", "bad", true, error));
        QVERIFY(!AgentTransfer::read("../report.bin", bytes, error));
    }
    void guestTransferScript() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("spaces and 'quotes';.bin");
        auto run = [](const QString &command, QByteArray *out = nullptr) {
            QProcess p; p.start("/bin/sh", {"-c", command});
            if (!p.waitForStarted() || !p.waitForFinished(5000)) return -1;
            if (out) *out = p.readAllStandardOutput();
            return p.exitCode();
        };
        const QByteArray bytes(AgentTransfer::limit, '\xff');
        QCOMPARE(run(AgentTransfer::command("upload", path, bytes, false)), 0);
        QVERIFY(run(AgentTransfer::command("upload", path, "bad", false)) != 0);
        QByteArray out;
        QCOMPARE(run(AgentTransfer::command("download", path, {}, false), &out), 0);
        QCOMPARE(QByteArray::fromBase64(out), bytes);
        QCOMPARE(run(AgentTransfer::command("upload", path, "new", true)), 0);
        QVERIFY(QFile::link(path, dir.filePath("link")));
        QVERIFY(run(AgentTransfer::command("download", dir.filePath("link"), {}, false)) != 0);
        QVERIFY(run(AgentTransfer::command("upload", dir.filePath("link"), "bad", true)) != 0);
        QVERIFY(QFile::link(dir.path(), dir.filePath("parent-link")));
        QVERIFY(run(AgentTransfer::command("download", dir.filePath("parent-link/") + QFileInfo(path).fileName(), {}, false)) != 0);
        QFile large(dir.filePath("large")); QVERIFY(large.open(QIODevice::WriteOnly)); large.write(QByteArray(AgentTransfer::limit + 1, 'a')); large.close();
        QVERIFY(run(AgentTransfer::command("download", large.fileName(), {}, false)) != 0);
    }
    void validation() {
        QString error;
        QVERIFY(Mcp::validateManagementArguments("vm_details", {{"vm", "test"}}, error));
        QVERIFY(!Mcp::validateManagementArguments("vm_details", {}, error));
        QVERIFY(!Mcp::validateManagementArguments("manage_iso", {{"vm", "test"}, {"action", "download"}}, error));
        QVERIFY(!Mcp::validateManagementArguments("clone_vm", {{"vm", "test"}, {"snapshot", "id"}, {"name", "copy"}, {"mode", "linked"}}, error));
        QVERIFY(!Mcp::validateManagementArguments("update_vm_resources", {{"vm", "test"}, {"cpus", 1.5}}, error));
        QVERIFY(!Mcp::validateManagementArguments("update_vm_resources", {{"vm", "test"}, {"cpus", "2"}}, error));
        QVERIFY(!Mcp::validateManagementArguments("manage_network_adapter", {{"vm", "test"}, {"action", "add"}, {"dry_run", true}}, error));
        QVERIFY(!Mcp::validateManagementArguments("transfer_file", {{"vm", "test"}, {"direction", "upload"}, {"file", "a"}, {"guest_path", "/tmp/a"}, {"overwrite", "false"}}, error));
        QVERIFY(Mcp::validateManagementArguments("wait_for_vm", {{"vm", "test"}, {"condition", "running"}, {"timeout_seconds", 0}}, error));
    }
    void provisioningValidation() {
        QString error;
        const QVariantMap vm{{"request_id","10000000-0000-4000-8000-000000000001"},{"name","remnux"},{"media_kind","disk"},{"media","remnux.qcow2"}};
        QVERIFY(Mcp::validateManagementArguments("create_vm",vm,error));
        for (const auto &change : QVariantList{QVariantMap{{"cpus","2"}},QVariantMap{{"cpus",0}},QVariantMap{{"cpus",257}},QVariantMap{{"cpus",true}},QVariantMap{{"memory_mib",255}},QVariantMap{{"dry_run","false"}},QVariantMap{{"source","/etc/passwd"}},QVariantMap{{"location","/tmp"}},QVariantMap{{"start",true}},QVariantMap{{"media","../remnux.qcow2"}},QVariantMap{{"media","id_rsa"}},QVariantMap{{"media_kind","cloud"}},QVariantMap{{"disk_gib",32}},QVariantMap{{"networks",QVariantList{"User"}}},QVariantMap{{"networks",QVariantList{"user","user"}}},QVariantMap{{"networks",QVariantList{"default"}}},QVariantMap{{"networks",QVariantList{1}}},QVariantMap{{"preset","--connect"}}}) {
            auto args = vm; const auto map = change.toMap(); for (auto it=map.begin();it!=map.end();++it) args[it.key()] = it.value();
            QVERIFY2(!Mcp::validateManagementArguments("create_vm",args,error),qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(args).toJson())));
        }
        // "user" (the VM's private internet connection) may be one of the ordered adapters.
        for (const auto &nets : {QVariantList{"user"}, QVariantList{"user","20000000-0000-4000-8000-000000000001"}, QVariantList{"20000000-0000-4000-8000-000000000001","user"}}) {
            auto args = vm; args["networks"] = nets; QVERIFY2(Mcp::validateManagementArguments("create_vm",args,error),qPrintable(error));
        }
        const QVariantMap net{{"request_id",vm["request_id"]},{"name","lab-lan"},{"mode","isolated"}};
        QVERIFY(AgentProvision::validate("create_network",net,error));
        auto args=net; args["autostart"]=false; QVERIFY(AgentProvision::validate("create_network",args,error));
        args["autostart"]="yes"; QVERIFY(!AgentProvision::validate("create_network",args,error));
        args=net; args["dhcp"]=false; QVERIFY(!AgentProvision::validate("create_network",args,error));
        args=net; args["mode"]="nat"; QVERIFY(!AgentProvision::validate("create_network",args,error));
        args["subnet"]="192.168.90.0/24"; QVERIFY(AgentProvision::validate("create_network",args,error));
        args["dhcp"]=true; QVERIFY(!AgentProvision::validate("create_network",args,error));
        args["dhcp_start"]="192.168.90.100"; args["dhcp_end"]="192.168.90.200"; QVERIFY(AgentProvision::validate("create_network",args,error));
        args["dhcp_end"]="192.168.91.200"; QVERIFY(!AgentProvision::validate("create_network",args,error));
        QVERIFY(!AgentProvision::validate("list_installation_media",{{"path","/tmp"}},error));
        QVERIFY(!AgentProvision::validate("authorize_network",{{"request_id",vm["request_id"]},{"network","default"},{"revision",QString(64,'a')}},error));
    }
    void provisioningMediaAndEnvelope() {
        // A private fixture under HOME avoids shared /tmp ancestors. No real media is read.
        QTemporaryDir dir(QDir::homePath()+"/.omaware-provision-test-XXXXXX"); QVERIFY(dir.isValid());
        const auto old=qgetenv("XDG_DATA_HOME"); qputenv("XDG_DATA_HOME",dir.path().toUtf8());
        const auto restore=qScopeGuard([&] { if(old.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME",old); });
        // Media/storage fixtures must have private permissions regardless of the
        // builder's inherited umask (Ubuntu commonly uses a group-writable 0002).
        const auto oldMask = ::umask(0077);
        const auto restoreMask = qScopeGuard([&] { ::umask(oldMask); });
        QVERIFY(QDir().mkpath(Paths::isos())); QVERIFY(QDir().mkpath(Paths::root()+"/appliances"));
        auto make=[](QString path) { QFile f(path); if(!f.open(QIODevice::WriteOnly)) return false; return f.write("fixture") == 7; };
        QVERIFY(make(Paths::isos()+"/Windows.iso")); QVERIFY(make(Paths::root()+"/appliances/remnux.qcow2"));
        QString error; QVariantMap identity;
        int fd=AgentProvision::openMedia("disk","remnux.qcow2",identity,error); QVERIFY2(fd>=0,qPrintable(error)); ::close(fd);
        QCOMPARE(AgentProvision::media().size(),2);
        QVERIFY(QFile::link(Paths::isos()+"/Windows.iso",Paths::isos()+"/linked.iso"));
        QVERIFY(AgentProvision::openMedia("iso","linked.iso",identity,error)<0);
        QCOMPARE(::link(QFile::encodeName(Paths::isos()+"/Windows.iso").constData(),QFile::encodeName(Paths::isos()+"/hard.iso").constData()),0);
        QVERIFY(AgentProvision::openMedia("iso","hard.iso",identity,error)<0);
        QVERIFY(AgentProvision::openMedia("iso","Windows.iso",identity,error)<0);
        QVERIFY(QFile::remove(Paths::isos()+"/hard.iso"));
        QFile empty(Paths::isos()+"/empty.iso"); QVERIFY(empty.open(QIODevice::WriteOnly)); empty.close();
        QVERIFY(AgentProvision::openMedia("iso","empty.iso",identity,error)<0);
        QCOMPARE(::mkfifo(QFile::encodeName(Paths::isos()+"/pipe.iso").constData(),0600),0);
        QVERIFY(AgentProvision::openMedia("iso","pipe.iso",identity,error)<0);
        const QString netId="20000000-0000-4000-8000-000000000001";
        QVariantMap n{{"uuid",netId},{"managed",true},{"active",true},{"available",true},{"bridge","oma12345678"},{"revision",QString(64,'a')},{"name","omaware-lan"}};
        QVariantMap args{{"request_id","10000000-0000-4000-8000-000000000001"},{"name","remnux"},{"media_kind","disk"},{"media","remnux.qcow2"},{"networks",QVariantList{netId}}};
        QVariantMap input; QVERIFY2(AgentProvision::prepare("create_vm",args,{n},input,error),qPrintable(error));
        QVERIFY(!QDir(Paths::vms()).exists()); // Preparation never creates even the default storage root.
        QCOMPARE(input["sourceMode"].toString(),QString("disk")); QCOMPARE(input["networkId"].toString(),QString("none"));
        QCOMPARE(input["networks"].toList().size(),1);
        QVERIFY(!input.contains("location")); QVERIFY(!input.contains("start"));
        {
            // Adapters keep the caller's order (the guest's first NIC is the first entry), with "user" as
            // the private internet connection; every adapter gets its own MAC.
            auto ordered=args; ordered["networks"]=QVariantList{"user",netId};
            QVariantMap in; QVERIFY2(AgentProvision::prepare("create_vm",ordered,{n},in,error),qPrintable(error));
            const auto adapters=in["networks"].toList(); QCOMPARE(adapters.size(),2);
            QCOMPARE(adapters[0].toMap()["id"].toString(),QString("user"));
            QCOMPARE(adapters[1].toMap()["id"].toString(),QString("bridge:oma12345678"));
            QVERIFY(adapters[0].toMap()["mac"] != adapters[1].toMap()["mac"]);
            QCOMPARE(in["networkRevisions"].toList().size(),1);
            ordered["networks"]=QVariantList{netId,"user"};
            QVERIFY(AgentProvision::prepare("create_vm",ordered,{n},in,error));
            QCOMPARE(in["networks"].toList()[0].toMap()["id"].toString(),QString("bridge:oma12345678"));
            QCOMPARE(in["networks"].toList()[1].toMap()["id"].toString(),QString("user"));
            in["agentRequest"]=true; in["provisionEpoch"]=qulonglong(1);
            QVERIFY2(AgentProvision::verifyEnvelope("vm.create",in,{n},error),qPrintable(error));
            // Containment of ownership still applies to the other entries.
            auto unowned=n; unowned["managed"]=false;
            QVERIFY(!AgentProvision::prepare("create_vm",ordered,{unowned},in,error));
        }
        input["agentRequest"]=true; input["provisionEpoch"]=qulonglong(1);
        QVERIFY(AgentProvision::verifyEnvelope("vm.create",input,{n},error));
        auto tampered=input; tampered["source"]="/etc/passwd"; QVERIFY(!AgentProvision::verifyEnvelope("vm.create",tampered,{n},error));
        tampered=input; tampered["location"]=dir.path(); QVERIFY(!AgentProvision::verifyEnvelope("vm.create",tampered,{n},error));
        tampered=input; tampered["agentRequest"]="true"; QVERIFY(!AgentProvision::verifyEnvelope("vm.create",tampered,{n},error));
        tampered=input; tampered["provisionEpoch"]=qulonglong(2); QVERIFY(!AgentProvision::verifyEnvelope("vm.create",tampered,{n},error));
        tampered=input; auto dry=args; dry["dry_run"]=true; tampered["provisionArgs"]=dry; QVERIFY(!AgentProvision::verifyEnvelope("vm.create",tampered,{n},error));
        QVERIFY(!AgentProvision::verifyEnvelope("networks.save",input,{n},error));
        auto changed=n; changed["managed"]=false; QVERIFY(!AgentProvision::verifyEnvelope("vm.create",input,{changed},error));
        changed=n; changed["revision"]=QString(64,'b'); QVERIFY(!AgentProvision::verifyEnvelope("vm.create",input,{changed},error));
        changed=n; changed["available"]=false; QVERIFY(!AgentProvision::verifyEnvelope("vm.create",input,{changed},error));
        QVERIFY(make(Paths::root()+"/appliances/remnux.qcow2")); // Even same-size in-place writes change ctime/mtime.
        QVERIFY(!AgentProvision::verifyEnvelope("vm.create",input,{n},error));
        const auto appliance=Paths::root()+"/appliances";
        QVERIFY(QDir().rename(appliance,appliance+"-real")); QVERIFY(QFile::link(appliance+"-real",appliance));
        QVERIFY(AgentProvision::openMedia("disk","remnux.qcow2",identity,error)<0);
        QVERIFY(QFile::link(dir.path(),Paths::vms())); QVERIFY(!AgentProvision::storageRoot(error));
        QVERIFY(!AgentProvision::storageRoot(error,true));
        QVERIFY(!QFileInfo(dir.filePath("system.qcow2")).exists());
        QVariantMap netArgs{{"request_id",args["request_id"]},{"name","lan"},{"mode","isolated"}};
        QVERIFY(AgentProvision::prepare("create_network",netArgs,{},input,error));
        QCOMPARE(input["autostart"].toBool(),true); QCOMPARE(input["authorize"].toBool(),false);
        input["agentRequest"]=true; input["provisionEpoch"]=qulonglong(1); QVERIFY(AgentProvision::verifyEnvelope("networks.save",input,{},error));
        QVERIFY(!AgentProvision::verifyEnvelope("networks.save",input,{n},error)); // duplicate name, including non-owned networks
        QVariantMap auth{{"request_id",args["request_id"]},{"network",netId},{"revision",n["revision"]}};
        QVERIFY(AgentProvision::prepare("authorize_network",auth,{n},input,error));
        input["agentRequest"]=true; input["provisionEpoch"]=qulonglong(1); QVERIFY(AgentProvision::verifyEnvelope("networks.authorize",input,{n},error));
        changed=n; changed["active"]=false; QVERIFY(!AgentProvision::verifyEnvelope("networks.authorize",input,{changed},error));
    }
    void provisioningPinnedDisk() {
        if (QStandardPaths::findExecutable("qemu-img").isEmpty()) QSKIP("qemu-img unavailable; descriptor I/O tested by media/envelope test.");
        QTemporaryDir dir(QDir::homePath()+"/.omaware-pinned-test-XXXXXX"); QVERIFY(dir.isValid());
        const auto old=qgetenv("XDG_DATA_HOME"); qputenv("XDG_DATA_HOME",dir.path().toUtf8());
        const auto restore=qScopeGuard([&] { if(old.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME",old); });
        QVERIFY(QDir().mkpath(Paths::root()+"/appliances"));
        // Independent of the machine's umask (Ubuntu uses 002): the library must not be group-writable.
        for (const auto &d : {Paths::root(), Paths::root()+"/appliances"}) QVERIFY(QFile::setPermissions(d, QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto raw=Paths::root()+"/appliances/tiny.raw";
        QFile f(raw); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(QByteArray(4096,'x')),4096); f.close();
        QVERIFY(QFile::setPermissions(raw, QFile::ReadOwner|QFile::WriteOwner));
        QVariantMap identity; QString error;
        const int fd=AgentProvision::openMedia("disk","tiny.raw",identity,error); QVERIFY2(fd>=0,qPrintable(error));
        const auto close=qScopeGuard([&] { ::close(fd); });
        const auto pinned=QString("/proc/%1/fd/%2").arg(::getpid()).arg(fd);
        // Replacement of the name cannot change the source qemu-img opens.
        QVERIFY(QFile::rename(raw,raw+".old")); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(QByteArray(4096,'y')),4096); f.close();
        auto run=[](QStringList args,QByteArray &out) { QProcess p; p.start("qemu-img",args); if(!p.waitForStarted(5000)||!p.waitForFinished(5000)) return false; out=p.readAllStandardOutput(); return p.exitStatus()==QProcess::NormalExit && p.exitCode()==0; };
        QByteArray out;
        const auto copy=dir.filePath("copy.qcow2"); QVERIFY(run({"convert","-f","raw","-O","qcow2",pinned,copy},out));
        QVERIFY(run({"info","--output=json","-f","qcow2",copy},out));
        const auto info=QJsonDocument::fromJson(out).toVariant().toMap(); QVERIFY(!info.contains("backing-filename"));
        const auto check=dir.filePath("copy.raw"); QVERIFY(run({"convert","-f","qcow2","-O","raw",copy,check},out));
        QFile result(check); QVERIFY(result.open(QIODevice::ReadOnly)); QCOMPARE(result.readAll(),QByteArray(4096,'x'));
    }
    void provisioningAccessEpoch() {
        // Construction alone does not open libvirt or start a worker thread.
        VmWorker worker("qemu:///session");
        QCOMPARE(worker.provisionEpoch(), quint64(0));
        worker.setProvisionAccess(true);
        const auto approved = worker.provisionEpoch();
        QVERIFY(approved % 2 == 1);
        worker.setProvisionAccess(true);
        QCOMPARE(worker.provisionEpoch(), approved);
        worker.setProvisionAccess(false);
        QVERIFY(worker.provisionEpoch() % 2 == 0);
        QVERIFY(worker.provisionEpoch() != approved);
        worker.setProvisionAccess(true);
        QVERIFY(worker.provisionEpoch() % 2 == 1);
        QVERIFY(worker.provisionEpoch() != approved);
    }
    void provisioningAdmission() {
        QHash<QString,QVariantMap> requests, states;
        const QVariantMap request{{"tool","create_vm"},{"args",QVariantMap{{"name","remnux"}}}};
        QCOMPARE(AgentProvision::admission("id",request,requests,states),QString("new"));
        requests["id"]=request;
        for (auto state : {"preparing","awaiting_approval","running","succeeded","failed","declined"}) {
            states["id"]={{"state",state}};
            QCOMPARE(AgentProvision::admission("id",request,requests,states),QString("existing"));
            QCOMPARE(AgentProvision::admission("id",{{"tool","create_network"}},requests,states),QString("request_conflict"));
            QCOMPARE(AgentProvision::admission("other",request,requests,states),QString(QStringList{"preparing","awaiting_approval","running"}.contains(state) ? "busy" : "new"));
        }
        for (int i=1;i<128;++i) states[QString::number(i)]={{"state","succeeded"}};
        QCOMPARE(AgentProvision::admission("new",request,requests,states),QString("session_limit"));
        QCOMPARE(AgentProvision::admission("id",request,requests,states),QString("existing"));
    }
    void provisioningProtocol() {
        for (const auto &v : AgentProvision::tools()) {
            const auto tool=v.toMap(); QCOMPARE(tool["inputSchema"].toMap()["additionalProperties"].toBool(),false);
            int count=0; for(const auto &t : Mcp::tools()) if(t.toMap()["name"]==tool["name"]) ++count; QCOMPARE(count,1);
        }
        auto request=QJsonObject{{"id",1},{"method","tools/call"},{"params",QJsonObject{{"name","create_vm"},{"arguments",QJsonObject{{"source","/etc/passwd"}}}}}};
        bool called=false;
        auto response=Mcp::respond(QJsonDocument(request).toJson(),[&](const QString &,const QVariantMap &) { called=true; return QVariantMap{}; });
        QVERIFY(!called); QCOMPARE(QJsonDocument::fromJson(response).object()["error"].toObject()["code"].toInt(),-32602);
        request["params"]=QJsonObject{{"name","provision_status"},{"arguments",QJsonObject{{"request_id","10000000-0000-4000-8000-000000000001"}}}};
        response=Mcp::respond(QJsonDocument(request).toJson(),[&](const QString &tool,const QVariantMap &args) { called=tool=="provision_status" && args.contains("request_id"); return QVariantMap{{"ok",true},{"result",QVariantMap{{"state","awaiting_approval"}}}}; });
        QVERIFY(called); QCOMPARE(QJsonDocument::fromJson(response).object()["result"].toObject()["structuredContent"].toObject()["state"].toString(),QString("awaiting_approval"));
    }
    void bridgeHelperReasons() {
        // pkexec's own exit codes.
        QCOMPARE(BridgeHelper::classify(126,""),QString("authorization_cancelled"));
        QCOMPARE(BridgeHelper::classify(127,"Error executing command as another user: Not authorized"),QString("authorization_denied"));
        QCOMPARE(BridgeHelper::classify(127,"Error executing command as another user: No authentication agent found."),QString("authorization_unavailable"));
        // The helper's exit codes, and the messages of helpers from before they had codes.
        QCOMPARE(BridgeHelper::classify(3,""),QString("network_not_owned"));
        QCOMPARE(BridgeHelper::classify(4,""),QString("bridge_inactive"));
        QCOMPARE(BridgeHelper::classify(5,""),QString("deny_rule"));
        QCOMPARE(BridgeHelper::classify(6,""),QString("bridge_policy_unsafe"));
        QCOMPARE(BridgeHelper::classify(7,""),QString("qemu_bridge_helper_missing"));
        QCOMPARE(BridgeHelper::classify(1,"An existing deny rule blocks this bridge. Ask the host administrator to review its policy."),QString("deny_rule"));
        QCOMPARE(BridgeHelper::classify(1,"The managed bridge is not active. Start the network first."),QString("bridge_inactive"));
        QCOMPARE(BridgeHelper::classify(1,"The selected network is not managed by OmaWare."),QString("network_not_owned"));
        QCOMPARE(BridgeHelper::classify(1,"Traceback (most recent call last): ..."),QString("helper_failed"));
        // The script's exit codes are the ones classify() knows.
        QFile script(QStringLiteral(QT_TESTCASE_SOURCEDIR)+"/scripts/authorize-bridge.py"); QVERIFY(script.open(QIODevice::ReadOnly));
        QVERIFY(script.readAll().contains("NOT_OWNED, INACTIVE, DENIED, UNSAFE_POLICY, NO_BRIDGE_HELPER = 3, 4, 5, 6, 7"));
        // Agent messages never contain helper output, and no path but the helper's documented location.
        for (const auto &code : {"helper_missing","helper_untrusted","authorization_unavailable","authorization_cancelled","authorization_denied","authorization_timeout","deny_rule","bridge_inactive","network_not_owned","bridge_policy_unsafe","qemu_bridge_helper_missing","helper_failed","something_new"}) {
            auto text=BridgeHelper::agentMessage(code); QVERIFY(!text.isEmpty());
            text.remove(BridgeHelper::destination());
            QVERIFY2(!QRegularExpression("(/[A-Za-z0-9_.-]+){2,}").match(text).hasMatch(),qPrintable(text));
        }
        // Only a root-owned file in root-owned, unshared folders is trusted.
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QCOMPARE(BridgeHelper::check(dir.filePath("missing")),QString("helper_missing"));
        QFile mine(dir.filePath("authorize-bridge")); QVERIFY(mine.open(QIODevice::WriteOnly)); mine.write("#!/bin/sh\n"); mine.close();
        QVERIFY(mine.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QCOMPARE(BridgeHelper::check(mine.fileName()),QString("helper_untrusted"));
        QVERIFY(QFile::link(mine.fileName(),dir.filePath("link")));
        QCOMPARE(BridgeHelper::check(dir.filePath("link")),QString("helper_untrusted"));
        if (QFileInfo("/usr/bin/env").isFile() && !QFileInfo("/usr/bin/env").isSymLink() && QFileInfo("/usr/bin/env").ownerId()==0)
            QCOMPARE(BridgeHelper::check("/usr/bin/env"),QString());
        // The install commands use the real script path, quoted for the shell.
        const auto commands=BridgeHelper::installCommands("/opt/My Apps/omaware/authorize-bridge","/home/me/build dir");
        QCOMPARE(commands.size(),2);
        QCOMPARE(commands[0],"sudo install -D -o root -g root -m 0755 '/opt/My Apps/omaware/authorize-bridge' "+BridgeHelper::destination());
        QCOMPARE(commands[1],QString("sudo cmake --install '/home/me/build dir' --component helper"));
        QVERIFY(BridgeHelper::installCommands("",{}).first().startsWith("sudo install -D -o root -g root -m 0755 "));
        // The build folder has the copy the command points to.
        QVERIFY(QFileInfo(QCoreApplication::applicationDirPath()+"/authorize-bridge").isExecutable());
    }
    void provisioningFailureReasons() {
        // provision_status passes the stable code and short reason through to MCP clients.
        auto request=QJsonObject{{"id",1},{"method","tools/call"},{"params",QJsonObject{{"name","provision_status"},{"arguments",QJsonObject{{"request_id","10000000-0000-4000-8000-000000000001"}}}}}};
        const QVariantMap state{{"state","failed"},{"tool","authorize_network"},{"message",BridgeHelper::agentMessage("helper_missing")},{"result",QVariantMap{{"code","helper_missing"},{"reason",BridgeHelper::agentMessage("helper_missing")}}}};
        auto response=Mcp::respond(QJsonDocument(request).toJson(),[&](const QString &,const QVariantMap &) { return QVariantMap{{"ok",true},{"result",state}}; });
        const auto structured=QJsonDocument::fromJson(response).object()["result"].toObject()["structuredContent"].toObject();
        QCOMPARE(structured["state"].toString(),QString("failed"));
        QCOMPARE(structured["result"].toObject()["code"].toString(),QString("helper_missing"));
        QVERIFY(structured["result"].toObject()["reason"].toString().contains(BridgeHelper::destination()));
    }
    void provisioningPublicResult() {
        // A worker failure with the helper's own output: agents get the code and fixed reason only.
        const QVariantMap worker{{"message","pkexec: Error executing command as another user: Not authorized\n/usr/local/libexec/omaware/authorize-bridge"},{"code","authorization_denied"},{"reason",BridgeHelper::agentMessage("authorization_denied")},{"requestTag","x"},{"xml","<network/>"}};
        auto shown=AgentProvision::publicResult(false,worker);
        QCOMPARE(shown["result"].toMap(),QVariantMap({{"code","authorization_denied"},{"reason",BridgeHelper::agentMessage("authorization_denied")}}));
        QVERIFY(shown["message"].toString().startsWith(BridgeHelper::agentMessage("authorization_denied")));
        QVERIFY(!QJsonDocument::fromVariant(shown).toJson().contains("pkexec")); QVERIFY(!QJsonDocument::fromVariant(shown).toJson().contains("<network"));
        // Other failures keep a generic message and code; successes keep only public fields.
        shown=AgentProvision::publicResult(false,{{"message","virt-install: /home/user/secret path"}});
        QCOMPARE(shown["result"].toMap()["code"].toString(),QString("operation_failed")); QVERIFY(!shown["message"].toString().contains("/home"));
        shown=AgentProvision::publicResult(true,{{"uuid","u"},{"revision","r"},{"storage","/home/user/vms/x"},{"message","done"}});
        QCOMPARE(shown["result"].toMap(),QVariantMap({{"uuid","u"},{"revision","r"}}));
    }
    void managementSchemas() {
        QString error;
        // manage_network needs an owned network, an action and the revision the agent saw.
        QVERIFY(Mcp::validateManagementArguments("manage_network",{{"network","lan"},{"action","set_autostart"},{"revision",QString(64,'a')},{"autostart",true}},error));
        QVERIFY(!Mcp::validateManagementArguments("manage_network",{{"network","lan"},{"action","set_autostart"},{"autostart",true}},error));
        QVERIFY(!Mcp::validateManagementArguments("manage_network",{{"network","lan"},{"action","rename"},{"revision","r"}},error));
        QVERIFY(!Mcp::validateManagementArguments("manage_network",{{"network","lan"},{"action","edit"},{"revision","r"},{"bridge","virbr0"}},error));
        QVERIFY(!Mcp::validateManagementArguments("manage_network",{{"network","lan"},{"action","edit"},{"revision","r"},{"mode","bridged"}},error));
        // Adapter changes can ask for a restart when they can't apply live.
        QVERIFY(Mcp::validateManagementArguments("manage_network_adapter",{{"vm","fw"},{"action","update"},{"mac","52:54:00:00:00:01"},{"apply","restart_if_needed"},{"restart_timeout_seconds",60}},error));
        QVERIFY(!Mcp::validateManagementArguments("manage_network_adapter",{{"vm","fw"},{"action","update"},{"apply","force"}},error));
        QVERIFY(!Mcp::validateManagementArguments("manage_network_adapter",{{"vm","fw"},{"action","update"},{"restart_timeout_seconds",1.5}},error));
        // The tool descriptions agents read say what changed.
        QString runCommand;
        for (const auto &t : Mcp::tools()) if (t.toMap()["name"]=="run_command") runCommand=t.toMap()["description"].toString();
        QVERIFY(runCommand.contains("isolated networks too")); QVERIFY(!runCommand.contains("Not for VMs only on isolated networks"));
    }
    void sessionGrantScope() {
        // Only owned isolated or host-only bridges, or removals; never the internet, a local network or a VM-private connection.
        const QVariantMap isolated{{"id","bridge:oma1"},{"kind","bridge"},{"managed",true},{"category","Isolated"}};
        auto hostonly=isolated; hostonly["category"]="Host-only";
        auto nat=isolated; nat["category"]="Shared NAT";
        auto unmanaged=isolated; unmanaged["managed"]=false;
        const QVariantMap user{{"id","user"},{"kind","user"},{"category","NAT"}};
        QCOMPARE(AgentGrants::forAdapterChange("add",isolated),QString(AgentGrants::privateAdapters));
        QCOMPARE(AgentGrants::forAdapterChange("update",hostonly),QString(AgentGrants::privateAdapters));
        QCOMPARE(AgentGrants::forAdapterChange("remove",{}),QString(AgentGrants::privateAdapters));
        for (const auto &target : {nat,unmanaged,user}) { QVERIFY(AgentGrants::forAdapterChange("add",target).isEmpty()); QVERIFY(AgentGrants::forAdapterChange("update",target).isEmpty()); }
        QVERIFY(AgentGrants::forAdapterChange("list",isolated).isEmpty());
        QVERIFY(AgentGrants::label(AgentGrants::privateAdapters).contains("isolated"));
        QCOMPARE(AgentGrants::lifetimeMs,8LL*3600*1000);
        // The serial console and serial logins are agent tools with checked arguments.
        QString error;
        QVERIFY(Mcp::validateManagementArguments("serial_console",{{"vm","fw"},{"send","8\n"},{"wait_for","Enter an option"},{"timeout_seconds",20}},error));
        QVERIFY(!Mcp::validateManagementArguments("serial_console",{{"vm","fw"},{"send",8}},error));
        QVERIFY(!Mcp::validateManagementArguments("serial_console",{{"vm","fw"},{"device","/dev/ttyS1"}},error));
        bool listed=false; for (const auto &t : Mcp::tools()) if (t.toMap()["name"]=="type_login") listed=t.toMap()["inputSchema"].toMap()["properties"].toMap().contains("via");
        QVERIFY(listed);
    }
    void diagnosisHints() {
        auto codes=[](const QVariantList &hints) { QStringList out; for (const auto &h : hints) out << h.toMap()["code"].toString(); return out; };
        const QVariantMap running{{"active",true},{"agentConfigured",true},{"agentConnected",false},{"changes",QVariantList{}}};
        const QVariantMap noAddress{{"agent",false},{"interfaces",QVariantList{QVariantMap{{"mac","52:54:00:00:00:01"},{"linkUp",true},{"ips",QVariantList{}}}}}};
        QCOMPARE(codes(AgentDiagnosis::hints(running,noAddress)),QStringList({"guest_agent_not_running","no_ipv4_seen"}));
        auto agent=running; agent["agentConnected"]=true; auto reported=noAddress; reported["agent"]=true;
        const auto hints=AgentDiagnosis::hints(agent,reported);
        QCOMPARE(codes(hints),QStringList({"guest_has_no_ipv4"})); QVERIFY(hints[0].toMap()["hint"].toString().contains("ens33"));
        auto addressed=reported; addressed["interfaces"]=QVariantList{QVariantMap{{"mac","52:54:00:00:00:01"},{"linkUp",true},{"ips",QVariantList{"172.30.1.10"}}}};
        QVERIFY(AgentDiagnosis::hints(agent,addressed).isEmpty());
        auto pending=agent; pending["changes"]=QVariantList{QVariantMap{{"key","network"}}};
        QCOMPARE(codes(AgentDiagnosis::hints(pending,addressed)),QStringList({"restart_needed"}));
        auto stopped=running; stopped["active"]=false; QVERIFY(AgentDiagnosis::hints(stopped,noAddress).isEmpty());
        auto noChannel=running; noChannel["agentConfigured"]=false; QCOMPARE(codes(AgentDiagnosis::hints(noChannel,{})),QStringList({"no_guest_agent_channel"}));
    }
    void protocol() {
        QStringList names;
        for (const auto &v : Mcp::tools()) names << v.toMap()["name"].toString();
        for (const auto &name : {"vm_details", "transfer_file", "wait_for_vm", "update_vm_resources", "diagnose_vm", "clone_vm", "manage_iso", "manage_network_adapter"}) {
            QCOMPARE(names.count(name), 1);
            bool called = false;
            auto response = Mcp::respond(QJsonDocument(QJsonObject{{"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", QJsonObject{{"vm", "test"}}}}}}).toJson(), [&](const QString &tool, const QVariantMap &args) {
                called = tool == name && args["vm"] == "test";
                return QVariantMap{{"ok", false}, {"error", "declined"}, {"result", QVariantMap{{"code", "declined"}}}};
            });
            QVERIFY(called);
            auto result = QJsonDocument::fromJson(response).object()["result"].toObject();
            QVERIFY(result["isError"].toBool());
            QCOMPARE(result["structuredContent"].toObject()["code"].toString(), QString("declined"));
        }
    }
};
QTEST_GUILESS_MAIN(AgentToolsTest)
#include "test_agenttools.moc"
