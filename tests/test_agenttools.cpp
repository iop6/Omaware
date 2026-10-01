// SPDX-License-Identifier: GPL-3.0-or-later
#include "agenttransfer.h"
#include "mcpserver.h"
#include "paths.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <unistd.h>

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
