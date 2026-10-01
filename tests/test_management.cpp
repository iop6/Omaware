// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "console.h"
#include "theme.h"
#include "workspace.h"
#include "domainconfig.h"
#include "configuration.h"
#include "checkpoints.h"
#include "containment.h"
#include "isolibrary.h"
#include "updater.h"
#include "paths.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>
#include <QProcess>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QJSValue>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDomDocument>
#include <QNetworkInterface>
#include <QBuffer>
#include <libvirt/libvirt-qemu.h>

class ManagementTest : public QObject {
    Q_OBJECT
    std::unique_ptr<Backend> backend;
    virConnectPtr external = nullptr;
    QStringList created;
    QString networkUuid;
    QTemporaryDir files;
    bool resultOk = false;
    QVariantMap command(QString op, QVariantMap args = {}) {
        if (op == "snapshots.restore" && !args.contains("safety")) args["safety"] = false;
        QSignalSpy results(backend.get(), &Backend::commandFinished);
        if (!backend->request(op, args)) { resultOk = false; return {{"message", "Backend busy"}}; }
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 30000) {
            for (const auto &result : results) if (result[0] == op) { resultOk = result[1].toBool(); return result[2].toMap(); }
            QTest::qWait(10);
        }
        resultOk = false; return {{"message", "Operation timed out: " + op}};
    }
    QString xml(QString uuid, bool live = false) {
        auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); if (!d) return {};
        char *raw = virDomainGetXMLDesc(d, VIR_DOMAIN_XML_SECURE | (live ? 0 : VIR_DOMAIN_XML_INACTIVE));
        QString result = raw ? QString::fromUtf8(raw) : QString{}; free(raw); virDomainFree(d); return result;
    }
    QVariantMap details(QString uuid, bool live = false) { QString error; return DomainConfig::describe(xml(uuid, live), error); }
    bool active(QString uuid) { auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); if (!d) return false; bool result = virDomainIsActive(d) == 1; virDomainFree(d); return result; }
    unsigned domainId(QString uuid) { auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); if (!d) return unsigned(-1); auto id = virDomainGetID(d); virDomainFree(d); return id; }
    bool process(QString program, QStringList args) { QProcess p; p.start(program, args); return p.waitForFinished(30000) && p.exitCode() == 0; }
    bool process(QString program, QStringList args, QByteArray *output) { QProcess p; p.start(program, args); const bool ok = p.waitForFinished(30000) && p.exitCode() == 0; *output = p.readAllStandardOutput(); return ok; }
    QString makeSource() {
        auto path = files.filePath("source-" + QUuid::createUuid().toString(QUuid::Id128) + ".raw");
        if (!process("qemu-img", {"create", "-f", "raw", path, "64M"}) || !process("qemu-io", {"-f", "raw", "-c", "write -P 0x31 0 4096", path})) return {};
        return path;
    }
private slots:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariable("OMAWARE_VM_TEST") == "1" && qEnvironmentVariable("OMAWARE_VM_TEST_HOST") == QSysInfo::machineHostName(),
            "VM tests create, pause and power off OmaWare VMs in your libvirt session. Run them only on a disposable machine, "
            "with OMAWARE_VM_TEST=1 and OMAWARE_VM_TEST_HOST set to that machine's hostname.");
        QVERIFY(files.isValid()); external = virConnectOpen("qemu:///session"); QVERIFY(external);
    }
    void init() {
        QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/workspace.ini", QSettings::IniFormat).clear();
        backend = std::make_unique<Backend>("qemu:///session");
        connect(backend.get(), &Backend::created, this, [this](QString uuid) {
            created << uuid;
            auto journalPath = qEnvironmentVariable("OMAWARE_VM_JOURNAL");
            if (!journalPath.isEmpty()) { QFile journal(journalPath); if (journal.open(QIODevice::Append)) journal.write(uuid.toUtf8() + "\n"); }
        });
        QTRY_VERIFY(backend->connected());
    }
    void cleanup() {
        backend.reset();
        for (const auto &uuid : created) {
            auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData());
            if (d) {
                if (virDomainIsActive(d) == 1) virDomainDestroy(d);
                virDomainSnapshotPtr *snapshots = nullptr; int n = virDomainListAllSnapshots(d, &snapshots, VIR_DOMAIN_SNAPSHOT_LIST_ROOTS);
                for (int i = 0; i < n; ++i) { virDomainSnapshotDelete(snapshots[i], VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN); virDomainSnapshotFree(snapshots[i]); }
                free(snapshots); virDomainUndefineFlags(d, VIR_DOMAIN_UNDEFINE_NVRAM | VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA); virDomainFree(d);
            }
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/vms/" + uuid).removeRecursively();
            QDir(Paths::vms() + "/" + uuid).removeRecursively();
            QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pending/" + uuid + ".json");
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + uuid).removeRecursively();
        }
        created.clear();
        if (!networkUuid.isEmpty()) {
            auto c = virConnectOpen("qemu:///system");
            if (c) { auto n = virNetworkLookupByUUIDString(c, networkUuid.toUtf8().constData()); if (n) { if (virNetworkIsActive(n) == 1) virNetworkDestroy(n); virNetworkUndefine(n); virNetworkFree(n); } virConnectClose(c); }
            networkUuid.clear();
        }
    }
    void cleanupTestCase() { if (external) virConnectClose(external); }
    void creationSettingsAndCheckpoints() {
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        QVariantMap values{{"name", "acceptance-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source},
            {"preset", "generic"}, {"firmware", "bios"}, {"cpus", 1}, {"memoryMiB", 256}, {"diskGiB", 1}, {"networkId", "user"}, {"location", files.path()}};
        auto bad = values; bad["source"] = files.filePath("missing.raw");
        command("vm.create", bad); QVERIFY(!resultOk); QVERIFY(created.isEmpty());
        auto result = command("vm.create", values); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        const auto uuid = result["uuid"].toString(); QVERIFY(created.contains(uuid));
        auto info = details(uuid); auto disk = info["disks"].toList().first().toMap()["source"].toString();
        QVERIFY(disk != source); QVERIFY(QFile::exists(disk));
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x31 0 4096", disk}));
        auto revision = info["revision"].toString();
        result = command("hardware.save", {{"uuid", uuid}, {"revision", revision}, {"cpus", 2}, {"memoryMiB", 512}, {"boot", "hd"}, {"clipboard", true}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        info = details(uuid); QCOMPARE(info["vcpus"].toInt(), 2); QCOMPARE(info["memoryMiB"].toInt(), 512); QVERIFY(info["clipboardConfigured"].toBool());
        command("hardware.save", {{"uuid", uuid}, {"revision", revision}, {"cpus", 1}, {"memoryMiB", 512}}); QVERIFY(!resultOk);
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        result = command("stats", {{"uuid", uuid}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QVERIFY(result["uptimeSeconds"].toDouble() >= 0);
        result = command("hardware.save", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"cpus", 1}, {"memoryMiB", 768}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(details(uuid, true)["vcpus"].toInt(), 2); QCOMPARE(details(uuid, true)["memoryMiB"].toInt(), 512);
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy()); QCOMPARE(backend->details()["changes"].toList().size(), 2);
        PendingChanges reloaded(uuid); QCOMPARE(reloaded.items(xml(uuid)).size(), 2); QVERIFY(reloaded.matches(xml(uuid)));
        const auto pendingPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pending/" + uuid + ".json";
        QVERIFY(!(QFileInfo(pendingPath).permissions() & (QFile::ReadGroup | QFile::ReadOther)));
        result = command("pending.discard", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"key", "memory"}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); QCOMPARE(details(uuid)["memoryMiB"].toInt(), 512); QCOMPARE(details(uuid)["vcpus"].toInt(), 1);
        result = command("pending.discard", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"key", "all"}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); QCOMPARE(details(uuid)["vcpus"].toInt(), 2);
        result = command("vm.restart", {{"uuid", uuid}}); QVERIFY(resultOk); QVERIFY(result["waiting"].toBool()); QVERIFY(backend->busy());
        backend->cancelRestart(); QTRY_VERIFY(!backend->busy()); QVERIFY(active(uuid));
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        // Existing internal checkpoints from earlier OmaWare versions remain usable.
        auto legacyDomain = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(legacyDomain);
        auto legacy = virDomainSnapshotCreateXML(legacyDomain, "<domainsnapshot><name>Before update</name><description>Known first disk block</description><memory snapshot='no'/></domainsnapshot>", 0);
        QVERIFY(legacy); virDomainSnapshotFree(legacy); virDomainFree(legacyDomain);
        result = command("snapshots.list", {{"uuid", uuid}}); QCOMPARE(result["items"].toList().size(), 1);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x52 0 4096", disk}));
        result = command("snapshots.restore", {{"uuid", uuid}, {"name", "Before update"}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x31 0 4096", disk}));
        backend->action(uuid, "start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy());
        auto legacyId = domainId(uuid);
        result = command("snapshots.restore", {{"uuid", uuid}, {"name", "Before update"}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(active(uuid)); QVERIFY(domainId(uuid) != legacyId);
        backend->action(uuid, "pause"); QTRY_VERIFY(!backend->busy()); legacyId = domainId(uuid);
        result = command("snapshots.restore", {{"uuid", uuid}, {"name", "Before update"}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        auto legacyPaused = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(legacyPaused);
        int legacyState = 0, legacyReason = 0; QCOMPARE(virDomainGetState(legacyPaused, &legacyState, &legacyReason, 0), 0); virDomainFree(legacyPaused);
        QCOMPARE(legacyState, VIR_DOMAIN_PAUSED); QVERIFY(domainId(uuid) != legacyId);
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        result = command("snapshots.remove", {{"uuid", uuid}, {"name", "Before update"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("disk.add", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"diskGiB", 1}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        auto disks = details(uuid)["disks"].toList(); QCOMPARE(disks.size(), 2); auto extra = disks.last().toMap();
        result = command("disk.detach", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"target", extra["target"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(QFile::exists(extra["source"].toString())); QCOMPARE(details(uuid)["disks"].toList().size(), 1);
        QVERIFY(process("qemu-io", {"-f", "raw", "-c", "read -P 0x31 0 4096", source}));
        auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(d);
        QVERIFY(virDomainSetMetadata(d, VIR_DOMAIN_METADATA_ELEMENT, nullptr, nullptr, "https://omaware.org/xmlns/prototype/1", VIR_DOMAIN_AFFECT_CONFIG) == 0); virDomainFree(d);
        auto original = xml(uuid);
        command("hardware.save", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"cpus", 1}, {"memoryMiB", 256}}); QVERIFY(!resultOk); QCOMPARE(xml(uuid), original);
    }
    void wizardAndWorkspaceUi() {
        Theme theme("/nonexistent/palette"); QQmlApplicationEngine engine; QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](QList<QQmlError> items) { warnings += items; });
        engine.rootContext()->setContextProperty("backend", backend.get()); engine.rootContext()->setContextProperty("theme", &theme);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window); window->requestActivate();
        auto capture = [&](QString name) { QTest::qWait(200); auto dir = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR"); if (dir.isEmpty()) return true; return window->grabWindow().save(dir + "/" + name + ".png"); };
        auto click = [&](QObject *scope, const char *name, Qt::MouseButton button = Qt::LeftButton) {
            auto item = scope->findChild<QQuickItem *>(name);
            // Repeater delegates belong to the visual tree, not QObject children.
            std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *parent) -> QQuickItem * {
                if (!parent) return nullptr;
                if (parent->objectName() == name) return parent;
                for (auto child : parent->childItems()) if (auto match = find(child)) return match;
                return nullptr;
            };
            if (!item) item = find(qobject_cast<QQuickItem *>(scope));
            if (!item) item = find(qobject_cast<QQuickItem *>(scope->property("contentItem").value<QObject *>()));
            if (!item || !item->isEnabled() || !item->isVisible()) return false;
            QTest::mouseClick(item->window(), button, Qt::NoModifier, item->mapToScene({item->width()/2,item->height()/2}).toPoint()); return true;
        };
        window->resize(1280, 840);
        QVERIFY(capture("library-empty"));
        // The ISO window lists the publishers and the ISOs already in the folder (offline in tests).
        auto isos = qobject_cast<IsoLibrary *>(window->findChild<QObject *>("isoLibrary")); QVERIFY(isos);
        isos->setProperty("autoCheck", false);
        QCOMPARE(isos->folder(), Paths::isos());
        isos->setFolder(files.filePath("iso-library"));
        { QFile fake(files.filePath("iso-library/debian-13.1.0-amd64-netinst.iso")); QVERIFY(fake.open(QIODevice::WriteOnly)); fake.write("fixture"); }
        isos->rescan(); QCOMPARE(isos->files().size(), 1);
        auto shop = window->findChild<QObject *>("isoShop"); QVERIFY(shop);
        QVERIFY(click(window, "isoShopNav")); QTRY_COMPARE(window->property("navigation").toString(), QString("isos"));
        QVERIFY(capture("iso-shop"));
        QCOMPARE(shop->property("shown").toList().size(), isos->sourceIds().size());
        shop->setProperty("category", "server"); QTRY_VERIFY(shop->property("shown").toList().size() < isos->sourceIds().size()); QVERIFY(capture("iso-shop-server"));
        shop->setProperty("category", "all"); shop->setProperty("query", "mint"); QTRY_COMPARE(shop->property("shown").toList().size(), 1);
        shop->setProperty("query", ""); shop->setProperty("category", "mine"); QVERIFY(capture("iso-shop-mine"));
        // "New VM" from the shop opens the create dialog with that ISO chosen.
        QVERIFY(QMetaObject::invokeMethod(shop, "useIso", Q_ARG(QString, files.filePath("iso-library/debian-13.1.0-amd64-netinst.iso"))));
        auto creator = window->findChild<QObject *>("createVmDialog"); QVERIFY(creator); QTRY_VERIFY(creator->property("opened").toBool());
        QCOMPARE(creator->findChild<QObject *>("newVmSource")->property("text").toString(), files.filePath("iso-library/debian-13.1.0-amd64-netinst.iso"));
        QTRY_COMPARE(creator->findChild<QObject *>("newVmName")->property("text").toString(), QString("Debian 13.1"));
        QVERIFY(capture("create-from-shop"));
        QVERIFY(QMetaObject::invokeMethod(creator, "close")); QTRY_VERIFY(!creator->property("visible").toBool());
        // Quick deletes: older versions in one go, then a system's ISOs from its card.
        window->setProperty("navigation", "isos"); shop->setProperty("category", "mine");
        for (auto name : {"debian-13.0.0-amd64-netinst.iso", "alpine-virt-3.24.2-x86_64.iso"}) { QFile extra(files.filePath(QString("iso-library/") + name)); QVERIFY(extra.open(QIODevice::WriteOnly)); extra.write(QByteArray(2048, 'i')); }
        isos->rescan(); QCOMPARE(isos->files().size(), 3);
        QTRY_COMPARE(shop->property("olderNames").toList().size(), 1); QTest::qWait(100);
        QVERIFY(click(window, "isoDeleteOlder")); QTRY_COMPARE(shop->property("confirming").toString(), QString("older")); QVERIFY(capture("iso-delete-older"));
        QVERIFY(click(window, "isoBulkConfirm"));
        QTRY_COMPARE(isos->files().size(), 2); QVERIFY(!QFile::exists(files.filePath("iso-library/debian-13.0.0-amd64-netinst.iso")));
        QTRY_COMPARE(shop->property("confirming").toString(), QString()); QTest::qWait(100);
        QVERIFY(click(window, "isoSelectAll")); QTRY_COMPARE(shop->property("pickedNames").toList().size(), 2); QVERIFY(capture("iso-selected"));
        QVERIFY(click(window, "isoSelectAll")); QTRY_COMPARE(shop->property("pickedNames").toList().size(), 0);
        shop->setProperty("category", "server");
        QTest::qWait(200); QVERIFY(click(window, "isoDelete_alpine")); QTest::qWait(100); QVERIFY(capture("iso-card-delete")); QVERIFY(click(window, "isoDeleteConfirm_alpine"));
        QTRY_COMPARE(isos->files().size(), 1); QCOMPARE(isos->files().first().toMap()["source"].toString(), QString("debian"));
        // Dropping an ISO onto the window adds it to the folder and opens Create VM with it chosen.
        shop->setProperty("category", "all"); window->setProperty("navigation", "library");
        auto dropZone = window->findChild<QObject *>("isoDropZone"); QVERIFY(dropZone);
        QTemporaryDir downloads; QVERIFY(downloads.isValid());
        for (auto name : {"ubuntu-24.04.3-live-server-amd64.iso", "alpine-virt-3.24.2-x86_64.iso", "notes.txt"}) { QFile f(downloads.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(4096, 'd')); }
        QSignalSpy imported(isos, &IsoLibrary::imported);
        QVariant accepted;
        QVERIFY(QMetaObject::invokeMethod(dropZone, "dropUrls", Q_RETURN_ARG(QVariant, accepted), Q_ARG(QVariant, QVariantList{QUrl::fromLocalFile(downloads.filePath("ubuntu-24.04.3-live-server-amd64.iso"))})));
        QVERIFY(accepted.toBool()); QTRY_COMPARE(imported.size(), 1);
        const auto dropped = files.filePath("iso-library/ubuntu-24.04.3-live-server-amd64.iso");
        QVERIFY(QFile::exists(dropped)); QVERIFY(QFile::exists(downloads.filePath("ubuntu-24.04.3-live-server-amd64.iso")));
        QTRY_VERIFY(creator->property("opened").toBool());
        QCOMPARE(creator->findChild<QObject *>("newVmSource")->property("text").toString(), dropped);
        QTRY_COMPARE(creator->findChild<QObject *>("isoLibraryPicker")->property("currentText").toString(), QString("ubuntu-24.04.3-live-server-amd64.iso"));
        QVERIFY(dropZone->property("message").toString().contains("Added ubuntu-24.04.3-live-server-amd64.iso"));
        QVERIFY(capture("iso-dropped"));
        // With the dialog open, a drop switches its ISO; known files and non-ISOs are reported, not copied.
        const QVariantList several{QUrl::fromLocalFile(downloads.filePath("alpine-virt-3.24.2-x86_64.iso")), QUrl::fromLocalFile(downloads.filePath("ubuntu-24.04.3-live-server-amd64.iso")), QUrl::fromLocalFile(downloads.filePath("notes.txt"))};
        QVERIFY(QMetaObject::invokeMethod(dropZone, "dropUrls", Q_RETURN_ARG(QVariant, accepted), Q_ARG(QVariant, several)));
        QTRY_COMPARE(imported.size(), 2);
        QTRY_COMPARE(creator->findChild<QObject *>("newVmSource")->property("text").toString(), files.filePath("iso-library/alpine-virt-3.24.2-x86_64.iso"));
        const auto said = dropZone->property("message").toString();
        QVERIFY2(said.contains("Added alpine-virt-3.24.2-x86_64.iso") && said.contains("already in your ISOs") && said.contains("notes.txt was skipped"), qPrintable(said));
        QCOMPARE(isos->files().size(), 3);
        QVERIFY(QMetaObject::invokeMethod(creator, "close")); QTRY_VERIFY(!creator->property("visible").toBool());
        dropZone->setProperty("message", "");
        // Windows: UEFI and a TPM (when this computer can provide one) are chosen for you.
        { QFile w(files.filePath("iso-library/Win11_26H2_English_x64.iso")); QVERIFY(w.open(QIODevice::WriteOnly)); w.write("windows"); }
        isos->rescan();
        QVERIFY(QMetaObject::invokeMethod(creator, "beginWith", Q_ARG(QVariant, files.filePath("iso-library/Win11_26H2_English_x64.iso"))));
        QTRY_VERIFY(creator->property("opened").toBool());
        QTRY_COMPARE(creator->property("selectedPreset").toString(), QString("win11"));
        QTRY_VERIFY(creator->findChild<QQuickItem *>("windowsOptions")->isVisible());
        QCOMPARE(creator->findChild<QObject *>("newVmFirmware")->property("currentIndex").toInt(), 1);
        if (backend->management()["capabilities"].toMap()["tpm"].toBool()) QTRY_VERIFY(creator->findChild<QObject *>("newVmTpm")->property("checked").toBool());
        QVERIFY(capture("create-windows"));
        QVERIFY(QMetaObject::invokeMethod(creator, "close")); QTRY_VERIFY(!creator->property("visible").toBool());
        QFile::remove(files.filePath("iso-library/Win11_26H2_English_x64.iso")); isos->rescan();
        shop->setProperty("category", "all"); window->setProperty("navigation", "library");
        auto wizard = window->findChild<QObject *>("createVmDialog"); QVERIFY(wizard);
        QVERIFY(QMetaObject::invokeMethod(wizard, "begin"));
        QTRY_VERIFY(backend->management().contains("capabilities"));
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        QFile isoFixture(files.filePath("Library installer.ISO")); QVERIFY(isoFixture.open(QIODevice::WriteOnly)); isoFixture.write("UI media fixture"); isoFixture.close();
        QVERIFY(QMetaObject::invokeMethod(wizard, "setIsoFolder", Q_ARG(QVariant, files.path())));
        QTRY_COMPARE(wizard->property("images").toList().size(), 1);
        auto isoPicker = wizard->findChild<QObject *>("isoLibraryPicker"); QVERIFY(isoPicker);
        QVERIFY(QMetaObject::invokeMethod(isoPicker->property("popup").value<QObject *>(), "open")); QVERIFY(capture("iso-library"));
        QVERIFY(click(isoPicker->property("popup").value<QObject *>(), "selectOption_isoLibraryPicker_0"));
        QCOMPARE(wizard->findChild<QObject *>("newVmSource")->property("text").toString(), isoFixture.fileName());
        wizard->findChild<QObject *>("newVmSourceMode")->setProperty("currentIndex", 1);
        wizard->findChild<QObject *>("newVmName")->setProperty("text", "ui-" + QUuid::createUuid().toString(QUuid::Id128).left(8));
        wizard->findChild<QObject *>("newVmSource")->setProperty("text", source);
        // Keep all UI fixture storage inside its uniquely created temporary directory.
        auto caps = wizard->property("caps").toMap(); caps["storage"] = files.path(); wizard->setProperty("caps", caps);
        QVERIFY(capture("create-wizard"));
        auto sourceSelect = wizard->findChild<QObject *>("newVmSourceMode");
        QVERIFY(QMetaObject::invokeMethod(sourceSelect->property("popup").value<QObject *>(), "open"));
        QVERIFY(capture("source-options"));
        QVERIFY(QMetaObject::invokeMethod(sourceSelect->property("popup").value<QObject *>(), "close"));
        auto customLocation = wizard->findChild<QQuickItem *>("newVmLocation"); QVERIFY(customLocation);
        QVERIFY(!customLocation->isVisible()); QVERIFY(click(wizard, "createAdvancedToggle")); QTRY_VERIFY(customLocation->isVisible());
        customLocation->setProperty("text", files.path()); QVERIFY(capture("create-advanced"));
        QVERIFY(click(wizard, "createAdvancedToggle")); QTRY_VERIFY(!customLocation->isVisible()); QCOMPARE(customLocation->property("text").toString(), files.path());
        window->resize(940, 660); QVERIFY(capture("create-storage-small"));
        QVERIFY(capture("create-installation-small"));
        window->resize(1280, 840);
        QVERIFY(click(wizard, "editorSave")); QVERIFY(capture("create-review"));
        QVERIFY(click(wizard, "createPathsToggle")); QVERIFY(capture("create-review-paths")); QVERIFY(click(wizard, "createPathsToggle"));
        QVERIFY(click(wizard, "editorSave")); QTRY_VERIFY_WITH_TIMEOUT(!wizard->property("visible").toBool(), 30000);
        QVERIFY(!created.isEmpty()); auto uuid = created.last();
        QTRY_VERIFY(!backend->busy()); backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy());
        window->setProperty("detailsOpen", true);
        auto hardware = window->findChild<QObject *>("hardwareDialog"); QVERIFY(hardware);
        QVERIFY(QMetaObject::invokeMethod(hardware, "openFor", Q_ARG(QVariant, backend->details())));
        hardware->findChild<QObject *>("hardwareCpus")->setProperty("text", "2");
        hardware->findChild<QObject *>("hardwareMemory")->setProperty("text", "512");
        QVERIFY(capture("hardware-editor"));
        auto cpuMode = hardware->findChild<QQuickItem *>("hardwareCpuMode"); QVERIFY(cpuMode); QVERIFY(!cpuMode->isVisible());
        const auto savedCpuMode = cpuMode->property("currentText").toString();
        QVERIFY(click(hardware, "cpuAdvancedToggle")); QTRY_VERIFY(cpuMode->isVisible()); QVERIFY(capture("hardware-advanced"));
        QVERIFY(click(hardware, "cpuAdvancedToggle")); QVERIFY(!cpuMode->isVisible()); QCOMPARE(cpuMode->property("currentText").toString(), savedCpuMode);
        QVERIFY(click(hardware, "hardwareSection1")); QCOMPARE(hardware->property("configurationPage").toInt(), 1); QVERIFY(capture("hardware-boot"));
        QVERIFY(click(hardware, "hardwareSection2")); QCOMPARE(hardware->property("configurationPage").toInt(), 2); QVERIFY(capture("hardware-integration"));
        QVERIFY(click(hardware, "editorSave")); QVERIFY(capture("hardware-review")); QVERIFY(click(hardware, "editorSave"));
        QTRY_VERIFY(!hardware->property("visible").toBool()); QTRY_VERIFY(!backend->busy()); QCOMPARE(details(uuid)["memoryMiB"].toInt(), 512);
        auto inspectedPanel = window->findChild<QObject *>("vmDetails"); QVERIFY(inspectedPanel);
        inspectedPanel->setProperty("page", 1); QVERIFY(capture("hardware-page"));
        const auto firstDisk = backend->details()["disks"].toList().first().toMap()["target"].toString();
        QVERIFY(click(inspectedPanel, qPrintable("diskDetails_" + firstDisk + "Toggle"))); QVERIFY(capture("disk-details"));
        QVERIFY(click(inspectedPanel, qPrintable("diskDetails_" + firstDisk + "Toggle")));
        inspectedPanel->setProperty("page", 2); QVERIFY(capture("adapters-page"));
        auto adapterDialog = window->findChild<QObject *>("networkDialog"); QVERIFY(adapterDialog);
        QVERIFY(QMetaObject::invokeMethod(adapterDialog, "openFor", Q_ARG(QVariant, backend->details()), Q_ARG(QVariant, QVariantMap{}), Q_ARG(QVariant, false)));
        QVERIFY(capture("adapter-editor"));
        auto adapterModel = adapterDialog->findChild<QQuickItem *>("adapterModel"); QVERIFY(adapterModel); QVERIFY(!adapterModel->isVisible());
        QVERIFY(click(adapterDialog, "adapterAdvancedToggle")); QTRY_VERIFY(adapterModel->isVisible()); QVERIFY(capture("adapter-advanced"));
        QVERIFY(click(adapterDialog, "adapterAdvancedToggle")); QVERIFY(!adapterModel->isVisible());
        QVERIFY(QMetaObject::invokeMethod(adapterDialog, "reject"));
        inspectedPanel->setProperty("page", 0);
        window->setProperty("detailsOpen", false); QVERIFY(capture("workspace-stopped"));
        QVERIFY(click(window, "consoleToolsMenu", Qt::RightButton));
        auto consoleTools = window->findChild<QObject *>("consoleMenu"); QVERIFY(consoleTools);
        QTRY_VERIFY(consoleTools->property("opened").toBool());
        QVERIFY(capture("console-menu")); QVERIFY(click(consoleTools, "consoleKeysMenuEntry"));
        auto keysTools = window->findChild<QObject *>("consoleKeysMenu"); QVERIFY(keysTools); QTRY_VERIFY(keysTools->property("visible").toBool());
        QVERIFY(capture("console-keys-menu")); QVERIFY(QMetaObject::invokeMethod(keysTools, "close")); QVERIFY(QMetaObject::invokeMethod(consoleTools, "close"));
        QTRY_VERIFY(!consoleTools->property("visible").toBool());
        QVERIFY(click(window, "consoleClipboard"));
        auto clipboardTools = window->findChild<QObject *>("consoleClipboardPopup"); QVERIFY(clipboardTools); QTRY_VERIFY(clipboardTools->property("opened").toBool());
        QCOMPARE(clipboardTools->findChild<QObject *>("clipboardStatus")->property("text").toString(), "Needs setup");
        QVERIFY(!clipboardTools->findChild<QQuickItem *>("clipboardBoth")->isEnabled());
        QVERIFY(capture("console-clipboard-setup")); QVERIFY(QMetaObject::invokeMethod(clipboardTools, "close"));
        QTRY_VERIFY(!clipboardTools->property("visible").toBool());
        auto vmActions = window->findChild<QObject *>("powerMenu"); QVERIFY(vmActions);
        QVERIFY(QMetaObject::invokeMethod(vmActions, "open")); QVERIFY(capture("vm-actions")); QVERIFY(QMetaObject::invokeMethod(vmActions, "close"));
        auto appearance = window->findChild<QObject *>("appearanceInfo"); QVERIFY(appearance);
        QVERIFY(QMetaObject::invokeMethod(appearance, "open")); QVERIFY(capture("appearance")); QVERIFY(QMetaObject::invokeMethod(appearance, "close"));
        window->requestActivate(); QTRY_VERIFY(window->isActive());
        QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        auto palette = window->findChild<QObject *>("actionPalette"); QVERIFY(palette); QTRY_VERIFY(palette->property("opened").toBool());
        auto actionQuery = palette->findChild<QQuickItem *>("actionQuery"); QVERIFY(actionQuery); actionQuery->setProperty("text", "memory");
        QTRY_COMPARE(palette->property("matches").toList().size(), 1); QVERIFY(capture("action-search"));
        actionQuery->forceActiveFocus(); QTRY_VERIFY(actionQuery->hasActiveFocus()); QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(hardware->property("opened").toBool()); QVERIFY(!palette->property("visible").toBool());
        hardware->setProperty("failure", "The configuration changed while this editor was open. Refresh and review the saved configuration.");
        QVERIFY(capture("actionable-error"));
        auto errorDetails = hardware->findChild<QObject *>("errorDetails"); QVERIFY(errorDetails); QVERIFY(!errorDetails->property("expanded").toBool());
        QVERIFY(click(hardware, "errorDetailsToggle")); QTRY_VERIFY(errorDetails->property("expanded").toBool()); QVERIFY(capture("error-details"));
        QVERIFY(click(hardware, "errorRecovery")); QTRY_VERIFY(!hardware->property("visible").toBool());
        QTRY_VERIFY(!backend->detailsBusy()); QCOMPARE(backend->details()["uuid"].toString(), uuid);
        QCOMPARE(inspectedPanel->property("page").toInt(), 1);
        inspectedPanel->setProperty("page", 0);
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        auto console = window->findChild<Console *>("console"); QVERIFY(console); QTRY_VERIFY_WITH_TIMEOUT(console->hasFrame(), 15000);
        window->setProperty("detailsOpen", false);
        auto toolbar = window->findChild<QQuickItem *>("consoleTools"); QVERIFY(toolbar);
        auto fullscreenBar = window->findChild<QQuickItem *>("fullscreenToolbar"); QVERIFY(fullscreenBar);
        auto namingDialog = window->findChild<QObject *>("checkpointEditor"); QVERIFY(namingDialog);
        auto captureOn = [&](QQuickWindow *host, QString name) { QTest::qWait(200); auto dir = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR"); return dir.isEmpty() || host->grabWindow().save(dir + "/" + name + ".png"); };
        // One toolbar and one console move together; fullscreen leaves the frame
        // at a stable size while the controls reveal and hide above it.
        for (bool floating : {false, true}) {
            window->setProperty("consoleDetached", floating);
            auto host = floating ? window->findChild<QQuickWindow *>("detachedConsole") : window;
            QVERIFY(host); QTRY_COMPARE(console->window(), host); QTRY_COMPARE(toolbar->window(), host);
            host->requestActivate(); QTRY_VERIFY(host->isActive());
            QVERIFY(captureOn(host, floating ? "console-detached" : "console-embedded"));
            QVERIFY(click(window, "consoleSnapshot")); QTRY_VERIFY(namingDialog->property("opened").toBool());
            auto nameField = namingDialog->findChild<QQuickItem *>("checkpointName"); QVERIFY(nameField);
            QCOMPARE(nameField->window(), host); QVERIFY(nameField->property("text").toString().startsWith("Snapshot "));
            QVERIFY(!namingDialog->findChild<QQuickItem *>("checkpointMemory")->isVisible());
            QVERIFY(namingDialog->findChild<QObject *>("checkpointMemory")->property("checked").toBool());
            QVERIFY(captureOn(host, floating ? "console-name-detached" : "console-name"));
            QVERIFY(click(namingDialog, "editorCancel")); QTRY_VERIFY(!namingDialog->property("visible").toBool());
            QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, console->mapToScene({console->width()/2, console->height()/2}).toPoint());
            QTRY_VERIFY(console->captured()); QVERIFY(console->findChild<QQuickItem *>("consoleCaptureBorder")->isVisible());
            QVERIFY(captureOn(host, floating ? "console-captured-detached" : "console-captured"));
            QTest::keyClick(host, Qt::Key_Alt, Qt::ControlModifier); QTRY_VERIFY(!console->captured());
            QVERIFY(click(window, "consoleFullscreen")); QTRY_COMPARE(host->visibility(), QWindow::FullScreen);
            QTest::mouseMove(host, QPoint(host->width()/2, host->height()/2)); QTRY_VERIFY(!fullscreenBar->isVisible());
            QTRY_COMPARE(console->size(), QSizeF(host->width(), host->height()));
            const auto fullSize = console->size();
            QVERIFY(captureOn(host, floating ? "console-fullscreen-detached-hidden" : "console-fullscreen-hidden"));
            QTest::mouseMove(host, QPoint(host->width()/2, 3)); QTRY_VERIFY(fullscreenBar->isVisible());
            QCOMPARE(console->size(), fullSize);
            QVERIFY(captureOn(host, floating ? "console-fullscreen-detached-toolbar" : "console-fullscreen-toolbar"));
            QVERIFY(click(window, "consoleSnapshot")); QTRY_VERIFY(namingDialog->property("opened").toBool());
            QCOMPARE(nameField->window(), host); QTest::mouseMove(host, QPoint(host->width()/2, host->height()/2)); QTest::qWait(750);
            QVERIFY(fullscreenBar->isVisible()); QVERIFY(captureOn(host, floating ? "console-name-fullscreen-detached" : "console-name-fullscreen"));
            QVERIFY(click(namingDialog, "editorCancel")); QTRY_VERIFY(!namingDialog->property("visible").toBool());
            QTest::mouseMove(host, QPoint(host->width()/2, 3)); QTRY_VERIFY(fullscreenBar->isVisible());
            QVERIFY(click(window, "consoleClipboard")); QTRY_VERIFY(clipboardTools->property("opened").toBool());
            QTest::mouseMove(host, QPoint(host->width()/2, host->height()/2)); QTest::qWait(750);
            QVERIFY(fullscreenBar->isVisible());
            QVERIFY(QMetaObject::invokeMethod(clipboardTools, "close")); QTRY_VERIFY(!clipboardTools->property("visible").toBool());
            // Return focus to the guest and confirm the bar hides without taking input.
            QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, QPoint(host->width()/2, host->height()/2));
            QTRY_VERIFY(console->captured()); QTRY_VERIFY(!fullscreenBar->isVisible());
            QTest::keyClick(host, Qt::Key_Escape); QCOMPARE(host->visibility(), QWindow::FullScreen);
            QTest::keyClick(host, Qt::Key_Alt, Qt::ControlModifier); QTRY_VERIFY(!console->captured());
            QTest::mouseMove(host, QPoint(host->width()/2, 3)); QTRY_VERIFY(fullscreenBar->isVisible());
            QTest::keyClick(host, Qt::Key_Escape); QTRY_COMPARE(host->visibility(), QWindow::Windowed);
        }
        window->setProperty("consoleDetached", false); QTRY_COMPARE(console->window(), window);
        window->setProperty("detailsOpen", true); QTest::qWait(3100); QVERIFY(capture("overview-live"));
        auto technical = inspectedPanel->findChild<QQuickItem *>("vmTechnical"); QVERIFY(technical); QVERIFY(!technical->property("expanded").toBool());
        window->requestActivate(); QTRY_VERIFY(window->isActive());
        auto technicalToggle = technical->findChild<QQuickItem *>("vmTechnicalToggle"); QVERIFY(technicalToggle); QVERIFY(technicalToggle->isVisible()); technicalToggle->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(technicalToggle->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space); QTRY_VERIFY(technical->property("expanded").toBool()); QVERIFY(capture("overview-technical"));
        QTest::keyClick(window, Qt::Key_Space); QTRY_VERIFY(!technical->property("expanded").toBool());
        QCOMPARE(cpuMode->property("currentText").toString(), savedCpuMode);
        window->setProperty("navigation", "networks"); QTRY_VERIFY(backend->management().contains("networks.list"));
        auto networks = window->findChild<QObject *>("networksPage"); QVERIFY(networks);
        networks->setProperty("mapView", false); QVERIFY(capture("networks-manager"));
        networks->setProperty("mapView", true); QVERIFY(capture("network-map"));
        QVERIFY(!backend->management()["networks.list"].toMap()["topology"].toList().first().toMap()["liveInterfaces"].toList().isEmpty());
        {
            // Lab map: devices for the internet, this computer and the VM; the VM's NAT cable gives it internet reach.
            auto topology = networks->findChild<QQuickItem *>("networkTopology"); QVERIFY(topology);
            std::function<QQuickItem *(QQuickItem *, QString)> within = [&](QQuickItem *parent, QString name) -> QQuickItem * {
                if (!parent) return nullptr;
                if (parent->objectName() == name) return parent;
                for (auto child : parent->childItems()) if (auto match = within(child, name)) return match;
                return nullptr;
            };
            QTRY_VERIFY(within(topology, "topologyNode_internet")); QVERIFY(within(topology, "topologyNode_host"));
            QTRY_VERIFY(within(topology, "topologyNode_vm:" + uuid));
            QTRY_COMPARE(within(topology, "reach_vm:" + uuid)->property("text").toString(), QString("● INTERNET"));
            QVERIFY(within(topology, "topologySummary")->property("text").toString().contains("can reach the internet"));
            topology->setProperty("selected", "vm:" + uuid); QVERIFY(capture("network-map-selected"));
            // The kill switch pulls the cable live; plugging it back in restores reach.
            QSignalSpy links(backend.get(), &Backend::linksSet);
            QVERIFY(QMetaObject::invokeMethod(topology, "killInternet"));
            QTRY_VERIFY(!links.isEmpty()); QVERIFY2(links.last()[0].toBool(), qPrintable(links.last()[1].toString()));
            QTRY_COMPARE(within(topology, "reach_vm:" + uuid)->property("text").toString(), QString("● OFFLINE"));
            QVERIFY(!details(uuid, true)["interfaces"].toList().first().toMap()["linkUp"].toBool());
            QVERIFY(capture("network-map-killed"));
            const auto mac = details(uuid, true)["interfaces"].toList().first().toMap()["mac"].toString();
            links.clear(); backend->setLinks({QVariantMap{{"uuid", uuid}, {"mac", mac}}}, true);
            QTRY_VERIFY(!links.isEmpty()); QVERIFY(links.last()[0].toBool());
            QTRY_COMPARE(within(topology, "reach_vm:" + uuid)->property("text").toString(), QString("● INTERNET"));
            // Dragging the VM's port onto this computer offers a new adapter or moving the existing one.
            auto port = within(topology, "port_vm:" + uuid), hostNode = within(topology, "topologyNode_host"); QVERIFY(port); QVERIFY(hostNode);
            const auto from = port->mapToScene({port->width() / 2, port->height() / 2}).toPoint(), to = hostNode->mapToScene({hostNode->width() / 2, hostNode->height() / 2}).toPoint();
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
            for (int step = 1; step <= 12; ++step) { QTest::mouseMove(window, from + (to - from) * step / 12); QTest::qWait(16); }
            QTRY_COMPARE(topology->property("wire").toMap()["over"].toString(), QString("host"));
            QVERIFY(capture("network-map-wiring"));
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
            auto dropMenu = topology->findChild<QObject *>("topologyDropMenu"); QVERIFY(dropMenu);
            QTRY_VERIFY(dropMenu->property("opened").toBool()); QVERIFY(topology->property("wire").isNull() || !topology->property("wire").toMap().size());
            QVERIFY(capture("network-map-drop"));
            QVERIFY(QMetaObject::invokeMethod(dropMenu, "close")); QTRY_VERIFY(!dropMenu->property("visible").toBool());
            topology->setProperty("selected", "host"); QVERIFY(capture("network-map-host"));
            topology->setProperty("selected", "");
            // A second VM on its own private internet connection stacks below the first; its cable goes around.
            backend->createTest(); QTRY_VERIFY(!backend->busy()); const auto second = created.last();
            QSignalSpy configured(backend.get(), &Backend::networkConfigured);
            QVERIFY(backend->configureNetwork(second, "", "user", "virtio", true, false, DomainConfig::revision(xml(second))));
            QTRY_VERIFY(!configured.isEmpty()); QVERIFY(configured.last()[1].toBool()); QTRY_VERIFY(!backend->busy());
            QVERIFY(QMetaObject::invokeMethod(networks, "refresh"));
            QTRY_VERIFY(within(topology, "topologyNode_vm:" + second));
            QVERIFY(QMetaObject::invokeMethod(topology, "arrange")); QVERIFY(capture("network-map-two"));
            topology->setProperty("selected", "vm:" + second); QVERIFY(capture("network-map-two-selected"));
            topology->setProperty("selected", "");
            // Creating the VM selected it; go back to the VM the rest of this test works on.
            window->setProperty("selectedUuid", uuid); window->setProperty("navigation", "networks");
            QTRY_COMPARE(backend->details()["uuid"].toString(), uuid); QTRY_VERIFY(!backend->detailsBusy());
        }
        auto networkEditor = window->findChild<QObject *>("hostNetworkEditor"); QVERIFY(networkEditor);
        // The new-network dialog offers three plain choices and can connect VMs straight away.
        const QVariant withVm = QVariantMap{{"vms", QStringList{uuid}}};
        QVERIFY(QMetaObject::invokeMethod(networkEditor, "openFor", Q_ARG(QVariant, withVm))); QVERIFY(capture("create-network"));
        QCOMPARE(networkEditor->property("modeIndex").toInt(), 0);
        QVERIFY(networkEditor->property("chosenVms").toMap().contains(uuid));
        QVERIFY(click(networkEditor, "networkMode_isolated")); QTRY_COMPARE(networkEditor->property("modeIndex").toInt(), 2); QVERIFY(capture("create-network-isolated"));
        QVERIFY(click(networkEditor, "networkMode_nat")); QTRY_COMPARE(networkEditor->property("modeIndex").toInt(), 0);
        QVERIFY(click(networkEditor, "networkAddressesToggle")); QVERIFY(capture("network-address-settings"));
        QVERIFY(QMetaObject::invokeMethod(networkEditor, "reject"));
        window->resize(940, 660); QVERIFY(capture("network-map-small"));
        window->setProperty("navigation", "library"); window->setProperty("compactLibrary", true); theme.setMode("light"); QVERIFY(capture("overview-small-light"));
        auto organize = window->findChild<QObject *>("organizeDialog"); QVERIFY(organize);
        QVERIFY(QMetaObject::invokeMethod(organize, "openFor", Q_ARG(QVariant, backend->details())));
        organize->findChild<QObject *>("vmAlias")->setProperty("text", "Ubuntu workspace"); organize->findChild<QObject *>("vmFolder")->setProperty("text", "Development");
        organize->findChild<QObject *>("vmFavorite")->setProperty("checked", true); QVERIFY(capture("organize-vm")); QVERIFY(click(organize, "editorSave"));
        Workspace reopened; QCOMPARE(reopened.vm(uuid)["alias"].toString(), "Ubuntu workspace"); QVERIFY(reopened.vm(uuid)["favorite"].toBool());
        command("hardware.save", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"cpus", 1}, {"memoryMiB", 768}}); QVERIFY(resultOk);
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy());
        auto pending = window->findChild<QObject *>("pendingDialog"); QVERIFY(pending);
        QVERIFY(QMetaObject::invokeMethod(pending, "openFor", Q_ARG(QVariant, backend->details()))); QVERIFY(capture("pending-settings")); QVERIFY(QMetaObject::invokeMethod(pending, "close"));
        command("pending.discard", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"key", "all"}}); QVERIFY(resultOk);
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy());
        auto vmDetails = window->findChild<QObject *>("vmDetails"); QVERIFY(vmDetails);
        QVERIFY(click(window, "snapshotsTab")); QCOMPARE(vmDetails->property("page").toInt(), 3);
        auto snapshotPage = window->findChild<QObject *>("snapshotsPage"); QVERIFY(snapshotPage);
        QTRY_VERIFY(snapshotPage->property("ready").toBool());
        QVERIFY(capture("snapshot-empty"));
        QVERIFY(click(vmDetails, "snapshotPageMore")); QVERIFY(click(vmDetails, "snapshotHelp")); auto snapshotHelp = window->findChild<QObject *>("snapshotHelpDialog"); QVERIFY(snapshotHelp);
        QTRY_VERIFY(snapshotHelp->property("visible").toBool()); QVERIFY(capture("snapshot-help")); QVERIFY(QMetaObject::invokeMethod(snapshotHelp, "reject"));
        command("snapshots.create", {{"uuid", uuid}, {"name", "Ready to install"}, {"notes", "Hardware and network configured"}}); QVERIFY(resultOk);
        QTRY_VERIFY(backend->management().contains("snapshots.list")); QVERIFY(capture("checkpoints"));
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy());
        command("snapshots.list", {{"uuid", uuid}}); QVERIFY(resultOk);
        // The primary action captures immediately with a unique name and preview.
        QTRY_VERIFY_WITH_TIMEOUT(console->hasFrame(), 15000);
        QVERIFY(click(vmDetails, "newSnapshot"));
        auto checkpointEditor = window->findChild<QObject *>("checkpointEditor"); QVERIFY(checkpointEditor);
        QVERIFY(!checkpointEditor->property("visible").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(Checkpoints::list(uuid).size() == 2 && !backend->busy(), 30000);
        QVERIFY(active(uuid));
        const auto quickSnapshot = Checkpoints::list(uuid).last().toMap();
        QVERIFY(!quickSnapshot["memory"].toString().isEmpty());
        QVERIFY(quickSnapshot["name"].toString().startsWith("Snapshot "));
        QVERIFY(!quickSnapshot["previewUrl"].toString().isEmpty());
        QTRY_COMPARE(snapshotPage->property("selectedSnapshot").toMap()["id"].toString(), quickSnapshot["id"].toString());
        QVERIFY(click(vmDetails, "snapshotDetailsButton")); auto snapshotDetails = window->findChild<QObject *>("snapshotDetailsDialog"); QVERIFY(snapshotDetails); QTRY_VERIFY(snapshotDetails->property("opened").toBool()); QVERIFY(capture("snapshot-details"));
        QVERIFY(click(snapshotDetails, "snapshotPreview")); auto previewDialog = window->findChild<QObject *>("snapshotPreviewDialog"); QVERIFY(previewDialog);
        QTRY_VERIFY(previewDialog->property("visible").toBool()); QVERIFY(capture("snapshot-preview")); QVERIFY(QMetaObject::invokeMethod(previewDialog, "reject")); QTRY_VERIFY(!previewDialog->property("visible").toBool());
        auto renameGraph = window->findChild<QQuickItem *>("snapshotBranchMap"); QVERIFY(renameGraph);
        window->requestActivate(); QTRY_VERIFY(window->isActive()); renameGraph->forceActiveFocus(); QTRY_VERIFY(renameGraph->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_F2); QTRY_COMPARE(renameGraph->property("renamingId").toString(), quickSnapshot["id"].toString());
        QTRY_VERIFY(window->activeFocusItem() && window->activeFocusItem()->objectName().startsWith("renameSnapshot_"));
        window->activeFocusItem()->setProperty("text", "Quick baseline"); QVERIFY(capture("snapshot-inline-rename")); QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 10000); QTRY_COMPARE(snapshotPage->property("selectedSnapshot").toMap()["name"].toString(), "Quick baseline");
        QVERIFY(renameGraph->property("renamingId").toString().isEmpty());
        QVERIFY(click(vmDetails, "snapshotOptions"));
        QVERIFY(!checkpointEditor->property("advanced").toBool());
        checkpointEditor->findChild<QObject *>("checkpointName")->setProperty("text", "");
        QVERIFY(!checkpointEditor->findChild<QQuickItem *>("editorSave")->isEnabled());
        checkpointEditor->findChild<QObject *>("checkpointName")->setProperty("text", "Live from the UI");
        checkpointEditor->findChild<QObject *>("checkpointThumbnail")->setProperty("checked", true);
        QVERIFY(capture("live-checkpoint-dialog"));
        QVERIFY(click(checkpointEditor, "snapshotAdvanced")); QVERIFY(checkpointEditor->property("advanced").toBool());
        QVERIFY(capture("snapshot-options")); QVERIFY(click(checkpointEditor, "snapshotAdvanced"));
        QVERIFY(click(checkpointEditor, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!checkpointEditor->property("visible").toBool(), 30000); QVERIFY(active(uuid));
        command("snapshots.list", {{"uuid", uuid}}); QVERIFY(resultOk); QVERIFY(!Checkpoints::list(uuid).last().toMap()["previewUrl"].toString().isEmpty()); QVERIFY(capture("live-checkpoints"));
        QTRY_COMPARE(snapshotPage->property("selectedSnapshot").toMap()["name"].toString(), "Live from the UI");
        QCOMPARE(snapshotPage->property("selectedIndex").toInt(), 2);
        const auto beforeRestore = domainId(uuid);
        QSignalSpy reopenedConsole(backend.get(), &Backend::graphics);
        // Selecting a saved point only browses; Go to changes the working parent.
        QTRY_VERIFY(snapshotPage->property("selectedIndex").toInt() >= 0);
        while (snapshotPage->property("selectedIndex").toInt() > 0) {
            const int previous = snapshotPage->property("selectedIndex").toInt();
            renameGraph->forceActiveFocus(); QTest::keyClick(window, Qt::Key_Left);
            QTRY_COMPARE(snapshotPage->property("selectedIndex").toInt(), previous - 1);
        }
        QCOMPARE(snapshotPage->property("selectedSnapshot").toMap()["name"].toString(), "Ready to install");
        QCOMPARE(domainId(uuid), beforeRestore); // Browsing never changes the VM.
        QVERIFY(click(vmDetails, "restoreCheckpoint"));
        // A disk-only snapshot of a running VM honestly needs a boot; there is no recovery-point option.
        QCOMPARE(checkpointEditor->property("actionText").toString(), "Revert & reboot");
        QVERIFY(!checkpointEditor->findChild<QObject *>("checkpointSafety"));
        QVERIFY(checkpointEditor->findChild<QObject *>("checkpointExplanation")->property("text").toString().contains("unsaved work"));
        QVERIFY(capture("running-restore-dialog"));
        QVERIFY(click(checkpointEditor, "editorCancel")); QTRY_VERIFY(!checkpointEditor->property("visible").toBool());
        QCOMPARE(domainId(uuid), beforeRestore); QVERIFY(!backend->busy());
        QVERIFY(click(vmDetails, "restoreCheckpoint")); QVERIFY(click(checkpointEditor, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!checkpointEditor->property("visible").toBool(), 30000); QVERIFY(active(uuid));
        QVERIFY(domainId(uuid) != beforeRestore); QTRY_VERIFY(!reopenedConsole.isEmpty()); QTRY_VERIFY(console->hasFrame());
        QTRY_VERIFY(!backend->busy()); QVERIFY(capture("running-restore-complete"));
        QTRY_COMPARE(snapshotPage->property("selectedSnapshot").toMap()["name"].toString(), "Ready to install");
        QVERIFY(Checkpoints::undoId(uuid).isEmpty()); // Reverting from the UI never saves a recovery point.
        {
            // Restored disks are thin overlays whose backing file is the snapshot's own image.
            const auto restoredId = snapshotPage->property("selectedSnapshot").toMap()["id"].toString();
            const auto disk = details(uuid)["disks"].toList().first().toMap()["source"].toString();
            QVERIFY(disk.contains("/restore-"));
            QByteArray chain; QVERIFY(process("qemu-img", {"info", "--output=json", "--backing-chain", "-U", disk}, &chain));
            QVERIFY(QString::fromUtf8(chain).contains("/checkpoints/" + uuid + "/" + restoredId + "/"));
            // Undo remains available for recovery points made through the API.
            const auto recovery = command("snapshots.restore", {{"uuid", uuid}, {"id", restoredId}, {"allowRestart", true}, {"safety", true}});
            QVERIFY2(resultOk, qPrintable(recovery["message"].toString())); QVERIFY(!Checkpoints::undoId(uuid).isEmpty());
            QTRY_VERIFY(!backend->busy()); QTRY_VERIFY(console->hasFrame());
        }
        window->resize(1280,820); theme.setMode("dark"); QVERIFY(capture("snapshot-timeline-dark"));
        QVERIFY(click(vmDetails,"snapshotPageMore")); QVERIFY(click(vmDetails,"checkpointStorage")); auto storage=window->findChild<QObject *>("checkpointStorageDialog"); QVERIFY(storage); QVERIFY(capture("checkpoint-storage")); QVERIFY(QMetaObject::invokeMethod(storage,"close"));
        auto jobs=window->findChild<QObject *>("checkpointJobs"); QVERIFY(jobs); QVERIFY(QMetaObject::invokeMethod(jobs,"open")); QVERIFY(capture("checkpoint-jobs")); QVERIFY(QMetaObject::invokeMethod(jobs,"close"));
        QVERIFY(click(vmDetails,"snapshotMore")); QVERIFY(click(vmDetails,"editCheckpoint")); QVERIFY(capture("checkpoint-edit")); QVERIFY(QMetaObject::invokeMethod(checkpointEditor,"close"));
        theme.setMode("light"); window->resize(940,660); QVERIFY(capture("snapshot-timeline-small-light"));
        auto restoreButton = vmDetails->findChild<QQuickItem *>("restoreCheckpoint"); QVERIFY(restoreButton);
        auto detailsScroll = vmDetails->findChild<QQuickItem *>("detailsScroll"); QVERIFY(detailsScroll);
        const auto restoreBounds = restoreButton->mapRectToItem(detailsScroll, restoreButton->boundingRect());
        QVERIFY2(detailsScroll->boundingRect().contains(restoreBounds), "Restore must remain fully visible at the minimum window size");
        const auto recoveryId = Checkpoints::undoId(uuid);
        const auto beforeUndo = domainId(uuid);
        QVERIFY(click(vmDetails, "snapshotPageMore")); QVERIFY(click(vmDetails, "undoCheckpoint")); QCOMPARE(checkpointEditor->property("verb").toString(), "undo");
        QVERIFY(click(checkpointEditor, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!checkpointEditor->property("visible").toBool(), 30000);
        QVERIFY(active(uuid)); QVERIFY(domainId(uuid) != beforeUndo);
        QTRY_COMPARE(snapshotPage->property("selectedSnapshot").toMap()["id"].toString(), recoveryId);
        QTRY_VERIFY(!backend->busy()); QTRY_VERIFY(console->hasFrame());
        // A -> B, Go to A, capture C: B and C become siblings without creating a named branch.
        window->resize(1280, 900); theme.setMode("dark"); detailsScroll->setProperty("contentY", 0);
        QVERIFY(QMetaObject::invokeMethod(snapshotPage, "selectSnapshot", Q_ARG(QVariant, 0)));
        const auto baseId = snapshotPage->property("selectedSnapshot").toMap()["id"].toString();
        const auto beforeBranch = domainId(uuid);
        QVERIFY(click(vmDetails, "restoreCheckpoint"));
        QVERIFY(click(checkpointEditor, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!checkpointEditor->property("visible").toBool(), 30000);
        QVERIFY(domainId(uuid) != beforeBranch); QCOMPARE(Checkpoints::history(uuid)["currentId"].toString(), baseId);
        QTRY_VERIFY(!backend->busy()); QTRY_VERIFY(console->hasFrame());
        QVERIFY(click(vmDetails, "snapshotOptions"));
        checkpointEditor->findChild<QObject *>("checkpointName")->setProperty("text", "Try new tools");
        QVERIFY(click(checkpointEditor, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!checkpointEditor->property("visible").toBool(), 30000);
        QTRY_COMPARE(snapshotPage->property("selectedSnapshot").toMap()["name"].toString(), "Try new tools");
        const auto forkId = snapshotPage->property("selectedSnapshot").toMap()["id"].toString();
        QCOMPARE(snapshotPage->property("selectedSnapshot").toMap()["parentId"].toString(), baseId);
        auto graph = vmDetails->findChild<QObject *>("snapshotBranchMap"); QVERIFY(graph);
        auto graphNode = [&](QString id) {
            const auto layout = graph->property("layout").value<QJSValue>().toVariant().toMap();
            for (const auto &v : layout["nodes"].toList()) if (v.toMap()["id"] == id) return v.toMap();
            return QVariantMap{};
        };
        QTRY_COMPARE(graphNode("__working__")["parentId"].toString(), forkId);
        QCOMPARE(graphNode(quickSnapshot["id"].toString())["x"], graphNode(forkId)["x"]);
        QVERIFY(graphNode(quickSnapshot["id"].toString())["y"] != graphNode(forkId)["y"]);
        for (const auto &saved : Checkpoints::list(uuid)) QVERIFY(!graphNode(saved.toMap()["id"].toString()).isEmpty());
        QVERIFY(graphNode(quickSnapshot["id"].toString())["branch"] != graphNode(forkId)["branch"]);
        std::function<QQuickItem *(QQuickItem *, QString)> visualItem = [&](QQuickItem *parent, QString name) -> QQuickItem * {
            if (parent->objectName() == name) return parent;
            for (auto child : parent->childItems()) if (auto found = visualItem(child, name)) return found;
            return nullptr;
        };
        auto firstBranchCard = visualItem(window->contentItem(), "snapshotNode_" + quickSnapshot["id"].toString()); QVERIFY(firstBranchCard);
        auto forkCard = visualItem(window->contentItem(), "snapshotNode_" + forkId); QVERIFY(forkCard);
        QVERIFY(!firstBranchCard->property("currentSaved").toBool()); QVERIFY(forkCard->property("currentSaved").toBool());
        QCOMPARE(visualItem(window->contentItem(), "snapshotState_" + forkId)->property("text").toString(), "CURRENT SNAPSHOT");
        QVERIFY(vmDetails->findChild<QObject *>("currentSnapshotLabel")->property("text").toString().contains("Try new tools"));
        QVERIFY(!vmDetails->findChild<QObject *>("focusSnapshotPath"));
        QVERIFY(capture("snapshot-tree-dark"));
        auto mapViewport = vmDetails->findChild<QQuickItem *>("branchMapViewport"); QVERIFY(mapViewport);
        mapViewport->setProperty("contentX", 50.0);
        command("snapshots.list", {{"uuid", uuid}}); QVERIFY(resultOk); QTest::qWait(200);
        QCOMPARE(mapViewport->property("contentX").toDouble(), 50.0);
        QVERIFY(click(vmDetails, "fitBranches")); QVERIFY(capture("snapshot-tree-fit"));
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + quickSnapshot["id"].toString())));
        QTRY_COMPARE(snapshotPage->property("selectedKey").toString(), quickSnapshot["id"].toString());
        QCOMPARE(Checkpoints::history(uuid)["currentId"].toString(), forkId);
        QVERIFY(visualItem(window->contentItem(), "snapshotNode_" + forkId)->property("currentSaved").toBool());
        QVERIFY(!visualItem(window->contentItem(), "snapshotNode_" + quickSnapshot["id"].toString())->property("currentSaved").toBool());
        QCOMPARE(graphNode("__working__")["parentId"].toString(), forkId); // selection is not current state
        // Capture while browsing B still extends C, where the VM actually is.
        const auto beforeCapture = Checkpoints::list(uuid).size(); QVERIFY(click(vmDetails, "newSnapshot"));
        QTRY_VERIFY_WITH_TIMEOUT(Checkpoints::list(uuid).size() == beforeCapture + 1 && !backend->busy(), 30000);
        QTRY_COMPARE(snapshotPage->property("selectedSnapshot").toMap()["parentId"].toString(), forkId);
        const auto newestId = snapshotPage->property("selectedSnapshot").toMap()["id"].toString();
        QVERIFY(click(vmDetails, "locateWorkingState")); QTRY_VERIFY(snapshotPage->property("workingSelected").toBool());
        const auto parentBeforeRevert = graphNode("__working__")["parentId"].toString();
        QVERIFY(click(vmDetails, "revertSnapshot")); QCOMPARE(checkpointEditor->property("checkpointId").toString(), newestId);
        // Click Cancel only once the dialog is fully open and laid out (as a person would), then make sure nothing happened.
        QTRY_VERIFY(checkpointEditor->property("opened").toBool()); QTest::qWait(250);
        QVERIFY(click(checkpointEditor, "editorCancel"));
        QTRY_VERIFY(!checkpointEditor->property("visible").toBool());
        QTest::qWait(500); QVERIFY(!backend->busy()); QCOMPARE(graphNode("__working__")["parentId"].toString(), parentBeforeRevert);
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + quickSnapshot["id"].toString())));
        QTRY_COMPARE(snapshotPage->property("selectedKey").toString(), quickSnapshot["id"].toString());
        QVERIFY(click(vmDetails, "restoreCheckpoint"));
        QTRY_VERIFY(checkpointEditor->property("opened").toBool()); QTest::qWait(250);
        QCOMPARE(checkpointEditor->property("checkpointId").toString(), quickSnapshot["id"].toString());
        QVERIFY(click(checkpointEditor, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!checkpointEditor->property("visible").toBool(), 30000);
        QTRY_VERIFY2(graphNode("__working__")["parentId"].toString() == quickSnapshot["id"].toString(),
            qPrintable("restore: " + window->property("operationError").toString() + " | log: " + backend->activity().value(0).toMap()["message"].toString() + " / " + backend->activity().value(1).toMap()["message"].toString() + " / " + backend->activity().value(2).toMap()["message"].toString()
                + " | on disk: " + [&] { const auto want = Checkpoints::history(uuid)["currentId"].toString(); for (auto v : Checkpoints::list(uuid)) if (v.toMap()["id"] == want) return v.toMap()["name"].toString(); return want; }()
                + " | quick: " + quickSnapshot["name"].toString()
                + " | working parent: " + [&] { const auto want = graphNode("__working__")["parentId"].toString(); for (auto v : Checkpoints::list(uuid)) if (v.toMap()["id"] == want) return v.toMap()["name"].toString() + " (" + v.toMap()["tags"].toString() + ")"; return want; }()));
        // Console Revert follows the working parent, not the newest recovery or
        // the most recently created node on the other branch.
        QVERIFY(click(window, "consoleTab")); QTRY_VERIFY(!window->property("detailsOpen").toBool());
        QVERIFY(capture("console-snapshot-actions")); QVERIFY(click(window, "consoleRevert"));
        QTRY_VERIFY(checkpointEditor->property("opened").toBool());
        QCOMPARE(checkpointEditor->property("checkpointId").toString(), quickSnapshot["id"].toString());
        QCOMPARE(checkpointEditor->property("actionText").toString(), "Revert");
        QVERIFY(!window->property("detailsOpen").toBool()); QVERIFY(capture("console-revert-memory"));
        QVERIFY(click(checkpointEditor, "editorCancel")); QTRY_VERIFY(!checkpointEditor->property("visible").toBool());
        window->setProperty("consoleDetached", true);
        auto revertHost = console->window(); revertHost->requestActivate(); QTRY_VERIFY(revertHost->isActive());
        QVERIFY(click(window, "consoleRevert")); QTRY_VERIFY(checkpointEditor->property("opened").toBool());
        QCOMPARE(checkpointEditor->findChild<QQuickItem *>("checkpointName")->window(), revertHost);
        QCOMPARE(checkpointEditor->property("checkpointId").toString(), quickSnapshot["id"].toString());
        QVERIFY(captureOn(revertHost, "console-revert-detached")); QVERIFY(click(checkpointEditor, "editorCancel")); QTRY_VERIFY(!checkpointEditor->property("visible").toBool());
        window->setProperty("consoleDetached", false); window->requestActivate(); QTRY_VERIFY(window->isActive());
        QVERIFY(click(window, "snapshotsTab"));
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + quickSnapshot["id"].toString())));
        QVERIFY(!graphNode(forkId).isEmpty()); QVERIFY(capture("snapshot-tree-returned"));
        theme.setMode("light"); window->resize(940, 660); QVERIFY(capture("snapshot-tree-small-light"));
        QVERIFY(detailsScroll->boundingRect().contains(restoreButton->mapRectToItem(detailsScroll, restoreButton->boundingRect())));
        QVERIFY(click(window, "detailsTab")); QVERIFY(click(window, "snapshotsTab"));
        QTRY_VERIFY(snapshotPage->property("workingSelected").toBool());
        QVERIFY(click(vmDetails, "fitBranches")); // readable scale must still reveal the current state
        QTest::qWait(200);
        const auto workingNode = graphNode("__working__");
        const QRectF workingBounds(workingNode["x"].toDouble() - mapViewport->property("contentX").toDouble(), workingNode["y"].toDouble() - mapViewport->property("contentY").toDouble(), graph->property("cardWidth").toDouble(), graph->property("cardHeight").toDouble());
        QVERIFY(mapViewport->boundingRect().contains(workingBounds));
        QVERIFY(capture("snapshot-tree-working-state"));
        // Re-enter Snapshots after scrolling other pages, including switches
        // while details and history refresh asynchronously.
        auto graphItem = qobject_cast<QQuickItem *>(graph); QVERIFY(graphItem);
        for (int cycle = 0; cycle < 8; ++cycle) {
            QVERIFY(click(window, "detailsTab"));
            vmDetails->setProperty("page", cycle % 3);
            detailsScroll->setProperty("contentY", detailsScroll->property("contentHeight").toDouble());
            if (cycle % 2) QVERIFY(click(window, "consoleTab"));
            backend->inspect(uuid);
            QVERIFY(click(window, "snapshotsTab"));
            QTRY_VERIFY(snapshotPage->property("ready").toBool());
            QTRY_VERIFY(graphItem->isVisible() && graphItem->width() > 100 && graphItem->height() > 100);
            QTRY_VERIFY(detailsScroll->boundingRect().contains(graphItem->mapRectToItem(detailsScroll, graphItem->boundingRect())));
            QTRY_VERIFY(visualItem(window->contentItem(), "snapshotNode___working__") && visualItem(window->contentItem(), "snapshotNode___working__")->isVisible());
            auto parentCard = visualItem(window->contentItem(), "snapshotNode_" + quickSnapshot["id"].toString()); QVERIFY(parentCard);
            QTRY_VERIFY(mapViewport->boundingRect().contains(parentCard->mapRectToItem(mapViewport, parentCard->boundingRect())));
        }
        QVERIFY(capture("snapshot-after-tab-switches"));
        // Right-click targets the card under the pointer, independently of the
        // previously selected card and the actual current snapshot.
        graph->setProperty("fit", true); theme.setMode("dark");
        auto contextMenu = window->findChild<QObject *>("snapshotContextMenu"); QVERIFY(contextMenu);
        QVERIFY(click(vmDetails, "locateWorkingState"));
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + quickSnapshot["id"].toString()), Qt::RightButton));
        QTRY_VERIFY(contextMenu->property("opened").toBool());
        QCOMPARE(snapshotPage->property("contextKey").toString(), quickSnapshot["id"].toString());
        QCOMPARE(Checkpoints::history(uuid)["currentId"].toString(), quickSnapshot["id"].toString());
        QVERIFY(capture("snapshot-parent-context"));
        auto deleteButton = contextMenu->findChild<QQuickItem *>("deleteSnapshot"); QVERIFY(deleteButton);
        QVERIFY(window->contentItem()->boundingRect().contains(deleteButton->mapRectToItem(window->contentItem(), deleteButton->boundingRect())));
        QVERIFY(click(contextMenu, "deleteSnapshot")); auto deletion = window->findChild<QObject *>("deleteSnapshotDialog"); QVERIFY(deletion);
        QTRY_VERIFY(deletion->property("opened").toBool());
        QVERIFY(deletion->findChild<QObject *>("deleteOnlySnapshot")->property("checked").toBool());
        QCOMPARE(deletion->property("affected").toList().size(), 1);
        QCOMPARE(deletion->property("snapshot").toMap()["id"].toString(), quickSnapshot["id"].toString());
        QVERIFY(capture("delete-only-dialog")); const auto deleteBefore = Checkpoints::list(uuid).size(); const auto deleteRuntime = domainId(uuid);
        QVERIFY(click(deletion, "editorCancel")); QTRY_VERIFY(!deletion->property("visible").toBool()); QCOMPARE(Checkpoints::list(uuid).size(), deleteBefore);
        // Keyboard access reaches the same menu and preserves selection.
        graphItem->forceActiveFocus(); QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(contextMenu->property("opened").toBool()); QVERIFY(click(contextMenu, "deleteSnapshot"));
        QVERIFY(click(deletion, "editorSave")); QTRY_VERIFY_WITH_TIMEOUT(!deletion->property("visible").toBool(), 30000);
        QTRY_COMPARE(Checkpoints::list(uuid).size(), deleteBefore - 1); QTRY_VERIFY(graphNode(quickSnapshot["id"].toString()).isEmpty());
        QVERIFY(!graphNode(forkId).isEmpty()); QCOMPARE(domainId(uuid), deleteRuntime); QTRY_VERIFY(!backend->busy());
        window->requestActivate(); QTRY_VERIFY(window->isActive());
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + forkId)));
        QTRY_COMPARE(snapshotPage->property("selectedKey").toString(), forkId);
        QTRY_VERIFY(graph->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_Delete); QTRY_VERIFY(deletion->property("visible").toBool());
        QVERIFY(click(deletion, "editorCancel")); QTRY_VERIFY(!deletion->property("visible").toBool());
        // Pinned snapshots inside a branch require explicit confirmation.
        QString forkName; for (const auto &v : Checkpoints::list(uuid)) if (v.toMap()["id"].toString() == forkId) forkName = v.toMap()["name"].toString();
        const auto pinning = command("snapshots.edit", {{"uuid", uuid}, {"id", forkId}, {"name", forkName}, {"pinned", true}}); QVERIFY2(resultOk, qPrintable(pinning["message"].toString()));
        command("snapshots.list", {{"uuid", uuid}}); QVERIFY(resultOk);
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + forkId), Qt::RightButton));
        QTRY_VERIFY(contextMenu->property("opened").toBool());
        QCOMPARE(snapshotPage->property("contextKey").toString(), forkId);
        QVERIFY(click(contextMenu, "deleteSnapshotBranch")); QTRY_VERIFY(deletion->property("opened").toBool());
        QVERIFY(deletion->findChild<QObject *>("deleteWholeBranch")->property("checked").toBool());
        QVERIFY(deletion->property("pinnedCount").toInt() > 0);
        QVERIFY(!deletion->findChild<QQuickItem *>("editorSave")->isEnabled());
        QVERIFY(click(deletion, "deletePinnedSnapshots")); QVERIFY(deletion->findChild<QQuickItem *>("editorSave")->isEnabled());
        QVERIFY(capture("delete-branch-dialog")); QVERIFY(click(deletion, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!deletion->property("visible").toBool(), 30000); QTRY_VERIFY(graphNode(forkId).isEmpty()); QTRY_VERIFY(graphNode(newestId).isEmpty());
        QVERIFY(!graphNode(baseId).isEmpty()); QCOMPARE(domainId(uuid), deleteRuntime); QVERIFY(active(uuid)); QVERIFY(Checkpoints::undoId(uuid).isEmpty());
        // A leaf offers only single-snapshot deletion, including the pin guard.
        QTRY_VERIFY(!graphNode(recoveryId).isEmpty());
        const auto beforeLeafDelete = Checkpoints::list(uuid).size();
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + recoveryId), Qt::RightButton));
        QTRY_VERIFY(contextMenu->property("opened").toBool());
        QVERIFY(!contextMenu->findChild<QQuickItem *>("deleteSnapshotBranch")->isVisible());
        QVERIFY(capture("snapshot-child-context")); QVERIFY(click(contextMenu, "deleteSnapshot"));
        QTRY_VERIFY(deletion->property("opened").toBool());
        QCOMPARE(deletion->property("snapshot").toMap()["id"].toString(), recoveryId);
        QVERIFY(!deletion->findChild<QQuickItem *>("editorSave")->isEnabled());
        QVERIFY(click(deletion, "deletePinnedSnapshots")); QVERIFY(click(deletion, "editorSave"));
        QTRY_VERIFY_WITH_TIMEOUT(!deletion->property("visible").toBool(), 30000);
        QTRY_COMPARE(Checkpoints::list(uuid).size(), beforeLeafDelete - 1); QTRY_VERIFY(graphNode(recoveryId).isEmpty());
        QCOMPARE(Checkpoints::history(uuid)["currentId"].toString(), baseId); QCOMPARE(domainId(uuid), deleteRuntime);
        // The working marker is not a saved snapshot and cannot be deleted.
        QVERIFY(click(vmDetails, "snapshotNode___working__", Qt::RightButton));
        QTRY_VERIFY(contextMenu->property("opened").toBool());
        QVERIFY(!deleteButton->isVisible()); QVERIFY(contextMenu->findChild<QQuickItem *>("contextCapture")->isVisible());
        QVERIFY(capture("snapshot-working-context")); QVERIFY(QMetaObject::invokeMethod(contextMenu, "close"));
        QTRY_VERIFY(!contextMenu->property("visible").toBool());
        QVERIFY(capture("snapshot-after-delete"));
        QVERIFY(click(vmDetails, qPrintable("snapshotNode_" + baseId)));
        QVERIFY(QMetaObject::invokeMethod(appearance, "open"));
        QTRY_VERIFY(appearance->property("opened").toBool());
        auto textSize = appearance->findChild<QObject *>("textSize"); QVERIFY(textSize);
        QVERIFY(QMetaObject::invokeMethod(textSize->property("popup").value<QObject *>(), "open"));
        QTRY_VERIFY(textSize->property("popup").value<QObject *>()->property("opened").toBool());
        QVERIFY(capture("text-size-options"));
        QVERIFY(click(textSize->property("popup").value<QObject *>(), "selectOption_textSize_2")); QCOMPARE(theme.textScale(), 1.3);
        QVERIFY(click(appearance, "reducedMotion")); QVERIFY(theme.reducedMotion()); QVERIFY(capture("accessibility-options"));
        QVERIFY(QMetaObject::invokeMethod(appearance, "close")); QVERIFY(capture("large-text-snapshots"));
        for (const auto control : {"newSnapshot", "snapshotOptions", "restoreCheckpoint", "snapshotDetailsButton", "snapshotMore", "snapshotPageMore", "fitBranches"}) {
            const auto item = vmDetails->findChild<QQuickItem *>(control); QVERIFY(item);
            const auto bounds = item->mapRectToItem(detailsScroll, item->boundingRect());
            QVERIFY2(bounds.left() >= 0 && bounds.right() <= detailsScroll->width(), control);
        }
        QVERIFY(click(window, "detailsTab")); QVERIFY(capture("large-text-overview"));
        QVERIFY(QMetaObject::invokeMethod(wizard, "begin")); QVERIFY(capture("large-text-create")); QVERIFY(QMetaObject::invokeMethod(wizard, "reject"));
        QTRY_VERIFY(!wizard->property("visible").toBool());
        QVERIFY(click(window, "snapshotsTab"));
        QVERIFY(QMetaObject::invokeMethod(appearance, "open"));
        QTRY_VERIFY(appearance->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(textSize->property("popup").value<QObject *>(), "open"));
        QTRY_VERIFY(textSize->property("popup").value<QObject *>()->property("opened").toBool());
        QVERIFY(click(textSize->property("popup").value<QObject *>(), "selectOption_textSize_0")); QCOMPARE(theme.textScale(), 1.0);
        QVERIFY(click(appearance, "reducedMotion")); QVERIFY(!theme.reducedMotion()); QVERIFY(QMetaObject::invokeMethod(appearance, "close"));
        QVERIFY(click(window, "consoleTab"));
        const auto beforeConsoleCapture = Checkpoints::list(uuid).size();
        QVERIFY(capture("console-snapshot-actions-small")); QVERIFY(click(window, "consoleSnapshot"));
        QTRY_VERIFY(checkpointEditor->property("opened").toBool());
        auto consoleName = checkpointEditor->findChild<QQuickItem *>("checkpointName"); QVERIFY(consoleName);
        consoleName->setProperty("text", "Console checkpoint"); consoleName->forceActiveFocus(); QTRY_VERIFY(consoleName->hasActiveFocus());
        QVERIFY(capture("console-name-small")); QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(Checkpoints::list(uuid).size() == beforeConsoleCapture + 1 && !backend->busy(), 30000);
        QVERIFY(!window->property("detailsOpen").toBool());
        QCOMPARE(Checkpoints::list(uuid).last().toMap()["name"].toString(), "Console checkpoint");
        QVERIFY(!Checkpoints::list(uuid).last().toMap()["memory"].toString().isEmpty());
        // QEMU 8.2 cannot migrate the vdagent chardev. Exercise configured
        // clipboard controls only after the memory-snapshot scenarios finish.
        QTRY_VERIFY(!backend->busy()); backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        command("hardware.save", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"clipboard", true}}); QVERIFY(resultOk);
        QTRY_VERIFY(!backend->busy()); backend->action(uuid, "start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy());
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy()); QTRY_VERIFY(console->hasFrame());
        QTRY_VERIFY(backend->details()["liveClipboardConfigured"].toBool());
        for (bool floating : {false, true}) {
            window->setProperty("consoleDetached", floating); auto host = console->window(); host->requestActivate(); QTRY_VERIFY(host->isActive());
            QVERIFY(click(window, "consoleClipboard")); QTRY_VERIFY(clipboardTools->property("opened").toBool());
            QVERIFY(click(clipboardTools, "clipboardToGuest")); QCOMPARE(console->clipboardMode(), "toGuest");
            QVERIFY(click(clipboardTools, "clipboardBoth")); QCOMPARE(console->clipboardMode(), "both");
            QVERIFY(captureOn(host, floating ? "console-clipboard-detached" : "console-clipboard"));
            QVERIFY(click(clipboardTools, "clipboardOff")); QCOMPARE(console->clipboardMode(), "off");
            QVERIFY(QMetaObject::invokeMethod(clipboardTools, "close")); QTRY_VERIFY(!clipboardTools->property("visible").toBool());
        }
        window->setProperty("consoleDetached", false);
        theme.setMode("light"); theme.setTextScale(1.3); QVERIFY(capture("console-compact-light"));
        window->setProperty("consoleDetached", true); auto compactConsole = console->window(); compactConsole->resize(640, 480);
        compactConsole->requestActivate(); QTRY_VERIFY(compactConsole->isActive());
        QVERIFY(captureOn(compactConsole, "console-detached-compact-light"));
        QVERIFY(click(window, "consoleClipboard")); QTRY_VERIFY(clipboardTools->property("opened").toBool());
        auto clipboardContent = qobject_cast<QQuickItem *>(clipboardTools->property("contentItem").value<QObject *>()); QVERIFY(clipboardContent);
        const auto clipboardBounds = clipboardContent->mapRectToScene(clipboardContent->boundingRect());
        QVERIFY(clipboardBounds.left() >= 0 && clipboardBounds.right() <= compactConsole->width());
        QVERIFY(clipboardBounds.top() >= 0 && clipboardBounds.bottom() <= compactConsole->height());
        QVERIFY(captureOn(compactConsole, "console-clipboard-compact-light")); QVERIFY(QMetaObject::invokeMethod(clipboardTools, "close"));
        window->setProperty("consoleDetached", false); theme.setMode("dark"); theme.setTextScale(1.0);
        window->requestActivate(); QTRY_VERIFY(window->isActive());
        auto activity = window->findChild<QObject *>("activityDialog"); QVERIFY(activity); QVERIFY(QMetaObject::invokeMethod(activity, "open")); QVERIFY(capture("activity")); QVERIFY(QMetaObject::invokeMethod(activity, "reject"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString{} : warnings.first().toString()));
    }
    void snapshotTreeRestoration() {
        const auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto result = command("vm.create", {{"name", "tree-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source}, {"preset", "generic"}, {"firmware", "bios"}, {"cpus", 1}, {"memoryMiB", 256}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto uuid = result["uuid"].toString();
        auto disk = [&] { return details(uuid)["disks"].toList().first().toMap()["source"].toString(); };
        auto point = [&](QString name) { for (const auto &v : Checkpoints::list(uuid)) if (v.toMap()["name"] == name) return v.toMap(); return QVariantMap{}; };
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "A Base"}}); QVERIFY(resultOk); const auto base = point("A Base")["id"];
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x52 0 4096", disk()}));
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "B Stable"}}); QVERIFY(resultOk); const auto stable = point("B Stable")["id"];
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", base}}); QVERIFY(resultOk);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x31 0 4096", disk()}));
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x73 0 4096", disk()}));
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "C New tools"}}); QVERIFY(resultOk); const auto toolsPoint = point("C New tools")["id"];
        QCOMPARE(point("B Stable")["parentId"], base); QCOMPARE(point("C New tools")["parentId"], base);
        QCOMPARE(Checkpoints::history(uuid)["currentId"], toolsPoint); QCOMPARE(Checkpoints::list(uuid).size(), 3);
        // A new backend loads the same ancestry/current position after restarting the application.
        { Backend reopened("qemu:///session"); QTRY_VERIFY(reopened.connected()); QVERIFY(reopened.request("snapshots.list", {{"uuid", uuid}})); QTRY_VERIFY(reopened.management().contains("snapshots.list")); QCOMPARE(reopened.management()["snapshots.list"].toMap()["currentId"], toolsPoint); }
        backend->action(uuid, "start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy()); const auto beforeGoTo = domainId(uuid);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", stable}}); QVERIFY(!resultOk); QCOMPARE(domainId(uuid), beforeGoTo);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", stable}, {"allowRestart", true}, {"safety", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(active(uuid)); QVERIFY(domainId(uuid) != beforeGoTo); QCOMPARE(Checkpoints::history(uuid)["currentId"], stable);
        const auto restoredStableDisk = disk(); const auto undoPoint = Checkpoints::undoId(uuid); QVERIFY(!undoPoint.isEmpty());
        result = command("snapshots.undo", {{"uuid", uuid}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(Checkpoints::history(uuid)["currentId"].toString(), undoPoint);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x52 0 4096", restoredStableDisk}));
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x73 0 4096", disk()}));
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", stable}}); QVERIFY(resultOk);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x52 0 4096", disk()}));
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "D Stable update"}}); QVERIFY(resultOk);
        QCOMPARE(point("D Stable update")["parentId"], stable); QCOMPARE(point("C New tools")["parentId"], base);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", toolsPoint}}); QVERIFY(resultOk);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x73 0 4096", disk()}));
        result = command("snapshots.remove", {{"uuid", uuid}, {"id", base}}); QVERIFY(resultOk);
        QVERIFY(point("A Base").isEmpty()); QVERIFY(point("B Stable")["parentId"].toString().isEmpty()); QVERIFY(point("C New tools")["parentId"].toString().isEmpty());
        const auto checkpointDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + uuid + "/" + stable.toString();
        QVERIFY(QFile::rename(checkpointDirectory + "/disk-0.qcow2", checkpointDirectory + "/missing.qcow2")); const auto beforeMissing = xml(uuid);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", stable}}); const bool missingAccepted = resultOk;
        QVERIFY(QFile::rename(checkpointDirectory + "/missing.qcow2", checkpointDirectory + "/disk-0.qcow2"));
        QVERIFY(!missingAccepted); QCOMPARE(xml(uuid), beforeMissing); QCOMPARE(Checkpoints::history(uuid)["currentId"], toolsPoint);
    }
    void snapshotDeletionChoices() {
        const auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto result = command("vm.create", {{"name", "delete-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source}, {"preset", "generic"}, {"firmware", "bios"}, {"cpus", 1}, {"memoryMiB", 256}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk,qPrintable(result["message"].toString())); const auto uuid=result["uuid"].toString();
        auto disk=[&] { return details(uuid)["disks"].toList().first().toMap()["source"].toString(); };
        auto point=[&](QString name) { for (const auto &v : Checkpoints::list(uuid)) if (v.toMap()["name"]==name) return v.toMap(); return QVariantMap{}; };
        const auto root=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/checkpoints/"+uuid;
        command("snapshots.create",{{"uuid",uuid},{"name","A"}}); QVERIFY(resultOk); const auto a=point("A")["id"];
        QVERIFY(process("qemu-io",{"-f","qcow2","-c","write -P 0x52 0 4096",disk()}));
        command("snapshots.create",{{"uuid",uuid},{"name","B"}}); QVERIFY(resultOk); const auto b=point("B")["id"];
        QVERIFY(process("qemu-io",{"-f","qcow2","-c","write -P 0x73 0 4096",disk()}));
        command("snapshots.create",{{"uuid",uuid},{"name","C"},{"pinned",true}}); QVERIFY(resultOk); const auto c=point("C")["id"];
        command("snapshots.restore",{{"uuid",uuid},{"id",a}}); QVERIFY(resultOk);
        QVERIFY(process("qemu-io",{"-f","qcow2","-c","write -P 0x94 0 4096",disk()}));
        command("snapshots.create",{{"uuid",uuid},{"name","D sibling"}}); QVERIFY(resultOk); const auto d=point("D sibling")["id"];
        // Pending settings keep deleted files alive, even when no snapshot needs them.
        QDomDocument pendingXml; QVERIFY(pendingXml.setContent(xml(uuid)));
        pendingXml.documentElement().firstChildElement("devices").firstChildElement("disk").firstChildElement("source").setAttribute("file",root+"/"+b.toString()+"/disk-0.qcow2");
        const auto pendingRoot=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/pending"; QVERIFY(QDir().mkpath(pendingRoot));
        QFile pending(pendingRoot+"/delete-test-"+uuid+".json"); QVERIFY(pending.open(QIODevice::WriteOnly)); pending.write(QJsonDocument::fromVariant(QVariantMap{{"xml",pendingXml.toString()}}).toJson()); pending.close();
        const auto before=xml(uuid);
        result=command("snapshots.remove",{{"uuid",uuid},{"id",b}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        QVERIFY(point("B").isEmpty()); QCOMPARE(point("C")["parentId"],a); QCOMPARE(point("D sibling")["parentId"],a); QCOMPARE(xml(uuid),before);
        QVERIFY(QFile::exists(root+"/"+b.toString()+"/disk-0.qcow2"));
        command("snapshots.cleanup",{{"uuid",uuid},{"key","deleted:"+b.toString()}}); QVERIFY(!resultOk);
        QVERIFY(pending.remove());
        const auto damaged=root+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces); QVERIFY(QDir().mkpath(damaged));
        QFile badMetadata(damaged+"/manifest.json"); QVERIFY(badMetadata.open(QIODevice::WriteOnly)); badMetadata.write("unreadable metadata"); badMetadata.close();
        command("snapshots.cleanup",{{"uuid",uuid},{"key","deleted:"+b.toString()}}); QVERIFY(!resultOk); QVERIFY(QFile::exists(root+"/"+b.toString()+"/disk-0.qcow2"));
        QVERIFY(QDir(damaged).removeRecursively());
        command("snapshots.cleanup",{{"uuid",uuid},{"key","deleted:"+b.toString()}}); QVERIFY(resultOk);
        QVERIFY(!QFile::exists(root+"/"+b.toString())); QCOMPARE(point("C")["parentId"],a);
        { Backend reopened("qemu:///session"); QTRY_VERIFY(reopened.connected()); QVERIFY(reopened.request("snapshots.list",{{"uuid",uuid}})); QTRY_VERIFY(reopened.management().contains("snapshots.list")); QCOMPARE(reopened.management()["snapshots.list"].toMap()["items"].toList().size(),3); }
        command("snapshots.restore",{{"uuid",uuid},{"id",b}}); QVERIFY(!resultOk); QCOMPARE(xml(uuid),before);
        command("snapshots.restore",{{"uuid",uuid},{"id",c}}); QVERIFY(resultOk); QVERIFY(process("qemu-io",{"-f","qcow2","-c","read -P 0x73 0 4096",disk()}));
        backend->action(uuid,"start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy()); const auto runtime=domainId(uuid);
        command("snapshots.create",{{"uuid",uuid},{"name","E"}}); QVERIFY(resultOk); const auto e=point("E")["id"]; const auto working=xml(uuid);
        command("snapshots.remove",{{"uuid",uuid},{"id",c},{"descendants",true},{"expectedIds",QStringList{c.toString()}},{"allowPinned",true}}); QVERIFY(!resultOk);
        const QStringList branch{c.toString(),e.toString()};
        command("snapshots.remove",{{"uuid",uuid},{"id",c},{"descendants",true},{"expectedIds",branch}}); QVERIFY(!resultOk); QCOMPARE(Checkpoints::list(uuid).size(),4);
        result=command("snapshots.remove",{{"uuid",uuid},{"id",c},{"descendants",true},{"expectedIds",branch},{"allowPinned",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        QCOMPARE(Checkpoints::list(uuid).size(),2); QVERIFY(point("C").isEmpty()); QVERIFY(point("E").isEmpty()); QCOMPARE(Checkpoints::history(uuid)["currentId"],a);
        QCOMPARE(domainId(uuid),runtime); QCOMPARE(xml(uuid),working); QVERIFY(active(uuid));
        backend->action(uuid,"force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy()); QVERIFY(process("qemu-io",{"-f","qcow2","-c","read -P 0x73 0 4096",disk()}));
        command("snapshots.remove",{{"uuid",uuid},{"id",a}}); QVERIFY(resultOk); QVERIFY(point("D sibling")["parentId"].toString().isEmpty());
        command("snapshots.restore",{{"uuid",uuid},{"id",d}}); QVERIFY(resultOk); QVERIFY(process("qemu-io",{"-f","qcow2","-c","read -P 0x94 0 4096",disk()}));
        command("snapshots.restore",{{"uuid",uuid},{"id",d},{"safety",true}}); QVERIFY(resultOk); const auto undo=Checkpoints::undoId(uuid); QVERIFY(!undo.isEmpty());
        command("snapshots.remove",{{"uuid",uuid},{"id",undo},{"allowPinned",true}}); QVERIFY(resultOk); QVERIFY(Checkpoints::undoId(uuid).isEmpty());
        command("snapshots.remove",{{"uuid",uuid},{"id",d}}); QVERIFY(resultOk); QVERIFY(Checkpoints::list(uuid).isEmpty()); QVERIFY(Checkpoints::history(uuid)["currentId"].toString().isEmpty());
    }
    void legacySnapshotDeletion() {
        const auto source=makeSource(); QVERIFY(!source.isEmpty());
        auto result=command("vm.create",{{"name","legacy-delete-"+QUuid::createUuid().toString(QUuid::Id128).left(8)},{"sourceMode","disk"},{"source",source},{"preset","generic"},{"firmware","bios"},{"cpus",1},{"memoryMiB",256},{"networkId","none"},{"location",files.path()}});
        QVERIFY(resultOk); const auto uuid=result["uuid"].toString();
        auto domain=virDomainLookupByUUIDString(external,uuid.toUtf8().constData()); QVERIFY(domain);
        for (const auto &name : {"A","B","C"}) {
            const auto definition=QString("<domainsnapshot><name>%1</name><memory snapshot='no'/></domainsnapshot>").arg(name);
            auto snapshot=virDomainSnapshotCreateXML(domain,definition.toUtf8().constData(),0); QVERIFY(snapshot); virDomainSnapshotFree(snapshot);
        }
        virDomainFree(domain); const auto before=xml(uuid);
        result=command("snapshots.remove",{{"uuid",uuid},{"name","B"}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        result=command("snapshots.list",{{"uuid",uuid}}); QCOMPARE(result["items"].toList().size(),2);
        for (const auto &v : result["items"].toList()) if (v.toMap()["name"]=="C") QCOMPARE(v.toMap()["parentId"].toString(),"internal:A");
        command("snapshots.remove",{{"uuid",uuid},{"name","A"},{"descendants",true},{"expectedIds",QStringList{"internal:A"}}}); QVERIFY(!resultOk);
        command("snapshots.remove",{{"uuid",uuid},{"name","A"},{"descendants",true},{"expectedIds",QStringList{"internal:A","internal:C"}}}); QVERIFY(resultOk);
        result=command("snapshots.list",{{"uuid",uuid}}); QVERIFY(result["items"].toList().isEmpty()); QCOMPARE(xml(uuid),before);
        const auto disk=details(uuid)["disks"].toList().first().toMap()["source"].toString(); QVERIFY(process("qemu-io",{"-f","qcow2","-c","read -P 0x31 0 4096",disk}));
    }
    void installationMediaAndEjection() {
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto result = command("vm.create", {{"name", "media-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "iso"}, {"source", source},
            {"preset", "generic"}, {"firmware", "bios"}, {"cpus", 1}, {"memoryMiB", 256}, {"diskGiB", 1}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); auto uuid = result["uuid"].toString();
        auto info = details(uuid); bool optical = false;
        for (auto v : info["disks"].toList()) if (v.toMap()["device"] == "cdrom") { QCOMPARE(v.toMap()["source"].toString(), source); optical = true; }
        QVERIFY(optical); QVERIFY(info["bootOrder"].toString().contains("cdrom"));
        QDomDocument definition; QVERIFY(definition.setContent(xml(uuid))); QCOMPARE(definition.documentElement().firstChildElement("on_reboot").text(), "restart");
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        result = command("hardware.save", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"iso", ""}, {"boot", "hd"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        for (auto v : details(uuid)["disks"].toList()) if (v.toMap()["device"] == "cdrom") QVERIFY(v.toMap()["source"].toString().isEmpty());
        QVERIFY(process("qemu-io", {"-f", "raw", "-c", "read -P 0x31 0 4096", source}));
    }
    void uefiCheckpointRestoration() {
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto result = command("vm.create", {{"name", "uefi-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source},
            {"preset", "generic"}, {"firmware", "uefi"}, {"cpus", 1}, {"memoryMiB", 256}, {"diskGiB", 1}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); auto uuid = result["uuid"].toString();
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy()); QTest::qWait(500);
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        auto before = details(uuid);
        auto oldDisk = before["disks"].toList().first().toMap()["source"].toString(), oldNvram = before["nvram"].toString();
        QFile nvram(oldNvram); QVERIFY(nvram.open(QIODevice::ReadOnly)); const auto firmware = nvram.readAll(); nvram.close(); QVERIFY(!firmware.isEmpty());
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "UEFI baseline"}, {"notes", "Disk and firmware checkpoint"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("snapshots.list", {{"uuid", uuid}}); QCOMPARE(result["items"].toList().size(), 1); const auto checkpoint = result["items"].toList().first().toMap(); QCOMPARE(checkpoint["kind"].toString(), "copy");
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x52 0 4096", oldDisk}));
        QVERIFY(nvram.open(QIODevice::ReadWrite)); QVERIFY(nvram.seek(nvram.size() - 1)); QVERIFY(nvram.write("X") == 1); nvram.close();
        result = command("snapshots.verify", {{"uuid", uuid}, {"name", checkpoint["name"]}, {"id", checkpoint["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("snapshots.restore", {{"uuid", uuid}, {"name", checkpoint["name"]}, {"id", checkpoint["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        // A VM without a TPM never gets TPM state, even after its snapshot was verified and restored.
        QVERIFY(!QDir(qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/libvirt/qemu/swtpm/" + uuid).exists());
        auto restored = details(uuid);
        auto newDisk = restored["disks"].toList().first().toMap()["source"].toString(); QVERIFY(newDisk != oldDisk);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x31 0 4096", newDisk}));
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x52 0 4096", oldDisk}));
        QFile restoredNvram(restored["nvram"].toString()); QVERIFY(restoredNvram.open(QIODevice::ReadOnly)); QCOMPARE(restoredNvram.readAll(), firmware); restoredNvram.close();
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        result = command("snapshots.remove", {{"uuid", uuid}, {"name", checkpoint["name"]}, {"id", checkpoint["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        // Restore intentionally retains the old firmware file; this file belongs only to our fixture.
        QVERIFY(QFile::remove(oldNvram));
    }
    void windowsTpmVms() {
        if (QStandardPaths::findExecutable("swtpm").isEmpty()) QSKIP("swtpm isn't installed here, so VMs can't have a TPM.");
        // A Windows 11 VM: SATA disk, TPM 2.0 and UEFI (with Secure Boot when the host has it).
        QFile installer(files.filePath("Win11_26H2_English_x64.iso")); QVERIFY(installer.open(QIODevice::WriteOnly)); installer.write(QByteArray(1 << 20, 'w')); installer.close();
        auto result = command("vm.create", {{"name", "win-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "iso"}, {"source", installer.fileName()},
            {"preset", "win11"}, {"firmware", "uefi"}, {"cpus", 1}, {"memoryMiB", 512}, {"diskGiB", 1}, {"networkId", "none"}, {"location", files.path()},
            {"tpm", true}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto uuid = result["uuid"].toString();
        QDomDocument doc; QVERIFY(doc.setContent(xml(uuid)));
        auto tpm = doc.documentElement().firstChildElement("devices").firstChildElement("tpm"); QVERIFY(!tpm.isNull());
        QCOMPARE(tpm.firstChildElement("backend").attribute("type"), QString("emulator")); QCOMPARE(tpm.firstChildElement("backend").attribute("version"), QString("2.0"));
        QStringList cds; QString systemBus;
        for (auto d = doc.documentElement().firstChildElement("devices").firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk")) {
            if (d.attribute("device") == "cdrom") cds << d.firstChildElement("source").attribute("file");
            else systemBus = d.firstChildElement("target").attribute("bus");
        }
        QCOMPARE(systemBus, QString("sata")); QCOMPARE(cds, QStringList({installer.fileName()}));
        qInfo() << "Secure Boot firmware:" << xml(uuid).contains("secure-boot") << doc.documentElement().firstChildElement("os").firstChildElement("loader").text();
        // The TPM's state appears once the VM has run, and snapshots keep it.
        const auto state = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/libvirt/qemu/swtpm/" + uuid + "/tpm2/tpm2-00.permall";
        auto contents = [&] { QFile f(state); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        QTRY_VERIFY(!contents().isEmpty());
        // A live snapshot (the VM keeps running) and a memory snapshot both work with a TPM.
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Live"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "With memory"}, {"memory", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        auto point = [&](QString name) { for (auto value : Checkpoints::list(uuid)) if (value.toMap()["name"] == name) return value.toMap(); return QVariantMap{}; };
        QCOMPARE(point("Live")["tpm"].toString(), QString("tpm-state")); QCOMPARE(point("With memory")["tpm"].toString(), QString("tpm-state"));
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        const auto saved = contents(); QVERIFY(!saved.isEmpty());
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Stopped"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(point("Stopped")["tpm"].toString(), QString("tpm-state"));
        // Something changes the TPM; restoring the snapshot brings back its contents.
        { QFile f(state); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write("changed"); }
        result = command("snapshots.restore", {{"uuid", uuid}, {"name", "Stopped"}, {"id", point("Stopped")["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(contents(), saved);
        // Restoring the memory snapshot resumes the VM with its TPM.
        result = command("snapshots.restore", {{"uuid", uuid}, {"name", "With memory"}, {"id", point("With memory")["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        // A VM made from a snapshot gets its own copy of the TPM.
        result = command("snapshots.clone", {{"uuid", uuid}, {"id", point("Stopped")["id"]}, {"name", "win-copy-" + QUuid::createUuid().toString(QUuid::Id128).left(6)}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto copy = result["uuid"].toString(); created << copy;
        QFile copied(qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/libvirt/qemu/swtpm/" + copy + "/tpm2/tpm2-00.permall");
        QVERIFY(copied.open(QIODevice::ReadOnly)); QCOMPARE(copied.readAll(), saved);
    }
    void liveCheckpoints_data() {
        QTest::addColumn<QString>("firmware");
        QTest::newRow("bios") << QString("bios"); QTest::newRow("uefi") << QString("uefi");
    }
    void liveCheckpoints() {
        QFETCH(QString, firmware);
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto result = command("vm.create", {{"name", "live-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source},
            {"preset", "generic"}, {"firmware", firmware}, {"cpus", 1}, {"memoryMiB", 256}, {"diskGiB", 1}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); auto uuid = result["uuid"].toString();
        command("disk.add", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"diskGiB", 1}}); QVERIFY(resultOk);
        auto initial = details(uuid); const auto initialDisks = initial["disks"].toList(); QCOMPARE(initialDisks.size(), 2);
        const auto second = initialDisks[1].toMap()["source"].toString();
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x63 0 4096", second}));
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy()); QTest::qWait(500);
        auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(d);
        std::unique_ptr<virDomain, decltype(&virDomainFree)> domain(d, virDomainFree);
        const auto runtimeId = virDomainGetID(d); const auto definition = xml(uuid);
        result = command("snapshots.list", {{"uuid", uuid}}); QVERIFY(resultOk); QVERIFY2(result["blocker"].toString().isEmpty(), qPrintable(result["blocker"].toString())); QVERIFY(result["restoreBlocker"].toString().isEmpty());
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "While running"}, {"notes", "Both disks and firmware"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(virDomainGetID(d), runtimeId); int state = 0, reason = 0; QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, VIR_DOMAIN_RUNNING); QCOMPARE(xml(uuid), definition);
        result = command("snapshots.list", {{"uuid", uuid}}); QCOMPARE(result["items"].toList().size(), 1); auto first = result["items"].toList().first().toMap();
        QCOMPARE(first["kind"].toString(), "copy"); QCOMPARE(first["capture"].toString(), "live"); QCOMPARE(first["diskCount"].toInt(), 2); QVERIFY(first["bytes"].toULongLong() > 0);
        command("snapshots.restore", {{"uuid", uuid}, {"id", first["id"]}}); QVERIFY(!resultOk); QVERIFY(active(uuid));
        backend->action(uuid, "pause"); QTRY_VERIFY(!backend->busy());
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "While paused"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, VIR_DOMAIN_PAUSED);
        auto copies = Checkpoints::list(uuid); QCOMPARE(copies.size(), 2); QVariantMap child;
        for (auto v : copies) if (v.toMap()["name"] == "While paused") child = v.toMap();
        QCOMPARE(child["capture"].toString(), "paused"); QCOMPARE(child["parentId"], first["id"]);
        command("snapshots.remove", {{"uuid", uuid}, {"id", first["id"]}, {"descendants", true}, {"expectedIds", QStringList{first["id"].toString()}}}); QVERIFY(!resultOk);
        result = command("snapshots.remove", {{"uuid", uuid}, {"id", child["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QCOMPARE(virDomainGetID(d), runtimeId);
        backend->action(uuid, "resume"); QTRY_VERIFY(!backend->busy());
        const auto scratch = files.filePath("foreign-" + uuid + ".qcow2"), socket = files.filePath("backup-" + uuid.left(8) + ".sock");
        const auto foreign = QString("<domainbackup mode='pull'><server transport='unix' socket='%1'/><disks><disk name='vda' type='file'><scratch file='%2'/></disk></disks></domainbackup>").arg(socket, scratch);
        QCOMPARE(virDomainBackupBegin(d, foreign.toUtf8().constData(), nullptr, 0), 0);
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Conflicting job"}}); QVERIFY(!resultOk); QVERIFY(result["message"].toString().contains("Another VM job"));
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", first["id"]}, {"allowRestart", true}}); QVERIFY(!resultOk); QVERIFY(result["message"].toString().contains("Another VM job")); QCOMPARE(domainId(uuid), runtimeId);
        char *foreignXml = virDomainBackupGetXMLDesc(d, 0); QVERIFY(foreignXml); QVERIFY(QString::fromUtf8(foreignXml).contains(scratch)); free(foreignXml);
        QCOMPARE(virDomainAbortJob(d), 0);
        auto jobStopped = [&] { virDomainJobInfo job{}; return virDomainGetJobInfo(d, &job) == 0 && job.type == VIR_DOMAIN_JOB_NONE; };
        QTRY_VERIFY(jobStopped());
        std::atomic_bool cancel{false}; bool began = false; QString error;
        QVERIFY(!Checkpoints::createLive(d, xml(uuid), "Cancelled copy", "", cancel, [&](QString phase, qulonglong, qulonglong) { if (phase == "Copying live disks") { began = true; cancel = true; } }, error));
        QVERIFY2(began, qPrintable(error)); QVERIFY(error.contains("cancelled")); QCOMPARE(Checkpoints::list(uuid).size(), 1);
        QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, VIR_DOMAIN_RUNNING); QCOMPARE(virDomainGetID(d), runtimeId);
        const auto nvram = details(uuid)["nvram"].toString();
        if (firmware == "uefi") {
            const auto permissions = QFileInfo(nvram).permissions(); QVERIFY(QFile::setPermissions(nvram, {}));
            result = command("snapshots.create", {{"uuid", uuid}, {"name", "Firmware failure"}});
            const auto ok = resultOk; QVERIFY(QFile::setPermissions(nvram, permissions)); QVERIFY(!ok); QVERIFY(result["message"].toString().contains("UEFI"));
            QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, VIR_DOMAIN_RUNNING);
        }
        cancel = false; error.clear(); bool stoppedDuringCopy = false;
        QVERIFY(!Checkpoints::createLive(d, xml(uuid), "Interrupted copy", "", cancel, [&](QString phase, qulonglong, qulonglong) { if (phase == "Copying live disks" && !stoppedDuringCopy) stoppedDuringCopy = virDomainDestroy(d) == 0; }, error));
        QVERIFY2(stoppedDuringCopy, qPrintable(error)); QVERIFY(!active(uuid)); QCOMPARE(Checkpoints::list(uuid).size(), 1);
        for (auto v : initialDisks) QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x7a 0 4096", v.toMap()["source"].toString()}));
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", first["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        const auto restored = details(uuid)["disks"].toList(); QCOMPARE(restored.size(), 2);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x31 0 4096", restored[0].toMap()["source"].toString()}));
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x63 0 4096", restored[1].toMap()["source"].toString()}));
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        result = command("snapshots.remove", {{"uuid", uuid}, {"id", first["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QVERIFY(active(uuid));
        if (!nvram.isEmpty()) QVERIFY(QFile::remove(nvram));
    }
    void runningRestoration_data() {
        QTest::addColumn<QString>("firmware");
        QTest::newRow("bios") << QString("bios"); QTest::newRow("uefi") << QString("uefi");
    }
    void runningRestoration() {
        QFETCH(QString, firmware);
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto result = command("vm.create", {{"name", "restore-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source},
            {"preset", "generic"}, {"firmware", firmware}, {"cpus", 1}, {"memoryMiB", 256}, {"diskGiB", 1}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto uuid = result["uuid"].toString();
        result = command("disk.add", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"diskGiB", 1}}); QVERIFY(resultOk);
        const auto originalDisks = details(uuid)["disks"].toList(); QCOMPARE(originalDisks.size(), 2);
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x63 0 4096", originalDisks[1].toMap()["source"].toString()}));
        backend->action(uuid, "start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Restore target"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        const auto checkpoint = Checkpoints::list(uuid).first().toMap();
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + uuid + "/" + checkpoint["id"].toString();
        for (auto disk : originalDisks) QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "write -P 0x7a 0 4096", disk.toMap()["source"].toString()}));
        backend->action(uuid, "start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy());
        const auto beforeId = domainId(uuid); const auto beforeXml = xml(uuid); const auto nvram = details(uuid)["nvram"].toString();
        auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(d);
        std::unique_ptr<virDomain, decltype(&virDomainFree)> domain(d, virDomainFree);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", checkpoint["id"]}}); QVERIFY(!resultOk); QCOMPARE(domainId(uuid), beforeId);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"allowRestart", true}}); QVERIFY(!resultOk); QCOMPARE(domainId(uuid), beforeId);
        QVERIFY(QFile::rename(directory + "/disk-0.qcow2", directory + "/missing.qcow2"));
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", checkpoint["id"]}, {"allowRestart", true}});
        const auto missingResult = resultOk; QVERIFY(QFile::rename(directory + "/missing.qcow2", directory + "/disk-0.qcow2"));
        QVERIFY(!missingResult); QCOMPARE(domainId(uuid), beforeId); QCOMPARE(xml(uuid), beforeXml);
        QString failure; bool stayedOn = false, changed = false;
        QVERIFY(!Checkpoints::restore(external, d, beforeXml, checkpoint["id"].toString(), failure, true, [&](QString phase, qulonglong, qulonglong) {
            if (phase == "Checking restored files before switching the VM") { stayedOn = active(uuid) && domainId(uuid) == beforeId; changed = virDomainSuspend(d) == 0; }
        }));
        QVERIFY(stayedOn); QVERIFY(changed); QVERIFY(failure.contains("state or configuration changed")); QCOMPARE(domainId(uuid), beforeId);
        QCOMPARE(virDomainResume(d), 0);
        // A valid definition can still fail to start, e.g. an unavailable network.
        // Inject this only into the disposable checkpoint and verify old VM recovery.
        QFile manifestFile(directory + "/manifest.json"); QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const auto manifestBytes = manifestFile.readAll(); manifestFile.close(); auto manifest = QJsonDocument::fromJson(manifestBytes).toVariant().toMap();
        auto incomplete = manifest; incomplete["disks"] = QVariantList{};
        QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)); manifestFile.write(QJsonDocument::fromVariant(incomplete).toJson()); manifestFile.close();
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", checkpoint["id"]}, {"allowRestart", true}}); QVERIFY(!resultOk); QCOMPARE(domainId(uuid), beforeId);
        QDomDocument broken; QVERIFY(broken.setContent(manifest["xml"].toString()));
        auto iface = broken.createElement("interface"); iface.setAttribute("type", "bridge");
        auto bridge = broken.createElement("source"); bridge.setAttribute("bridge", "oma-absent-test"); iface.appendChild(bridge);
        auto model = broken.createElement("model"); model.setAttribute("type", "virtio"); iface.appendChild(model);
        broken.documentElement().firstChildElement("devices").appendChild(iface); manifest["xml"] = broken.toString(-1);
        QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)); manifestFile.write(QJsonDocument::fromVariant(manifest).toJson()); manifestFile.close();
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", checkpoint["id"]}, {"allowRestart", true}});
        const auto failedStartResult = resultOk;
        QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)); manifestFile.write(manifestBytes); manifestFile.close();
        QVERIFY(!failedStartResult); QVERIFY2(result["message"].toString().contains("Start restored VM"), qPrintable(result["message"].toString()));
        QVERIFY(result["message"].toString().contains("previous disks and configuration were restored and restarted"));
        QVERIFY(active(uuid)); QCOMPARE(xml(uuid), beforeXml); QVERIFY(domainId(uuid) != beforeId); QVERIFY(result["restarted"].toBool());
        const auto recoveredId = domainId(uuid);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", checkpoint["id"]}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(active(uuid)); QVERIFY(domainId(uuid) != recoveredId); QVERIFY(result["restarted"].toBool());
        int state = 0, reason = 0; QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, VIR_DOMAIN_RUNNING);
        const auto runningDisks = details(uuid)["disks"].toList();
        for (int i = 0; i < runningDisks.size(); ++i) QVERIFY(runningDisks[i].toMap()["source"] != originalDisks[i].toMap()["source"]);
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x31 0 4096", runningDisks[0].toMap()["source"].toString()}));
        QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x63 0 4096", runningDisks[1].toMap()["source"].toString()}));
        for (auto disk : originalDisks) QVERIFY(process("qemu-io", {"-f", "qcow2", "-c", "read -P 0x7a 0 4096", disk.toMap()["source"].toString()}));
        QCOMPARE(virDomainCreateWithFlags(d, VIR_DOMAIN_START_PAUSED), 0); const auto pausedId = domainId(uuid);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", checkpoint["id"]}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(domainId(uuid) != pausedId); QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, VIR_DOMAIN_PAUSED);
        if (firmware == "uefi") {
            QFile expected(directory + "/firmware-vars"), actual(details(uuid)["nvram"].toString());
            QVERIFY(expected.open(QIODevice::ReadOnly)); QVERIFY(actual.open(QIODevice::ReadOnly)); QCOMPARE(actual.readAll(), expected.readAll());
            QVERIFY(QFile::remove(nvram));
        }
        QCOMPARE(virDomainResume(d), 0);
    }
    void checkpointWhileGuestWrites() {
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto object = files.filePath("writer.o"), boot = files.filePath("writer.bin");
        QVERIFY(process("as", {"--32", QStringLiteral(QT_TESTCASE_SOURCEDIR) + "/tests/fixtures/live-writer.S", "-o", object}));
        QVERIFY(process("ld", {"-m", "elf_i386", "-Ttext", "0x7c00", "--oformat", "binary", "-o", boot, object}));
        QFile bootFile(boot); QVERIFY(bootFile.open(QIODevice::ReadOnly)); auto sector = bootFile.readAll(); QCOMPARE(sector.size(), 512);
        QFile diskFile(source); QVERIFY(diskFile.open(QIODevice::ReadWrite)); QCOMPARE(diskFile.write(sector), qint64(512)); diskFile.close();
        auto result = command("vm.create", {{"name", "writer-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source},
            {"preset", "generic"}, {"firmware", "bios"}, {"cpus", 1}, {"memoryMiB", 256}, {"diskGiB", 1}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); auto uuid = result["uuid"].toString();
        // Public BlockPeek supports raw disks. Use a private raw working copy so
        // the test can observe guest writes without bypassing qcow2 image locks.
        const auto working = files.filePath("writer-live.raw");
        QVERIFY(process("qemu-img", {"convert", "-f", "raw", "-O", "raw", source, working}));
        QDomDocument definition; QVERIFY(definition.setContent(xml(uuid)));
        auto device = definition.documentElement().firstChildElement("devices").firstChildElement("disk");
        device.firstChildElement("driver").setAttribute("type", "raw"); device.firstChildElement("source").setAttribute("file", working);
        auto defined = virDomainDefineXMLFlags(external, definition.toString(-1).toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE); QVERIFY(defined); virDomainFree(defined);
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(d);
        std::unique_ptr<virDomain, decltype(&virDomainFree)> domain(d, virDomainFree);
        auto guestCounter = [&] { quint32 value = 0; return virDomainBlockPeek(d, "vda", 512, 4, &value, 0) == 0 ? qFromLittleEndian(value) : quint32(0); };
        QTRY_VERIFY_WITH_TIMEOUT(guestCounter() > 0 && guestCounter() < 10000, 15000);
        const auto before = guestCounter(), runtimeId = virDomainGetID(d);
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "During disk writes"}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(virDomainGetID(d), runtimeId); QTRY_VERIFY(guestCounter() > before);
        auto rows = Checkpoints::list(uuid); QCOMPARE(rows.size(), 1); auto checkpoint = rows.first().toMap();
        const auto copy = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + uuid + "/" + checkpoint["id"].toString() + "/disk-0.qcow2";
        const auto raw = files.filePath("captured.raw"); QVERIFY(process("qemu-img", {"convert", "-f", "qcow2", "-O", "raw", copy, raw}));
        QFile captured(raw); QVERIFY(captured.open(QIODevice::ReadOnly)); QVERIFY(captured.seek(512)); auto data = captured.read(512); QCOMPARE(data.size(), 512);
        quint32 value = qFromLittleEndian<quint32>(data.constData()); QVERIFY(value >= before); QVERIFY(value <= guestCounter());
        for (int i = 0; i < 512; i += 4) QCOMPARE(qFromLittleEndian<quint32>(data.constData() + i), value);
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", checkpoint["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        const auto restored = details(uuid)["disks"].toList().first().toMap()["source"].toString();
        const auto restoredRaw = files.filePath("restored.raw"); QVERIFY(process("qemu-img", {"convert", "-f", "qcow2", "-O", "raw", restored, restoredRaw}));
        QFile verify(restoredRaw); QVERIFY(verify.open(QIODevice::ReadOnly)); QVERIFY(verify.seek(512)); QCOMPARE(verify.read(512), data);
    }
    void checkpointWorkspace() {
        auto source = makeSource(); QVERIFY(!source.isEmpty());
        auto result = command("vm.create", {{"name", "workspace-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source}, {"preset", "generic"}, {"firmware", "uefi"}, {"cpus", 1}, {"memoryMiB", 256}, {"networkId", "user"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto uuid = result["uuid"].toString();
        const auto app = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), root = app + "/checkpoints/" + uuid;
        auto points = [&] { return Checkpoints::list(uuid); };
        auto byName = [&](QString name) { for (auto v : points()) if (v.toMap()["name"] == name) return v.toMap(); return QVariantMap{}; };
        auto disk = [&] { return details(uuid)["disks"].toList().first().toMap()["source"].toString(); };
        QImage preview(320,180,QImage::Format_RGB32); preview.fill(Qt::blue); QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(preview.save(&buffer,"PNG"));
        result = command("snapshots.create", {{"uuid",uuid},{"name","Baseline"},{"preview","data:image/png;base64," + QString::fromLatin1(png.toBase64())}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        auto a = byName("Baseline"); QVERIFY(!a.isEmpty()); QVERIFY(!a["safety"].toBool()); QVERIFY(!a["previewUrl"].toString().isEmpty());
        result = command("snapshots.edit", {{"uuid",uuid},{"id",a["id"]},{"name","Known baseline"},{"notes","Boots correctly"},{"tags","baseline, stable"},{"pinned",true},{"knownGood",true}}); QVERIFY(resultOk);
        a = byName("Known baseline"); QVERIFY(a["knownGood"].toBool()); QCOMPARE(a["notes"].toString(),"Boots correctly");
        command("snapshots.remove", {{"uuid",uuid},{"id",a["id"]}}); QVERIFY(!resultOk);
        QVERIFY(process("qemu-io", {"-f","qcow2","-c","write -P 0x42 0 4096",disk()}));
        result = command("snapshots.create", {{"uuid",uuid},{"name","Branch one"}}); QVERIFY(resultOk); auto b = byName("Branch one"); QCOMPARE(b["parentId"],a["id"]); QCOMPARE(b["parent"].toString(),"Known baseline"); QCOMPARE(b["depth"].toInt(),1);
        result = command("snapshots.restore", {{"uuid",uuid},{"id",a["id"]},{"safety",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        const auto recoveryId = Checkpoints::undoId(uuid); QVERIFY(!recoveryId.isEmpty());
        QVERIFY(process("qemu-io", {"-f","qcow2","-c","read -P 0x31 0 4096",disk()}));
        result = command("snapshots.create", {{"uuid",uuid},{"name","Branch two"}}); QVERIFY(resultOk); auto c = byName("Branch two"); QCOMPARE(c["parentId"],a["id"]); QCOMPARE(c["depth"].toInt(),1);
        result = command("snapshots.undo", {{"uuid",uuid}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); QVERIFY(Checkpoints::undoId(uuid).isEmpty());
        QVERIFY(process("qemu-io", {"-f","qcow2","-c","read -P 0x42 0 4096",disk()}));
        const auto originalXml = xml(uuid);
        result = command("snapshots.clone", {{"uuid",uuid},{"id",a["id"]},{"name","clone-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        auto cloneUuid = result["uuid"].toString(); QVERIFY(!cloneUuid.isEmpty()); QVERIFY(cloneUuid != uuid); QVERIFY(!active(cloneUuid)); QCOMPARE(xml(uuid), originalXml);
        const auto cloneInfo = details(cloneUuid); const auto cloneDisk = cloneInfo["disks"].toList().first().toMap()["source"].toString();
        QVERIFY(process("qemu-io", {"-f","qcow2","-c","read -P 0x31 0 4096",cloneDisk}));
        QVERIFY(cloneInfo["nvram"] != details(uuid)["nvram"]);
        const auto nic = cloneInfo["interfaces"].toList().first().toMap(); QVERIFY(nic["mac"] != details(uuid)["interfaces"].toList().first().toMap()["mac"]); QVERIFY(!nic["linkUp"].toBool());
        result = command("snapshots.verify", {{"uuid",uuid},{"id",a["id"]}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); QCOMPARE(byName("Known baseline")["health"].toString(),"Verified");
        const auto checkpointDisk = root + "/" + a["id"].toString() + "/disk-0.qcow2";
        QVERIFY(QFile::copy(checkpointDisk,files.filePath("checkpoint-original.qcow2")));
        QVERIFY(process("qemu-io", {"-f","qcow2","-c","write -P 0x77 0 4096",checkpointDisk}));
        result = command("snapshots.verify", {{"uuid",uuid},{"id",a["id"]}}); QVERIFY(!resultOk); QVERIFY(result["message"].toString().contains("changed")); QVERIFY(!byName("Known baseline")["healthy"].toBool());
        command("snapshots.restore", {{"uuid",uuid},{"id",a["id"]}}); QVERIFY(!resultOk); QCOMPARE(xml(uuid), originalXml);
        QVERIFY(QFile::remove(checkpointDisk)); QVERIFY(QFile::copy(files.filePath("checkpoint-original.qcow2"),checkpointDisk));
        result = command("snapshots.verify", {{"uuid",uuid},{"id",a["id"]}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); QVERIFY(byName("Known baseline")["healthy"].toBool());
        result = command("snapshots.list", {{"uuid",uuid}});
        QString retainedKey, retainedDisk;
        for (auto value : result["storage"].toMap()["candidates"].toList()) if (value.toMap()["available"].toBool()) { retainedKey=value.toMap()["key"].toString(); retainedDisk=Paths::vmDir(uuid)+"/"+retainedKey.mid(8)+"/disk-0.qcow2"; break; }
        QVERIFY(!retainedKey.isEmpty());
        // A backing file referenced indirectly by another stopped VM is protected.
        const auto overlay=files.filePath("dependent-vm.qcow2"), cloneXml=xml(cloneUuid);
        QVERIFY(process("qemu-img",{"create","-f","qcow2","-F","qcow2","-b",retainedDisk,overlay}));
        QDomDocument linked; QVERIFY(linked.setContent(cloneXml)); linked.documentElement().firstChildElement("devices").firstChildElement("disk").firstChildElement("source").setAttribute("file",overlay);
        auto linkedVm=virDomainDefineXML(external,linked.toString(-1).toUtf8().constData()); QVERIFY(linkedVm); virDomainFree(linkedVm);
        result=command("snapshots.cleanup",{{"uuid",uuid},{"key",retainedKey}}); QVERIFY(!resultOk); QVERIFY(QFile::exists(retainedDisk));
        linkedVm=virDomainDefineXML(external,cloneXml.toUtf8().constData()); QVERIFY(linkedVm); virDomainFree(linkedVm);
        // Pending configuration also protects files that are not currently attached.
        QDir().mkpath(app+"/pending"); QFile pending(app+"/pending/storage-test-"+uuid+".json"); QVERIFY(pending.open(QIODevice::WriteOnly)); QVERIFY(pending.write(QJsonDocument::fromVariant(QVariantMap{{"xml",linked.toString(-1)}}).toJson())>0); pending.close();
        command("snapshots.cleanup",{{"uuid",uuid},{"key",retainedKey}}); QVERIFY(!resultOk); QVERIFY(pending.remove());
        const auto orphan=QUuid::createUuid().toString(QUuid::WithoutBraces), orphanRoot=app+"/vms/"+orphan;
        QVERIFY(QDir().mkpath(orphanRoot)); QFile building(orphanRoot+"/building.json"); QVERIFY(building.open(QIODevice::WriteOnly)); QVERIFY(building.write(QJsonDocument::fromVariant(QVariantMap{{"sourceUuid",uuid},{"uuid",orphan},{"pid",0}}).toJson())>0); building.close();
        result=command("snapshots.cleanup",{{"uuid",uuid},{"key","clone:"+orphan}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); QVERIFY(!QFile::exists(orphanRoot));
        result = command("snapshots.list", {{"uuid",uuid}}); const auto candidates = result["storage"].toMap()["candidates"].toList(); QVERIFY(candidates.size() >= 2);
        bool protectedWorking = false, removedUnused = false;
        for (auto value : candidates) { auto row=value.toMap(); if (row["available"].toBool()) { result=command("snapshots.cleanup",{{"uuid",uuid},{"key",row["key"]}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); removedUnused=true; } else { command("snapshots.cleanup",{{"uuid",uuid},{"key",row["key"]}}); QVERIFY(!resultOk); protectedWorking=true; } }
        QVERIFY(removedUnused); QVERIFY(protectedWorking); QVERIFY(QFile::exists(disk())); QVERIFY(QFile::exists(source));
        // A saved journal must not overwrite an unrelated configuration change.
        auto d = virDomainLookupByUUIDString(external,uuid.toUtf8().constData()); QVERIFY(d); std::unique_ptr<virDomain,decltype(&virDomainFree)> domain(d,virDomainFree);
        auto journal = [&](QVariantMap data) { QFile f(root+"/restore.json"); return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument::fromVariant(data).toJson()) > 0; };
        QVERIFY(journal({{"uuid",uuid},{"beforeXml",originalXml},{"afterXml",originalXml},{"beforeState",VIR_DOMAIN_SHUTOFF},{"beforeMarker",QVariantMap{{"id",recoveryId}}}}));
        QString error; QVERIFY2(Checkpoints::recover(external,d,error),qPrintable(error)); QVERIFY(!QFile::exists(root+"/restore.json"));
        QVERIFY(journal({{"uuid",uuid},{"beforeXml",originalXml},{"afterXml",originalXml},{"beforeState",VIR_DOMAIN_SHUTOFF}}));
        QDomDocument changed; QVERIFY(changed.setContent(originalXml)); changed.documentElement().firstChildElement("vcpu").firstChild().setNodeValue("2");
        auto changedDomain=virDomainDefineXML(external,changed.toString(-1).toUtf8().constData()); QVERIFY(changedDomain); virDomainFree(changedDomain);
        QVERIFY(!Checkpoints::recover(external,d,error)); QVERIFY(error.contains("changed")); QVERIFY(QFile::exists(root+"/restore.json"));
        QVERIFY(QFile::remove(root+"/restore.json"));
        result=command("snapshots.restore",{{"uuid",uuid},{"id",a["id"]},{"safety",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); QVERIFY(!Checkpoints::undoId(uuid).isEmpty());
        result=command("snapshots.restore",{{"uuid",uuid},{"id",a["id"]},{"safety",false}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); QVERIFY(Checkpoints::undoId(uuid).isEmpty());
    }
    void incrementalCheckpointChain() {
        auto source=makeSource(); QVERIFY(!source.isEmpty());
        auto object=files.filePath("incremental-writer.o"), boot=files.filePath("incremental-writer.bin");
        QVERIFY(process("as",{"--32",QStringLiteral(QT_TESTCASE_SOURCEDIR)+"/tests/fixtures/live-writer.S","-o",object}));
        QVERIFY(process("ld",{"-m","elf_i386","-Ttext","0x7c00","--oformat","binary","-o",boot,object}));
        QFile bootFile(boot); QVERIFY(bootFile.open(QIODevice::ReadOnly)); QFile diskFile(source); QVERIFY(diskFile.open(QIODevice::ReadWrite)); QCOMPARE(diskFile.write(bootFile.readAll()),qint64(512)); diskFile.close();
        QVERIFY(process("qemu-io",{"-f","raw","-c","write -P 0x5a 1M 16M",source}));
        auto result=command("vm.create",{{"name","incremental-"+QUuid::createUuid().toString(QUuid::Id128).left(8)},{"sourceMode","disk"},{"source",source},{"preset","generic"},{"firmware","bios"},{"cpus",1},{"memoryMiB",256},{"networkId","none"},{"location",files.path()}});
        QVERIFY2(resultOk,qPrintable(result["message"].toString())); const auto uuid=result["uuid"].toString();
        backend->action(uuid,"start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy()); QTest::qWait(1000);
        auto d=virDomainLookupByUUIDString(external,uuid.toUtf8().constData()); QVERIFY(d); std::unique_ptr<virDomain,decltype(&virDomainFree)> domain(d,virDomainFree); const auto runtimeId=virDomainGetID(d);
        result=command("snapshots.create",{{"uuid",uuid},{"name","Full base"},{"incremental",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        auto a=Checkpoints::list(uuid).first().toMap(); QCOMPARE(a["storageMode"].toString(),"full"); QTest::qWait(400);
        result=command("snapshots.create",{{"uuid",uuid},{"name","Only changed blocks"},{"incremental",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        auto b=Checkpoints::list(uuid).last().toMap(); QCOMPARE(b["storageMode"].toString(),"incremental"); QCOMPARE(b["baseId"],a["id"]); QVERIFY(b["bytes"].toULongLong() < a["bytes"].toULongLong()/2); QCOMPARE(virDomainGetID(d),runtimeId);
        const auto root=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/checkpoints/"+uuid;
        auto capturedCounter=[&](QVariantMap cp) { auto raw=files.filePath(cp["id"].toString()+".raw"); if (!process("qemu-img",{"convert","-f","qcow2","-O","raw",root+"/"+cp["id"].toString()+"/disk-0.qcow2",raw})) return quint32(0); QFile f(raw); if (!f.open(QIODevice::ReadOnly) || !f.seek(512)) return quint32(0); auto bytes=f.read(512); if(bytes.size()!=512) return quint32(0); const auto n=qFromLittleEndian<quint32>(bytes.constData()); for(int i=0;i<512;i+=4) if(qFromLittleEndian<quint32>(bytes.constData()+i)!=n) return quint32(0); return n; };
        auto before=capturedCounter(a), after=capturedCounter(b); QVERIFY(before>0); QVERIFY(after>before);
        result=command("snapshots.remove",{{"uuid",uuid},{"id",a["id"]}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        QCOMPARE(Checkpoints::list(uuid).size(),1); QVERIFY(Checkpoints::list(uuid).first().toMap()["parentId"].toString().isEmpty());
        QVERIFY(QFile::exists(root+"/"+a["id"].toString()+"/disk-0.qcow2"));
        QCOMPARE(capturedCounter(b),after); // Deleted base still supplies the child's immutable sectors.
        command("snapshots.cleanup",{{"uuid",uuid},{"key","deleted:"+a["id"].toString()}}); QVERIFY(!resultOk);
        result=command("snapshots.verify",{{"uuid",uuid},{"id",b["id"]}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        result=command("snapshots.clone",{{"uuid",uuid},{"id",b["id"]},{"name","incremental-clone-"+QUuid::createUuid().toString(QUuid::Id128).left(6)}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        const auto cloneDisk=details(result["uuid"].toString())["disks"].toList().first().toMap()["source"].toString();
        QVERIFY(process("qemu-img",{"compare","-f","qcow2","-F","qcow2",root+"/"+b["id"].toString()+"/disk-0.qcow2",cloneDisk}));
        QCOMPARE(virDomainSuspend(d),0);
        result=command("snapshots.restore",{{"uuid",uuid},{"id",b["id"]},{"allowRestart",true},{"safety",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        int state=0,reason=0; QCOMPARE(virDomainGetState(d,&state,&reason,0),0); QCOMPARE(state,int(VIR_DOMAIN_PAUSED));
        // Restart changes the bitmap epoch; the next capture must start a full base.
        result=command("snapshots.create",{{"uuid",uuid},{"name","New epoch"},{"incremental",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        for(auto v:Checkpoints::list(uuid)) if(v.toMap()["name"]=="New epoch") QCOMPARE(v.toMap()["storageMode"].toString(),"full");
        result=command("snapshots.undo",{{"uuid",uuid},{"allowRestart",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        QCOMPARE(virDomainGetState(d,&state,&reason,0),0); QCOMPARE(state,int(VIR_DOMAIN_PAUSED)); QVERIFY(Checkpoints::undoId(uuid).isEmpty());
        QStringList deleting; for (const auto &v : Checkpoints::list(uuid)) deleting.append(v.toMap()["id"].toString());
        const auto beforeDelete = xml(uuid); const auto deleteRuntime = virDomainGetID(d);
        result=command("snapshots.remove",{{"uuid",uuid},{"id",b["id"]},{"descendants",true},{"allowPinned",true},{"expectedIds",deleting}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        QVERIFY(Checkpoints::list(uuid).isEmpty()); QCOMPARE(xml(uuid),beforeDelete); QCOMPARE(virDomainGetID(d),deleteRuntime);
        QVERIFY(!QFile::exists(root+"/"+a["id"].toString())); QVERIFY(!QFile::exists(root+"/"+b["id"].toString()));
    }
    void checkpointJobsStayResponsive() {
        auto source=makeSource(); QVERIFY(process("qemu-img",{"resize","-f","raw",source,"512M"}));
        QVERIFY(process("qemu-io",{"-f","raw","-c","write -P 0x64 1M 384M",source}));
        auto result=command("vm.create",{{"name","jobs-"+QUuid::createUuid().toString(QUuid::Id128).left(8)},{"sourceMode","disk"},{"source",source},{"preset","generic"},{"firmware","bios"},{"cpus",1},{"memoryMiB",256},{"networkId","none"},{"location",files.path()}});
        QVERIFY2(resultOk,qPrintable(result["message"].toString())); const auto uuid=result["uuid"].toString();
        result=command("snapshots.create",{{"uuid",uuid},{"name","Large verification"}}); QVERIFY2(resultOk,qPrintable(result["message"].toString())); auto cp=Checkpoints::list(uuid).first().toMap();
        QSignalSpy results(backend.get(),&Backend::commandFinished);
        QVERIFY(backend->request("snapshots.verify",{{"uuid",uuid},{"id",cp["id"]}}));
        QVERIFY(backend->checkpointJob()["active"].toBool());
        backend->inspect(uuid); QVERIFY(backend->request("capabilities",{}));
        QTRY_VERIFY_WITH_TIMEOUT(!backend->detailsBusy(),5000); QCOMPARE(backend->details()["uuid"].toString(),uuid);
        bool browsedWhileActive=false;
        QElapsedTimer timer; timer.start();
        while(timer.elapsed()<5000) { for(auto row:results) if(row[0]=="capabilities") browsedWhileActive=backend->checkpointJob()["active"].toBool(); if(browsedWhileActive) break; QTest::qWait(5); }
        QVERIFY(browsedWhileActive); backend->cancelCheckpoint(); QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),10000);
        bool cancelled=false; for(auto row:results) if(row[0]=="snapshots.verify") cancelled=!row[1].toBool() && row[2].toMap()["message"].toString().contains("cancel",Qt::CaseInsensitive); QVERIFY(cancelled);
        backend->action(uuid,"start"); QTRY_VERIFY(active(uuid)); QTRY_VERIFY(!backend->busy());
        auto d=virDomainLookupByUUIDString(external,uuid.toUtf8().constData()); QVERIFY(d); std::unique_ptr<virDomain,decltype(&virDomainFree)> domain(d,virDomainFree);
        auto saved=xml(uuid); auto runtimeId=virDomainGetID(d); std::atomic_bool cancel{false}; QString error; qulonglong reportedTotal=0;
        // Restoring never copies disks; it stays cancellable until the VM is switched.
        bool copied=false;
        QVERIFY(!Checkpoints::restore(external,d,saved,cp["id"].toString(),error,true,[&](QString phase,qulonglong,qulonglong total) { if(phase=="Copying disk") { copied=true; reportedTotal=total; } if(phase.startsWith("Checking restored files")) cancel=true; },&cancel));
        QVERIFY(error.contains("cancel",Qt::CaseInsensitive)); QVERIFY(!copied); QCOMPARE(reportedTotal,qulonglong(0)); QCOMPARE(domainId(uuid),runtimeId); QCOMPARE(xml(uuid),saved);
    }
    void memorySnapshots_data() {
        QTest::addColumn<QString>("firmware");
        QTest::newRow("bios") << QString("bios");
        QTest::newRow("uefi") << QString("uefi");
    }
    void memorySnapshots() {
        QFETCH(QString, firmware);
        const auto fixture = QStringLiteral(QT_TESTCASE_SOURCEDIR) + "/artifacts/agent-fixture/";
        QVERIFY(QFile::exists(fixture + "vmlinuz") && QFile::exists(fixture + "initrd.cpio.gz"));
        const auto source = makeSource(); QVERIFY(process("mkfs.ext4", {"-q", "-F", source}));
        auto result = command("vm.create", {{"name", "memory-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source}, {"preset", "generic"}, {"firmware", firmware}, {"cpus", 1}, {"memoryMiB", 384}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto uuid = result["uuid"].toString();
        QDomDocument doc; QVERIFY(doc.setContent(xml(uuid))); auto os = doc.documentElement().firstChildElement("os");
        for (auto pair : QVariantMap{{"kernel", fixture + "vmlinuz"}, {"initrd", fixture + "initrd.cpio.gz"}, {"cmdline", "console=ttyS0 rdinit=/init panic=-1"}}.toStdMap()) { auto e = doc.createElement(pair.first); e.appendChild(doc.createTextNode(pair.second.toString())); os.appendChild(e); }
        auto devices = doc.documentElement().firstChildElement("devices"), serial = devices.firstChildElement("serial"); serial.setAttribute("type", "file");
        auto output = doc.createElement("source"); output.setAttribute("path", files.filePath("memory-" + uuid + ".log")); serial.appendChild(output); devices.removeChild(devices.firstChildElement("console"));
        auto rawDomain = virDomainDefineXML(external, doc.toString(-1).toUtf8().constData()); QVERIFY(rawDomain);
        std::unique_ptr<virDomain, decltype(&virDomainFree)> domain(rawDomain, virDomainFree); auto d = domain.get(); QCOMPARE(virDomainCreate(d), 0);
        auto agent = [&](QVariantMap input) { const auto json = QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact); char *raw = virDomainQemuAgentCommand(d, json.constData(), 5, 0); auto answer = raw ? QJsonDocument::fromJson(raw).toVariant().toMap() : QVariantMap{}; free(raw); return answer; };
        auto guest = [&](QString script) {
            auto result = agent({{"execute", "guest-exec"}, {"arguments", QVariantMap{{"path", "/bin/sh"}, {"arg", QStringList{"-c", script}}, {"capture-output", true}}}});
            const auto pid = result["return"].toMap()["pid"]; if (!pid.isValid()) return QString{};
            QElapsedTimer timer; timer.start();
            while (timer.elapsed() < 5000) {
                const auto state = agent({{"execute", "guest-exec-status"}, {"arguments", QVariantMap{{"pid", pid}}}})["return"].toMap();
                if (state["exited"].toBool()) return QString::fromUtf8(QByteArray::fromBase64(state["out-data"].toByteArray())).trimmed();
                QTest::qWait(30);
            }
            return QString{};
        };
        QTRY_VERIFY_WITH_TIMEOUT(agent({{"execute", "guest-ping"}}).contains("return"), 20000);
        const auto bootId = guest("cat /proc/sys/kernel/random/boot_id"); QCOMPARE(bootId.size(), 36);
        auto setState = [&](QString value) { return guest("echo " + value + " > /tmp/memory-state; echo " + value + " > /data/disk-state; sync; cat /tmp/memory-state"); };
        auto point = [&](QString name) { for (auto value : Checkpoints::list(uuid)) if (value.toMap()["name"] == name) return value.toMap(); return QVariantMap{}; };
        QCOMPARE(setState("one"), "one");
        // A preceding disk-only bitmap must not prevent full-state capture.
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Disk base"}, {"incremental", true}}); QVERIFY(resultOk);
        const auto liveId = domainId(uuid);
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Memory A"}, {"memory", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QCOMPARE(domainId(uuid), liveId); QCOMPARE(guest("cat /proc/sys/kernel/random/boot_id"), bootId);
        auto a = point("Memory A"); QCOMPARE(a["memory"].toString(), "memory.save"); QCOMPARE(a["consistency"].toString(), "full-system"); QVERIFY(a["bytes"].toULongLong() > 1048576);
        QCOMPARE(setState("two"), "two");
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Memory B"}, {"memory", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto b = point("Memory B");
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", a["id"]}}); QVERIFY(!resultOk); QCOMPARE(domainId(uuid), liveId);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", a["id"]}, {"allowRestart", true}, {"safety", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QVERIFY(result["restoredMemory"].toBool());
        QTRY_COMPARE_WITH_TIMEOUT(guest("cat /tmp/memory-state; cat /data/disk-state; cat /proc/sys/kernel/random/boot_id"), "one\none\n" + bootId, 20000);
        result = command("snapshots.undo", {{"uuid", uuid}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QTRY_COMPARE_WITH_TIMEOUT(guest("cat /tmp/memory-state; cat /data/disk-state; cat /proc/sys/kernel/random/boot_id"), "two\ntwo\n" + bootId, 20000);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", a["id"]}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QTRY_COMPARE_WITH_TIMEOUT(setState("three"), "three", 20000);
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Memory C"}, {"memory", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QCOMPARE(point("Memory C")["parentId"], a["id"]); QCOMPARE(b["parentId"], a["id"]);
        // Paused snapshots restore paused, preserving exactly the same boot/session.
        QCOMPARE(virDomainSuspend(d), 0);
        result = command("snapshots.create", {{"uuid", uuid}, {"name", "Paused memory"}, {"memory", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto paused = point("Paused memory");
        QCOMPARE(virDomainResume(d), 0); QCOMPARE(setState("four"), "four");
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", paused["id"]}, {"allowRestart", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        int state = 0, reason = 0; QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, int(VIR_DOMAIN_PAUSED)); QCOMPARE(virDomainResume(d), 0);
        QTRY_COMPARE_WITH_TIMEOUT(guest("cat /tmp/memory-state; cat /data/disk-state; cat /proc/sys/kernel/random/boot_id"), "three\nthree\n" + bootId, 20000);
        const auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + uuid;
        const auto memoryFile = root + "/" + a["id"].toString() + "/memory.save";
        const auto beforeMissing = domainId(uuid); QVERIFY(QFile::rename(memoryFile, memoryFile + ".missing"));
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", a["id"]}, {"allowRestart", true}}); QVERIFY(!resultOk); QCOMPARE(domainId(uuid), beforeMissing); QVERIFY(QFile::rename(memoryFile + ".missing", memoryFile));
        // Invalid device/RAM payload after a valid header triggers recovery of the
        // previous RAM AND matching disks, rather than an unexpected cold boot.
        const auto preserved = files.filePath("memory-original-" + uuid); QVERIFY(QFile::copy(memoryFile, preserved));
        { QFile broken(memoryFile); QVERIFY(broken.open(QIODevice::ReadWrite)); QVERIFY(broken.resize(4 * 1024 * 1024)); }
        QCOMPARE(setState("rollback"), "rollback");
        const auto beforeFailure = domainId(uuid);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", a["id"]}, {"allowRestart", true}, {"safety", true}}); QVERIFY(!resultOk); QVERIFY2(active(uuid), qPrintable(result["message"].toString()));
        QVERIFY2(domainId(uuid) != beforeFailure, qPrintable(result["message"].toString()));
        QVERIFY2(result["message"].toString().contains("recovered without rebooting"), qPrintable(result["message"].toString()));
        QTRY_COMPARE_WITH_TIMEOUT(guest("cat /tmp/memory-state; cat /data/disk-state; cat /proc/sys/kernel/random/boot_id"), "rollback\nrollback\n" + bootId, 20000);
        QVERIFY(QFile::remove(memoryFile)); QVERIFY(QFile::rename(preserved, memoryFile));
        result = command("snapshots.verify", {{"uuid", uuid}, {"id", a["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QFile manifest(root + "/" + a["id"].toString() + "/manifest.json"); QVERIFY(manifest.open(QIODevice::ReadOnly)); QVERIFY(QJsonDocument::fromJson(manifest.readAll()).toVariant().toMap()["hashes"].toMap().contains("memory.save"));
        QCOMPARE(virDomainDestroy(d), 0);
        result = command("snapshots.restore", {{"uuid", uuid}, {"id", a["id"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QVERIFY(active(uuid));
        QTRY_COMPARE_WITH_TIMEOUT(guest("cat /tmp/memory-state; cat /data/disk-state; cat /proc/sys/kernel/random/boot_id"), "one\none\n" + bootId, 20000);
        const auto count = Checkpoints::list(uuid).size(); const auto currentId = domainId(uuid); std::atomic_bool cancel{false}; QString failure;
        QVERIFY(!Checkpoints::createLive(d, xml(uuid), "Cancelled memory", "", cancel, [&](QString phase, qulonglong, qulonglong) { if (phase == "Saving VM memory and device state") cancel = true; }, failure, {{"memory", true}}));
        QCOMPARE(Checkpoints::list(uuid).size(), count); QCOMPARE(domainId(uuid), currentId); QCOMPARE(virDomainGetState(d, &state, &reason, 0), 0); QCOMPARE(state, int(VIR_DOMAIN_RUNNING)); QVERIFY(!QFile::exists(root + "/freeze.json"));
    }
    void cleanerCheckpointAndRecovery() {
        const auto fixture=QStringLiteral(QT_TESTCASE_SOURCEDIR)+"/artifacts/agent-fixture/";
        QVERIFY2(QFile::exists(fixture+"vmlinuz") && QFile::exists(fixture+"initrd.cpio.gz"),"Build tests/fixtures/make-agent-initrd.py on the test machine and copy its test kernel first.");
        const auto source=makeSource(); QVERIFY(process("mkfs.ext4",{"-q","-F",source}));
        auto result=command("vm.create",{{"name","agent-"+QUuid::createUuid().toString(QUuid::Id128).left(8)},{"sourceMode","disk"},{"source",source},{"preset","generic"},{"firmware","bios"},{"cpus",1},{"memoryMiB",384},{"networkId","none"},{"location",files.path()}});
        QVERIFY2(resultOk,qPrintable(result["message"].toString())); const auto uuid=result["uuid"].toString();
        QDomDocument doc; QVERIFY(doc.setContent(xml(uuid))); auto os=doc.documentElement().firstChildElement("os");
        for(auto pair:QVariantMap{{"kernel",fixture+"vmlinuz"},{"initrd",fixture+"initrd.cpio.gz"},{"cmdline","console=ttyS0 rdinit=/init panic=-1"}}.toStdMap()) { auto e=doc.createElement(pair.first); e.appendChild(doc.createTextNode(pair.second.toString())); os.appendChild(e); }
        auto devices=doc.documentElement().firstChildElement("devices"); auto serial=devices.firstChildElement("serial"); serial.setAttribute("type","file");
        auto output=doc.createElement("source"); output.setAttribute("path",QStringLiteral(QT_TESTCASE_SOURCEDIR)+"/artifacts/agent-fixture/serial.log"); serial.appendChild(output); devices.removeChild(devices.firstChildElement("console"));
        auto d=virDomainDefineXML(external,doc.toString(-1).toUtf8().constData()); QVERIFY(d); std::unique_ptr<virDomain,decltype(&virDomainFree)> domain(d,virDomainFree);
        QCOMPARE(virDomainCreate(d),0); QTRY_VERIFY_WITH_TIMEOUT(details(uuid,true)["agentConnected"].toBool(),20000);
        auto agent=[&](QString json) { char *raw=virDomainQemuAgentCommand(d,json.toUtf8().constData(),5,0); auto data=raw?QJsonDocument::fromJson(raw).toVariant().toMap():QVariantMap{}; free(raw); return data; };
        QTRY_COMPARE(agent("{\"execute\":\"guest-fsfreeze-status\"}")["return"].toString(),"thawed");
        const auto runtimeId=virDomainGetID(d);
        result=command("snapshots.create",{{"uuid",uuid},{"name","Guest filesystems flushed"},{"incremental",true},{"clean",true}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        QCOMPARE(virDomainGetID(d),runtimeId); QCOMPARE(agent("{\"execute\":\"guest-fsfreeze-status\"}")["return"].toString(),"thawed");
        auto checkpoint=Checkpoints::list(uuid).first().toMap(); QCOMPARE(checkpoint["consistency"].toString(),"filesystem-consistent");
        QVERIFY(virDomainFSFreeze(d,nullptr,0,0)>0);
        result=command("snapshots.create",{{"uuid",uuid},{"name","Externally frozen"},{"clean",true}}); QVERIFY(!resultOk); QCOMPARE(agent("{\"execute\":\"guest-fsfreeze-status\"}")["return"].toString(),"frozen");
        QVERIFY(virDomainFSThaw(d,nullptr,0,0)>0);
        // Reconcile an interrupted capture, including its brief UEFI-style pause.
        QVERIFY(virDomainFSFreeze(d,nullptr,0,0)>0); QCOMPARE(virDomainSuspend(d),0);
        const auto root=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/checkpoints/"+uuid;
        QFile journal(root+"/freeze.json"); QVERIFY(journal.open(QIODevice::WriteOnly)); QVERIFY(journal.write(QJsonDocument::fromVariant(QVariantMap{{"uuid",uuid},{"runtimeId",runtimeId},{"frozen",true},{"paused",true}}).toJson())>0); journal.close();
        result=command("snapshots.recover",{{"uuid",uuid}}); QVERIFY2(resultOk,qPrintable(result["message"].toString()));
        QCOMPARE(agent("{\"execute\":\"guest-fsfreeze-status\"}")["return"].toString(),"thawed"); int state=0,reason=0; QCOMPARE(virDomainGetState(d,&state,&reason,0),0); QCOMPARE(state,int(VIR_DOMAIN_RUNNING)); QVERIFY(!QFile::exists(root+"/freeze.json"));
        // Read the captured filesystem through an independent raw export.
        const auto raw=files.filePath("clean-capture.raw"); QVERIFY(process("qemu-img",{"convert","-f","qcow2","-O","raw",root+"/"+checkpoint["id"].toString()+"/disk-0.qcow2",raw}));
        QVERIFY(process("e2fsck",{"-fn",raw}));
        QProcess counter; counter.start("debugfs",{"-R","cat /counter",raw}); QVERIFY(counter.waitForFinished(10000)); QVERIFY(counter.readAllStandardOutput().trimmed().toInt()>0);
    }
    void importRejectsDependentImages() {
        // A crafted qcow2 whose backing file is a host file would copy that file into the new VM's disk.
        const auto secret = files.filePath("host-secret.bin");
        { QFile f(secret); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(65536, 'S')); }
        const auto hostile = files.filePath("hostile.qcow2");
        QVERIFY(process("qemu-img", {"create", "-q", "-f", "qcow2", "-b", secret, "-F", "raw", hostile}));
        const auto tag = QUuid::createUuid().toString(QUuid::Id128).left(6);
        auto request = [&](const QString &name, const QString &source) {
            return command("vm.create", {{"name", name}, {"sourceMode", "disk"}, {"source", source}, {"preset", "generic"}, {"firmware", "bios"}, {"cpus", 1}, {"memoryMiB", 256}, {"networkId", "none"}, {"location", files.path()}});
        };
        auto result = request("hostile-" + tag, hostile);
        QVERIFY(!resultOk); QVERIFY2(result["message"].toString().contains("backing"), qPrintable(result["message"].toString()));
        // The format comes from the file's header, not its name, so a renamed image is refused too.
        const auto disguised = files.filePath("disguised.img"); QVERIFY(QFile::copy(hostile, disguised));
        result = request("disguised-" + tag, disguised); QVERIFY(!resultOk);
        // A standalone qcow2 still imports.
        const auto standalone = files.filePath("standalone.qcow2");
        QVERIFY(process("qemu-img", {"convert", "-q", "-f", "raw", "-O", "qcow2", makeSource(), standalone}));
        result = request("standalone-" + tag, standalone); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QTRY_VERIFY(!backend->busy());
    }
    void containmentEnforcement() {
        backend->createTest(); QTRY_VERIFY(!backend->busy()); QVERIFY(!created.isEmpty()); const auto uuid = created.last();
        auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(d);
        std::unique_ptr<virDomain, decltype(&virDomainFree)> domain(d, virDomainFree);
        virDomainSetAutostart(d, 1);
        // A VM with no network can be contained; autostart is turned off because it would bypass OmaWare.
        auto result = command("containment.set", {{"uuid", uuid}, {"enabled", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(Containment::enabled(xml(uuid))); int autostart = 1; QCOMPARE(virDomainGetAutostart(d, &autostart), 0); QCOMPARE(autostart, 0);
        QTRY_VERIFY(!backend->busy());
        // The UI path cannot attach NAT to a contained VM.
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy());
        QVERIFY(backend->details()["containment"].toMap()["enabled"].toBool());
        QSignalSpy configured(backend.get(), &Backend::networkConfigured);
        QVERIFY(backend->configureNetwork(uuid, "", "user", "virtio", true, false, backend->details()["revision"].toString()));
        QTRY_VERIFY(!configured.isEmpty()); QVERIFY(!configured.last()[1].toBool()); QVERIFY(configured.last()[2].toString().contains("contained"));
        QVERIFY(details(uuid)["interfaces"].toList().isEmpty());
        // Hardware edits keep the marker and cannot enable clipboard sharing.
        result = command("hardware.save", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"cpus", 1}, {"memoryMiB", 256}, {"clipboard", true}});
        QVERIFY(!resultOk); QVERIFY(result["message"].toString().contains("contained")); QVERIFY(!details(uuid)["clipboardConfigured"].toBool());
        result = command("hardware.save", {{"uuid", uuid}, {"revision", details(uuid)["revision"]}, {"cpus", 1}, {"memoryMiB", 320}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); QVERIFY(Containment::enabled(xml(uuid)));
        QTRY_VERIFY(!backend->busy());
        // A compliant contained VM starts.
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        // Tampering outside OmaWare (NAT added with virsh-equivalent API) is caught at start time.
        QCOMPARE(virDomainAttachDeviceFlags(d, "<interface type='user'><model type='virtio'/></interface>", VIR_DOMAIN_AFFECT_CONFIG), 0);
        QSignalSpy finished(backend.get(), &Backend::operationFinished);
        backend->action(uuid, "start"); QTRY_VERIFY(!finished.isEmpty()); QTRY_VERIFY(!backend->busy());
        QVERIFY(!finished.last()[1].toBool()); QVERIFY(finished.last()[0].toString().contains("Contained VM blocked")); QVERIFY(!active(uuid));
        result = command("vm.restart", {{"uuid", uuid}}); QVERIFY(!resultOk); QVERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy());
        QVERIFY(!backend->details()["containment"].toMap()["violations"].toList().isEmpty());
        // A non-compliant VM cannot be (re)contained; releasing containment is explicit.
        result = command("containment.set", {{"uuid", uuid}, {"enabled", false}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        QVERIFY(!Containment::enabled(xml(uuid))); QTRY_VERIFY(!backend->busy());
        result = command("containment.set", {{"uuid", uuid}, {"enabled", true}}); QVERIFY(!resultOk); QVERIFY(result["message"].toString().contains("private internet"));
        QVERIFY(!Containment::enabled(xml(uuid))); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
    }
    void cableLinks() {
        backend->createTest(); QTRY_VERIFY(!backend->busy()); QVERIFY(!created.isEmpty()); const auto uuid = created.last();
        QSignalSpy configured(backend.get(), &Backend::networkConfigured), links(backend.get(), &Backend::linksSet);
        QVERIFY(backend->configureNetwork(uuid, "", "user", "virtio", true, false, DomainConfig::revision(xml(uuid))));
        QTRY_VERIFY(!configured.isEmpty()); QVERIFY2(configured.last()[1].toBool(), qPrintable(configured.last()[2].toString()));
        const auto mac = details(uuid)["interfaces"].toList().first().toMap()["mac"].toString();
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        // Pulling a cable is live and saved, and is not listed as a pending restart change.
        backend->setLinks({QVariantMap{{"uuid", uuid}, {"mac", mac}}}, false);
        QTRY_VERIFY(!links.isEmpty()); QVERIFY2(links.last()[0].toBool(), qPrintable(links.last()[1].toString()));
        QVERIFY(!details(uuid, true)["interfaces"].toList().first().toMap()["linkUp"].toBool());
        QVERIFY(!details(uuid)["interfaces"].toList().first().toMap()["linkUp"].toBool());
        QVERIFY(PendingChanges(uuid).items(xml(uuid)).isEmpty());
        links.clear(); backend->setLinks({QVariantMap{{"uuid", uuid}, {"mac", mac}}}, true);
        QTRY_VERIFY(!links.isEmpty()); QVERIFY(links.last()[0].toBool());
        QVERIFY(details(uuid, true)["interfaces"].toList().first().toMap()["linkUp"].toBool());
        // Unknown adapters are reported, not ignored.
        links.clear(); backend->setLinks({QVariantMap{{"uuid", uuid}, {"mac", "52:54:00:00:00:01"}}}, false);
        QTRY_VERIFY(!links.isEmpty()); QVERIFY(!links.last()[0].toBool());
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        // A contained VM's cable cannot be plugged back into a NAT connection, even if it was added behind OmaWare's back.
        QVERIFY(backend->configureNetwork(uuid, mac, "", "virtio", true, true, DomainConfig::revision(xml(uuid))));
        QTRY_VERIFY(!backend->busy()); QVERIFY(details(uuid)["interfaces"].toList().isEmpty());
        auto result = command("containment.set", {{"uuid", uuid}, {"enabled", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QTRY_VERIFY(!backend->busy());
        auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); QVERIFY(d);
        QCOMPARE(virDomainAttachDeviceFlags(d, "<interface type='user'><mac address='52:54:00:6f:6d:61'/><model type='virtio'/><link state='down'/></interface>", VIR_DOMAIN_AFFECT_CONFIG), 0);
        virDomainFree(d);
        links.clear(); backend->setLinks({QVariantMap{{"uuid", uuid}, {"mac", "52:54:00:6f:6d:61"}}}, true);
        QTRY_VERIFY(!links.isEmpty()); QVERIFY(!links.last()[0].toBool()); QVERIFY(links.last()[1].toString().contains("contained"));
        QVERIFY(!details(uuid)["interfaces"].toList().first().toMap()["linkUp"].toBool());
    }
    void pauseOnCloseAndBulkPower() {
        for (int i = 0; i < 2; ++i) { backend->createTest(); QTRY_VERIFY(!backend->busy()); }
        QCOMPARE(created.size(), 2); const auto a = created[0], b = created[1];
        auto state = [&](QString uuid) { auto d = virDomainLookupByUUIDString(external, uuid.toUtf8().constData()); int s = -1, r = 0; if (d) { virDomainGetState(d, &s, &r, 0); virDomainFree(d); } return s; };
        backend->bulkAction({a, b}, "power-on");
        QTRY_VERIFY_WITH_TIMEOUT(state(a) == VIR_DOMAIN_RUNNING && state(b) == VIR_DOMAIN_RUNNING, 15000); QTRY_VERIFY(!backend->busy());
        QGuiApplication::setQuitOnLastWindowClosed(false);
        Theme theme("/nonexistent/palette");
        auto open = [&](QQmlApplicationEngine &engine) {
            engine.rootContext()->setContextProperty("backend", backend.get()); engine.rootContext()->setContextProperty("theme", &theme);
            engine.load(QUrl("qrc:/qml/Main.qml"));
            auto window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (window) { window->resize(1280, 840); window->show(); }
            return window;
        };
        auto item = [](QQuickItem *root, QString name) {
            std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *parent) -> QQuickItem * {
                if (parent->objectName() == name) return parent;
                for (auto child : parent->childItems()) if (auto match = find(child)) return match;
                return nullptr;
            };
            return root ? find(root) : nullptr;
        };
        auto press = [&](QQuickWindow *window, QString name, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            auto target = item(window->contentItem()->parentItem() ? window->contentItem()->parentItem() : window->contentItem(), name);
            if (!target || !target->isVisible() || !target->isEnabled()) return false;
            QTest::mouseClick(window, Qt::LeftButton, modifiers, target->mapToScene({target->width() / 2, target->height() / 2}).toPoint()); return true;
        };
        auto ready = [&](QQuickWindow *window, QString name) { auto target = item(window->contentItem(), name); return target && target->isVisible() && target->isEnabled(); };
        auto capture = [&](QQuickWindow *window, QString name) { QTest::qWait(200); auto dir = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR"); return dir.isEmpty() || window->grabWindow().save(dir + "/" + name + ".png"); };
        QList<QQmlError> warnings;
        {
            QQmlApplicationEngine engine; connect(&engine, &QQmlEngine::warnings, this, [&](QList<QQmlError> items) { warnings += items; });
            auto window = open(engine); QVERIFY(window);
            QTRY_VERIFY(item(window->contentItem(), "vmRow_" + b));
            // Ctrl-click picks several VMs; the bulk bar acts on all of them.
            window->setProperty("selectedUuid", a); QTRY_VERIFY(item(window->contentItem(), "vmRow_" + b));
            QVERIFY(press(window, "vmRow_" + b, Qt::ControlModifier));
            QTRY_COMPARE(window->property("marked").toList().size(), 2);
            QVERIFY(item(window->contentItem(), "bulkBar")->isVisible());
            QCOMPARE(item(window->contentItem(), "bulkCount")->property("text").toString(), "2 selected");
            QVERIFY(capture(window, "bulk-selection"));
            QTRY_VERIFY(ready(window, "bulkPause")); QVERIFY(press(window, "bulkPause"));
            QTRY_VERIFY(state(a) == VIR_DOMAIN_PAUSED && state(b) == VIR_DOMAIN_PAUSED); QTRY_VERIFY(!backend->busy());
            QTRY_VERIFY(ready(window, "bulkPowerOn")); QVERIFY(press(window, "bulkPowerOn"));
            QTRY_VERIFY(state(a) == VIR_DOMAIN_RUNNING && state(b) == VIR_DOMAIN_RUNNING); QTRY_VERIFY(!backend->busy());
            QTRY_VERIFY(item(window->contentItem(), "bulkForceOff")->isEnabled());
            QVERIFY(press(window, "bulkClear")); QTRY_VERIFY(window->property("marked").toList().isEmpty());
            // Closing pauses running VMs first, then the window closes.
            QTRY_VERIFY(!backend->domains().isEmpty());
            window->close();
            QTRY_VERIFY(!window->isVisible());
            QCOMPARE(state(a), VIR_DOMAIN_PAUSED); QCOMPARE(state(b), VIR_DOMAIN_PAUSED);
        }
        {
            QQmlApplicationEngine engine; connect(&engine, &QQmlEngine::warnings, this, [&](QList<QQmlError> items) { warnings += items; });
            auto window = open(engine); QVERIFY(window);
            // Next launch: paused VMs are listed with a banner to resume them.
            QTRY_VERIFY(item(window->contentItem(), "exitPausedBanner") && item(window->contentItem(), "exitPausedBanner")->isVisible());
            QCOMPARE(window->property("pausedOnExit").toList().size(), 2);
            QVERIFY(capture(window, "paused-on-exit"));
            QVERIFY(press(window, "choosePaused")); QTRY_COMPARE(window->property("marked").toList().size(), 2);
            QTRY_VERIFY(ready(window, "resumeAllPaused")); QVERIFY(press(window, "resumeAllPaused"));
            QTRY_VERIFY(state(a) == VIR_DOMAIN_RUNNING && state(b) == VIR_DOMAIN_RUNNING); QTRY_VERIFY(!backend->busy());
            QTRY_VERIFY(!item(window->contentItem(), "exitPausedBanner")->isVisible());
            QTRY_VERIFY(window->property("exitPaused").toList().isEmpty());
            // There is no setting to leave VMs running: closing always pauses them. (The test skips it
            // here only to keep both VMs running for the update below.)
            QVERIFY(!item(window->contentItem()->parentItem(), "closeMode"));
            window->setProperty("exiting", true);
            window->close(); QTRY_VERIFY(!window->isVisible());
            QCOMPARE(state(a), VIR_DOMAIN_RUNNING);
        }
        {
            // Updating: a button checks GitHub (a local feed here), the dialog warns that running VMs will be
            // paused, and "Update now" downloads, pauses them, swaps the new version in and restarts on the same page.
            QQmlApplicationEngine engine; connect(&engine, &QQmlEngine::warnings, this, [&](QList<QQmlError> items) { warnings += items; });
            auto window = open(engine); QVERIFY(window);
            QTRY_VERIFY(item(window->contentItem(), "vmRow_" + b));
            window->setProperty("navigation", "networks");
            QTemporaryDir release; QVERIFY(release.isValid());
            auto write = [](const QString &path, const QByteArray &data, bool executable = false) {
                QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); f.open(QIODevice::WriteOnly); f.write(data); f.close();
                if (executable) f.setPermissions(f.permissions() | QFile::ExeOwner);
            };
            auto sums = [](const QString &root, const QStringList &names) {
                QByteArray out;
                for (const auto &name : names) { QFile f(root + "/" + name); f.open(QIODevice::ReadOnly); out += QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex() + "  " + name.toUtf8() + "\n"; }
                QFile list(root + "/SHA256SUMS"); list.open(QIODevice::WriteOnly); list.write(out);
            };
            const auto app = release.filePath("app"), marker = release.filePath("started");
            write(app + "/omaware", "old", true); sums(app, {"omaware"});
            const auto stage = release.filePath("build/omaware-9.9.9-linux-x86_64");
            write(stage + "/omaware", "new", true); write(stage + "/omaware.sh", ("#!/bin/sh\necho \"$@\" > '" + marker + "'\n").toUtf8(), true);
            sums(stage, {"omaware", "omaware.sh"});
            QProcess tar; tar.start("tar", {"-czf", release.filePath("omaware-9.9.9-linux-x86_64.tar.gz"), "-C", release.filePath("build"), "omaware-9.9.9-linux-x86_64"}); QVERIFY(tar.waitForFinished()); QCOMPARE(tar.exitCode(), 0);
            QFile package(release.filePath("omaware-9.9.9-linux-x86_64.tar.gz")); QVERIFY(package.open(QIODevice::ReadOnly));
            write(release.filePath("SHA256SUMS"), QCryptographicHash::hash(package.readAll(), QCryptographicHash::Sha256).toHex() + "  omaware-9.9.9-linux-x86_64.tar.gz\n");
            write(release.filePath("release.json"), QJsonDocument(QJsonObject{{"tag_name", "v9.9.9"}, {"html_url", "https://example/release"}, {"body", "## Changes\n- Something new"},
                {"assets", QJsonArray{QJsonObject{{"name", "omaware-9.9.9-linux-x86_64.tar.gz"}, {"browser_download_url", QUrl::fromLocalFile(release.filePath("omaware-9.9.9-linux-x86_64.tar.gz")).toString()}},
                                      QJsonObject{{"name", "SHA256SUMS"}, {"browser_download_url", QUrl::fromLocalFile(release.filePath("SHA256SUMS")).toString()}}}}}).toJson());
            auto updater = qobject_cast<Updater *>(window->findChild<QObject *>("updater")); QVERIFY(updater);
            updater->setAppDir(app); updater->setCurrent("1.0.0"); updater->setFeed(QUrl::fromLocalFile(release.filePath("release.json")));
            // The sidebar button checks and shows the dialog with the warning; nothing changes until "Update now".
            QVERIFY(press(window, "checkUpdates"));
            QTRY_COMPARE(updater->status(), QString("available"));
            auto dialog = window->findChild<QObject *>("updateDialog"); QVERIFY(dialog); QTRY_VERIFY(dialog->property("opened").toBool());
            const auto warning = dialog->findChild<QObject *>("updateWarningText")->property("text").toString();
            QVERIFY2(warning.contains("2 running VMs will be paused"), qPrintable(warning));
            QVERIFY(capture(window, "update-prompt"));
            QTest::qWait(300); QCOMPARE(state(a), VIR_DOMAIN_RUNNING);
            QTRY_VERIFY(ready(window, "updateNow")); QVERIFY(press(window, "updateNow"));
            QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 20000);
            QCOMPARE(state(a), VIR_DOMAIN_PAUSED); QCOMPARE(state(b), VIR_DOMAIN_PAUSED);
            QTRY_VERIFY(QFile::exists(marker));
            QFile started(marker); QVERIFY(started.open(QIODevice::ReadOnly)); QCOMPARE(started.readAll().trimmed(), QByteArray("--restarted"));
            QFile installed(app + "/omaware"); QVERIFY(installed.open(QIODevice::ReadOnly)); QCOMPARE(installed.readAll(), QByteArray("new"));
            QCOMPARE(Workspace().get("restoreNavigation").toString(), QString("networks"));
            QCOMPARE(QJsonDocument::fromJson(Workspace().get("exitPaused").toString().toUtf8()).array().size(), 2);
        }
        {
            // The new copy reopens on that page.
            QQmlApplicationEngine engine; connect(&engine, &QQmlEngine::warnings, this, [&](QList<QQmlError> items) { warnings += items; });
            auto window = open(engine); QVERIFY(window);
            window->setProperty("restarted", true);
            QTRY_COMPARE(window->property("navigation").toString(), QString("networks"));
            // The VMs paused for the update are offered for resuming.
            QTRY_VERIFY(item(window->contentItem(), "exitPausedBanner") && item(window->contentItem(), "exitPausedBanner")->isVisible());
            // The Networks page refreshes as it opens; wait until that's done so the click isn't ignored.
            QTRY_VERIFY(!backend->busy() && ready(window, "resumeAllPaused")); QTest::qWait(500);
            QTRY_VERIFY(!backend->busy()); QVERIFY(press(window, "resumeAllPaused"));
            QTRY_VERIFY2(state(a) == VIR_DOMAIN_RUNNING && state(b) == VIR_DOMAIN_RUNNING, qPrintable(QString("%1 %2 busy=%3 marked=%4 error=%5").arg(state(a)).arg(state(b)).arg(backend->busy()).arg(window->property("marked").toList().size()).arg(window->property("operationError").toString())));
            QTRY_VERIFY(!backend->busy());
            // Closing pauses them again.
            window->close(); QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 20000);
            QCOMPARE(state(a), VIR_DOMAIN_PAUSED); QCOMPARE(state(b), VIR_DOMAIN_PAUSED);
        }
        QGuiApplication::setQuitOnLastWindowClosed(true);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString{} : warnings.first().toString()));
    }
    void liveNetworkChanges() {
        // A small Linux guest acknowledges PCI hot-plug, so adapter changes apply to the running VM.
        const auto fixture = QStringLiteral(QT_TESTCASE_SOURCEDIR) + "/artifacts/agent-fixture/";
        QVERIFY(QFile::exists(fixture + "vmlinuz") && QFile::exists(fixture + "initrd.cpio.gz"));
        const auto source = makeSource(); QVERIFY(process("mkfs.ext4", {"-q", "-F", source}));
        auto result = command("vm.create", {{"name", "net-" + QUuid::createUuid().toString(QUuid::Id128).left(8)}, {"sourceMode", "disk"}, {"source", source}, {"preset", "generic"}, {"firmware", "bios"}, {"cpus", 1}, {"memoryMiB", 384}, {"networkId", "none"}, {"location", files.path()}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); const auto uuid = result["uuid"].toString(); created << uuid;
        QDomDocument doc; QVERIFY(doc.setContent(xml(uuid))); auto os = doc.documentElement().firstChildElement("os");
        for (auto pair : QVariantMap{{"kernel", fixture + "vmlinuz"}, {"initrd", fixture + "initrd.cpio.gz"}, {"cmdline", "console=ttyS0 rdinit=/init panic=-1"}}.toStdMap()) { auto e = doc.createElement(pair.first); e.appendChild(doc.createTextNode(pair.second.toString())); os.appendChild(e); }
        auto d = virDomainDefineXML(external, doc.toString(-1).toUtf8().constData()); QVERIFY(d); QCOMPARE(virDomainCreate(d), 0);
        auto agentUp = [&] { char *raw = virDomainQemuAgentCommand(d, "{\"execute\":\"guest-ping\"}", 5, 0); const bool ok = raw != nullptr; free(raw); return ok; };
        QTRY_VERIFY_WITH_TIMEOUT(agentUp(), 30000);
        auto liveNics = [&] { return details(uuid, true)["interfaces"].toList(); };
        QSignalSpy configured(backend.get(), &Backend::networkConfigured);
        // Adding an adapter plugs it into the running VM, and it is not listed as a pending restart change.
        QVERIFY(backend->configureNetwork(uuid, "", "user", "virtio", true, false, DomainConfig::revision(xml(uuid))));
        QTRY_VERIFY_WITH_TIMEOUT(!configured.isEmpty(), 20000); QVERIFY2(configured.last()[1].toBool(), qPrintable(configured.last()[2].toString()));
        QVERIFY2(configured.last()[2].toString().contains("running VM"), qPrintable(configured.last()[2].toString()));
        QCOMPARE(liveNics().size(), 1); const auto mac = liveNics().first().toMap()["mac"].toString();
        QCOMPARE(details(uuid)["interfaces"].toList().first().toMap()["mac"].toString(), mac);
        QVERIFY(PendingChanges(uuid).items(xml(uuid)).isEmpty());
        // Changing the adapter model needs an unplug and replug; the guest releases the old device.
        configured.clear(); QTRY_VERIFY(!backend->busy());
        QVERIFY(backend->configureNetwork(uuid, mac, "user", "e1000", true, false, DomainConfig::revision(xml(uuid))));
        QTRY_VERIFY_WITH_TIMEOUT(!configured.isEmpty(), 20000); QVERIFY2(configured.last()[1].toBool(), qPrintable(configured.last()[2].toString()));
        QVERIFY2(configured.last()[2].toString().contains("running VM"), qPrintable(configured.last()[2].toString()));
        QCOMPARE(liveNics().size(), 1); QCOMPARE(liveNics().first().toMap()["model"].toString(), "e1000"); QCOMPARE(liveNics().first().toMap()["mac"].toString(), mac);
        QVERIFY(PendingChanges(uuid).items(xml(uuid)).isEmpty());
        // Removing it unplugs it live.
        configured.clear(); QTRY_VERIFY(!backend->busy());
        QVERIFY(backend->configureNetwork(uuid, mac, "", "virtio", true, true, DomainConfig::revision(xml(uuid))));
        QTRY_VERIFY_WITH_TIMEOUT(!configured.isEmpty(), 20000); QVERIFY2(configured.last()[1].toBool(), qPrintable(configured.last()[2].toString()));
        QVERIFY(liveNics().isEmpty()); QVERIFY(details(uuid)["interfaces"].toList().isEmpty());
        QVERIFY(PendingChanges(uuid).items(xml(uuid)).isEmpty());
        // Several VMs can be connected in one step.
        configured.clear(); QTRY_VERIFY(!backend->busy());
        QVERIFY(backend->connectVms({uuid}, "user"));
        QTRY_VERIFY_WITH_TIMEOUT(!configured.isEmpty(), 20000); QVERIFY2(configured.last()[1].toBool(), qPrintable(configured.last()[2].toString()));
        QCOMPARE(liveNics().size(), 1);
        // IP addresses are reported per MAC in the network list (none are known for a private NAT adapter).
        result = command("networks.list"); QVERIFY(resultOk);
        bool listed = false;
        for (auto row : result["topology"].toList()) if (row.toMap()["uuid"] == uuid) { listed = true; QVERIFY(row.toMap().contains("addresses")); }
        QVERIFY(listed);
        virDomainDestroy(d); virDomainFree(d);
    }
    void liveChangeFallsBackWithoutGuest() {
        // With no guest OS to acknowledge the unplug, a removal is saved for the next start instead.
        backend->createTest(); QTRY_VERIFY(!backend->busy()); const auto uuid = created.last();
        QSignalSpy configured(backend.get(), &Backend::networkConfigured);
        QVERIFY(backend->configureNetwork(uuid, "", "user", "virtio", true, false, DomainConfig::revision(xml(uuid))));
        QTRY_VERIFY(!configured.isEmpty()); QVERIFY(configured.last()[1].toBool());
        const auto mac = details(uuid)["interfaces"].toList().first().toMap()["mac"].toString();
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        configured.clear();
        QVERIFY(backend->configureNetwork(uuid, mac, "", "virtio", true, true, DomainConfig::revision(xml(uuid))));
        QTRY_VERIFY_WITH_TIMEOUT(!configured.isEmpty(), 30000); QVERIFY(configured.last()[1].toBool());
        QVERIFY2(configured.last()[2].toString().contains("full shutdown and start"), qPrintable(configured.last()[2].toString()));
        QVERIFY(details(uuid)["interfaces"].toList().isEmpty());
        QVERIFY(!PendingChanges(uuid).items(xml(uuid)).isEmpty());
    }
    void hostNetworkManager() {
        if (qEnvironmentVariable("OMAWARE_NETWORK_ADMIN_TEST") != "1") QSKIP("Host-network tests are opt-in: set OMAWARE_NETWORK_ADMIN_TEST=1 on a disposable test machine.");
        auto name = "nettest-" + QUuid::createUuid().toString(QUuid::Id128).left(8);
        auto result = command("networks.save", {{"name", name}, {"mode", "isolated"}, {"autostart", false}, {"start", true}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString())); networkUuid = result["uuid"].toString(); QVERIFY(!networkUuid.isEmpty());
        QFile journal(qEnvironmentVariable("OMAWARE_NETWORK_JOURNAL")); QVERIFY(journal.open(QIODevice::WriteOnly)); journal.write(networkUuid.toUtf8()); journal.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(qEnvironmentVariable("OMAWARE_NETWORK_READY")), 15000);
        result = command("networks.list"); QVERIFY(resultOk); QVariantMap network;
        for (auto v : result["items"].toList()) if (v.toMap()["uuid"] == networkUuid) network = v.toMap();
        QVERIFY(!network.isEmpty()); QVERIFY(network["managed"].toBool()); QVERIFY(network["available"].toBool());
        // The real libvirt switch passes live isolation checks: no host address, IPv6 off, no physical ports.
        QVERIFY2(network["isolation"].toMap()["isolated"].toBool(), qPrintable(QJsonDocument::fromVariant(network["isolation"]).toJson()));
        QVERIFY(QNetworkInterface::interfaceFromName(network["bridge"].toString()).addressEntries().isEmpty());
        backend->createTest(); QTRY_VERIFY(!backend->busy()); QVERIFY(!created.isEmpty()); auto uuid = created.last();
        backend->inspect(uuid); QTRY_VERIFY(!backend->detailsBusy());
        QSignalSpy configured(backend.get(), &Backend::networkConfigured);
        QVERIFY(backend->configureNetwork(uuid, "", "bridge:" + network["bridge"].toString(), "virtio", true, false, backend->details()["revision"].toString()));
        QTRY_VERIFY(!configured.isEmpty()); QVERIFY(configured.last()[1].toBool());
        // A contained VM may use the verified switch.
        result = command("containment.set", {{"uuid", uuid}, {"enabled", true}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QTRY_VERIFY(!backend->busy());
        backend->action(uuid, "start"); QTRY_VERIFY_WITH_TIMEOUT(active(uuid), 15000); QTRY_VERIFY(!backend->busy());
        command("networks.stop", {{"uuid", networkUuid}, {"revision", network["revision"]}}); QVERIFY(!resultOk);
        backend->action(uuid, "force-off"); QTRY_VERIFY(!active(uuid)); QTRY_VERIFY(!backend->busy());
        auto mac = details(uuid)["interfaces"].toList().first().toMap()["mac"].toString();
        configured.clear(); QVERIFY(backend->configureNetwork(uuid, mac, "user", "virtio", true, false, details(uuid)["revision"].toString())); QTRY_VERIFY(!configured.isEmpty()); QVERIFY(!configured.last()[1].toBool());
        result = command("containment.set", {{"uuid", uuid}, {"enabled", false}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); QTRY_VERIFY(!backend->busy());
        configured.clear(); QVERIFY(backend->configureNetwork(uuid, mac, "user", "virtio", true, false, details(uuid)["revision"].toString())); QTRY_VERIFY(!configured.isEmpty()); QVERIFY(configured.last()[1].toBool());
        result = command("networks.stop", {{"uuid", networkUuid}, {"revision", network["revision"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("networks.list"); for (auto v : result["items"].toList()) if (v.toMap()["uuid"] == networkUuid) network = v.toMap();
        result = command("networks.save", {{"uuid", networkUuid}, {"revision", network["revision"]}, {"name", name}, {"mode", "hostonly"}, {"subnet", "192.168.243.0/24"}, {"dhcp", true}, {"dhcpStart", "192.168.243.20"}, {"dhcpEnd", "192.168.243.100"}, {"autostart", false}});
        QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("networks.list"); for (auto v : result["items"].toList()) if (v.toMap()["uuid"] == networkUuid) network = v.toMap();
        QCOMPARE(network["category"].toString(), "Host-only"); QVERIFY(network["dhcp"].toBool());
        result = command("networks.start", {{"uuid", networkUuid}, {"revision", network["revision"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("networks.stop", {{"uuid", networkUuid}, {"revision", network["revision"]}}); QVERIFY(resultOk);
        result = command("networks.save", {{"uuid", networkUuid}, {"revision", network["revision"]}, {"name", name}, {"mode", "nat"}, {"subnet", "192.168.122.0/24"}, {"dhcp", false}}); QVERIFY(!resultOk); QVERIFY(result["message"].toString().contains("overlaps"));
        result = command("networks.save", {{"uuid", networkUuid}, {"revision", network["revision"]}, {"name", name}, {"mode", "nat"}, {"subnet", "192.168.243.0/24"}, {"dhcp", true}, {"dhcpStart", "192.168.243.20"}, {"dhcpEnd", "192.168.243.100"}, {"autostart", false}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("networks.list"); for (auto v : result["items"].toList()) if (v.toMap()["uuid"] == networkUuid) network = v.toMap();
        QCOMPARE(network["category"].toString(), "Shared NAT");
        result = command("networks.start", {{"uuid", networkUuid}, {"revision", network["revision"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString()));
        result = command("networks.stop", {{"uuid", networkUuid}, {"revision", network["revision"]}}); QVERIFY(resultOk);
        result = command("networks.remove", {{"uuid", networkUuid}, {"revision", network["revision"]}}); QVERIFY2(resultOk, qPrintable(result["message"].toString())); networkUuid.clear();
    }
};
int main(int argc, char **argv) {
    qputenv("QT_NO_GLIB", "1"); QGuiApplication app(argc, argv); QStandardPaths::setTestModeEnabled(true);
    QQuickStyle::setStyle("Basic"); qmlRegisterType<Console>("Omaware", 1, 0, "VmConsole"); qmlRegisterType<Workspace>("Omaware", 1, 0, "Workspace"); qmlRegisterType<IsoLibrary>("Omaware", 1, 0, "IsoLibrary"); qmlRegisterType<Updater>("Omaware", 1, 0, "Updater");
    ManagementTest test; return QTest::qExec(&test, argc, argv);
}
#include "test_management.moc"
