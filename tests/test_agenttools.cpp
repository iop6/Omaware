// SPDX-License-Identifier: GPL-3.0-or-later
#include "agenttransfer.h"
#include "agentprovision.h"
#include "backend.h"
#include "mcpserver.h"
#include "paths.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
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
        for (const auto &change : QVariantList{QVariantMap{{"cpus","2"}},QVariantMap{{"cpus",0}},QVariantMap{{"cpus",257}},QVariantMap{{"cpus",true}},QVariantMap{{"memory_mib",255}},QVariantMap{{"dry_run","false"}},QVariantMap{{"source","/etc/passwd"}},QVariantMap{{"location","/tmp"}},QVariantMap{{"start",true}},QVariantMap{{"media","../remnux.qcow2"}},QVariantMap{{"media","id_rsa"}},QVariantMap{{"media_kind","cloud"}},QVariantMap{{"disk_gib",32}},QVariantMap{{"networks",QVariantList{"user"}}},QVariantMap{{"networks",QVariantList{1}}},QVariantMap{{"preset","--connect"}}}) {
            auto args = vm; const auto map = change.toMap(); for (auto it=map.begin();it!=map.end();++it) args[it.key()] = it.value();
            QVERIFY2(!Mcp::validateManagementArguments("create_vm",args,error),qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(args).toJson())));
        }
        const QVariantMap net{{"request_id",vm["request_id"]},{"name","lab-lan"},{"mode","isolated"}};
        QVERIFY(AgentProvision::validate("create_network",net,error));
        auto args=net; args["dhcp"]=false; QVERIFY(!AgentProvision::validate("create_network",args,error));
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
        QCOMPARE(input["autostart"].toBool(),false); QCOMPARE(input["authorize"].toBool(),false);
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
