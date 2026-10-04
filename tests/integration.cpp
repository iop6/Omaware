// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "isolibrary.h"
#include "updater.h"
#include "console.h"
#include "theme.h"
#include "domainconfig.h"
#include "workspace.h"
#include <QStandardPaths>
#include <QTimer>
#include <QtTest>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSysInfo>
#include <memory>
#include <functional>

// Presentation-only inventory for empty/disconnected/read-only layout coverage.
// Real power, console and input paths below still use newly created QEMU VMs.
// Stands in for OmaWare's agent bridge: agent access off, no labs.
class UiAgent : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled MEMBER enabled NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
    Q_PROPERTY(QString command MEMBER command CONSTANT)
    Q_PROPERTY(QVariantMap screen MEMBER screen NOTIFY changed)
    Q_PROPERTY(QVariantMap proposal MEMBER proposal NOTIFY changed)
    Q_PROPERTY(QVariantMap confirmation MEMBER confirmation NOTIFY changed)
    Q_PROPERTY(QVariantMap build MEMBER build NOTIFY changed)
    Q_PROPERTY(QVariantList labs MEMBER labs NOTIFY changed)
    Q_PROPERTY(QVariantList logins MEMBER logins NOTIFY changed)
    Q_PROPERTY(QVariantList grants MEMBER grants NOTIFY changed)
public:
    bool enabled = false;
    QString error, command = "claude mcp add omaware -- omaware mcp";
    QVariantMap screen, proposal, confirmation, build;
    QVariantList labs, logins, grants;
    Q_INVOKABLE QVariantMap vmLab(const QString &) const { return {}; }
    Q_INVOKABLE QString revealPassword(const QString &) const { return {}; }
    Q_INVOKABLE QString generatePassword() const { return "abcd-efgh-jkmn-pqrs"; }
    Q_INVOKABLE void approve(const QString &, const QString &, const QString &, const QString &, bool) {}
    Q_INVOKABLE void decline(const QString &) {}
    Q_INVOKABLE void answer(const QString &, bool, bool = false) {}
    Q_INVOKABLE void revokeGrants() { grants.clear(); emit changed(); }
    Q_INVOKABLE void stop() { enabled = false; emit changed(); }
signals:
    void changed();
};
class UiInventory : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList domains MEMBER domains NOTIFY changed)
    Q_PROPERTY(bool connected MEMBER connected NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(QString message MEMBER message NOTIFY changed)
    Q_PROPERTY(QString uri MEMBER uri CONSTANT)
    Q_PROPERTY(QVariantMap details MEMBER details NOTIFY detailsChanged)
    Q_PROPERTY(bool detailsBusy MEMBER detailsBusy NOTIFY detailsChanged)
    Q_PROPERTY(QVariantMap management MEMBER management NOTIFY managementChanged)
    Q_PROPERTY(QVariantList activity MEMBER activity NOTIFY activityChanged)
    Q_PROPERTY(QString activityWarning MEMBER activityWarning NOTIFY activityChanged)
    Q_PROPERTY(QVariantMap checkpointJob MEMBER checkpointJob NOTIFY checkpointJobChanged)
public:
    QVariantList domains;
    bool connected = true, busy = false;
    QString message = "Connected to local session", uri = "qemu:///session";
    QVariantMap details;
    bool detailsBusy = false;
    QVariantMap management;
    QVariantMap checkpointJob;
    QVariantList activity;
    QString activityWarning;
    std::function<bool(QString, QVariantMap)> query;
    Q_INVOKABLE QVariantMap errorAdvice(QString message) const { return Diagnostics::advice(message); }
    Q_INVOKABLE void clearActivity() { activity.clear(); emit activityChanged(); }
    Q_INVOKABLE void showRecovery(QString action, QString uuid = {}) { emit recoveryRequested(action, uuid); }
    Q_INVOKABLE bool request(QString operation, QVariantMap input = {}) { return query ? query(operation, input) : true; }
    Q_INVOKABLE void cancelRestart() {}
    Q_INVOKABLE void cancelCheckpoint() {}
    Q_INVOKABLE void setLinks(QVariantList, bool) {}
    Q_INVOKABLE void bulkAction(QVariantList, QString) {}
    Q_INVOKABLE bool connectVms(QVariantList, QString) { return true; }
    Q_INVOKABLE void pauseForExit() {}
    Q_INVOKABLE void inspect(QString uuid, bool = false) {
        details = {};
        for (auto row : domains) if (row.toMap()["uuid"].toString() == uuid) details = row.toMap();
        emit detailsChanged();
    }
signals:
    void changed();
    void created(QString uuid);
    void graphics(GraphicsHandle socket);
    void operationFinished(QString message, bool ok);
    void detailsChanged();
    void networkConfigured(QString uuid, bool ok, QString message);
    void linksSet(bool ok, QString message);
    void pausedForExit(QStringList uuids, QStringList failures);
    void lifecycle();
    void commandFinished(QString operation, bool ok, QVariantMap result);
    void managementChanged();
    void activityChanged();
    void recoveryRequested(QString action, QString uuid);
    void checkpointJobChanged();
};

class Integration : public QObject {
    Q_OBJECT
    UiAgent uiAgent;
    QString uuid_;
    QString extraUuid_;
    virConnectPtr external_ = nullptr;
    std::unique_ptr<Backend> backend_;
    int state() {
        for (auto row : backend_->domains()) if (row.toMap()["uuid"] == uuid_) return row.toMap()["stateCode"].toInt();
        return -1;
    }
private slots:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariable("OMAWARE_VM_TEST") == "1" && qEnvironmentVariable("OMAWARE_VM_TEST_HOST") == QSysInfo::machineHostName(),
            "VM tests create, pause and power off OmaWare VMs in your libvirt session. Run them only on a disposable machine, "
            "with OMAWARE_VM_TEST=1 and OMAWARE_VM_TEST_HOST set to that machine's hostname.");
        external_ = virConnectOpen("qemu:///session");
        QVERIFY(external_);
    }
    void lifecycleConsoleAndReconnect() {
        backend_ = std::make_unique<Backend>("qemu:///session");
        QTRY_VERIFY_WITH_TIMEOUT(backend_->connected(), 15000);
        connect(backend_.get(), &Backend::created, this, [this](QString uuid) { uuid_ = uuid; });
        backend_->createTest();
        QTRY_VERIFY(!backend_->busy());
        QVERIFY(!uuid_.isEmpty());
        QSignalSpy lifecycle(backend_.get(), &Backend::lifecycle);
        backend_->action(uuid_, "start");
        QTRY_COMPARE_WITH_TIMEOUT(state(), VIR_DOMAIN_RUNNING, 15000);
        QTRY_VERIFY(lifecycle.count() > 0);
        QQuickWindow window;
        window.resize(800, 600);
        Console console(window.contentItem());
        console.setSize({800, 600});
        window.show();
        QSignalSpy frames(&console, &Console::frameReceived);
        connect(backend_.get(), &Backend::graphics, &console, &Console::attach);
        backend_->openConsole(uuid_);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0, 15000);
        QVERIFY(!console.frame().isNull());
        QCOMPARE(qAlpha(console.frame().pixel(0, 0)), 255);
        // Wait for BIOS text rather than accepting the initial black framebuffer.
        auto nonBlack = [&] {
            auto image = console.frame();
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
                if ((image.pixel(x, y) & 0xffffff) != 0) return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(nonBlack(), 15000);
        if (!qEnvironmentVariable("OMAWARE_FRAME_PATH").isEmpty()) QVERIFY(console.frame().save(qEnvironmentVariable("OMAWARE_FRAME_PATH")));
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
        QTRY_VERIFY(console.captured());
        QTest::keyClick(&window, Qt::Key_Alt, Qt::ControlModifier);
        QTRY_VERIFY(!console.captured());
        auto domain = virDomainLookupByUUIDString(external_, uuid_.toUtf8().constData());
        QVERIFY(domain);
        // External changes must be reflected without asking the backend to refresh.
        QVERIFY(virDomainSuspend(domain) == 0);
        QTRY_COMPARE(state(), VIR_DOMAIN_PAUSED);
        backend_->action(uuid_, "resume");
        QTRY_COMPARE(state(), VIR_DOMAIN_RUNNING);
        console.disconnectConsole();
        int previous = frames.count();
        backend_->openConsole(uuid_);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > previous, 10000);
        console.disconnectConsole();
        backend_.reset(); // closing OmaWare must not stop its QEMU guest
        QCOMPARE(virDomainIsActive(domain), 1);
        virDomainFree(domain);
        backend_ = std::make_unique<Backend>("qemu:///session");
        QTRY_VERIFY(backend_->connected());
        QTRY_COMPARE(state(), VIR_DOMAIN_RUNNING);
        backend_->action(uuid_, "force-off");
        QTRY_COMPARE(state(), VIR_DOMAIN_SHUTOFF);
        backend_->action(uuid_, "remove");
        QTRY_COMPARE(state(), -1);
        uuid_.clear();
    }
    void consoleSwitchesBetweenVms() {
        backend_ = std::make_unique<Backend>("qemu:///session");
        QTRY_VERIFY_WITH_TIMEOUT(backend_->connected(), 15000);
        connect(backend_.get(), &Backend::created, this, [this](QString uuid) {
            if (uuid_.isEmpty()) uuid_ = uuid;
            else extraUuid_ = uuid;
        });
        backend_->createTest();
        QTRY_VERIFY(!uuid_.isEmpty());
        QTRY_VERIFY(!backend_->busy());
        backend_->createTest();
        QTRY_VERIFY(!extraUuid_.isEmpty());
        QTRY_VERIFY(!backend_->busy());
        for (const auto &uuid : {uuid_, extraUuid_}) {
            auto domain = virDomainLookupByUUIDString(external_, uuid.toUtf8().constData());
            QVERIFY(domain);
            const int result = virDomainCreate(domain);
            virDomainFree(domain);
            QCOMPARE(result, 0);
        }

        QQuickWindow window;
        window.resize(800, 600);
        Console console(window.contentItem());
        console.setSize({800, 600});
        window.show();
        QSignalSpy frames(&console, &Console::frameReceived);
        QString attached;
        connect(backend_.get(), &Backend::graphics, &console, [&](GraphicsHandle socket) {
            attached = socket->uuid;
            frames.clear();
            console.attach(socket);
        });
        // The host crash surfaced when reusing the VNC worker for alternating
        // VMs. Exercise repeated client allocation/cleanup and frame delivery.
        for (int i = 0; i < 32; ++i) {
            const auto uuid = i % 2 ? extraUuid_ : uuid_;
            attached.clear();
            if (i % 4 == 0) console.disconnectConsole();
            backend_->openConsole(uuid);
            QTRY_COMPARE_WITH_TIMEOUT(attached, uuid, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(console.connected() && !frames.isEmpty(), 10000);
            QVERIFY(!console.frame().isNull());
            QCOMPARE(qAlpha(console.frame().pixel(0, 0)), 255);
        }
        console.disconnectConsole();
        for (const auto &uuid : {uuid_, extraUuid_}) {
            auto domain = virDomainLookupByUUIDString(external_, uuid.toUtf8().constData());
            QVERIFY(domain);
            const int active = virDomainIsActive(domain);
            virDomainFree(domain);
            QCOMPARE(active, 1);
        }
    }
    void cleanup() {
        backend_.reset();
        // Cleanup is restricted to the precise UUID created by this test.
        for (const auto &uuid : {uuid_, extraUuid_}) {
            if (external_ && !uuid.isEmpty()) {
                auto d = virDomainLookupByUUIDString(external_, uuid.toUtf8().constData());
                if (d) { if (virDomainIsActive(d) == 1) virDomainDestroy(d); virDomainUndefine(d); virDomainFree(d); }
            }
        }
        uuid_.clear();
        extraUuid_.clear();
    }
    void cleanupTestCase() {
        if (external_) virConnectClose(external_);
    }
    void uiInventoryStates() {
        UiInventory inventory;
        Theme theme("/nonexistent/omaware-test-palette.toml");
        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &items) { warnings += items; });
        engine.rootContext()->setContextProperty("backend", &inventory);
        engine.rootContext()->setContextProperty("theme", &theme); engine.rootContext()->setContextProperty("agent", &uiAgent);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto capture = [&](QString name) {
            auto directory = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR");
            if (directory.isEmpty()) return true;
            QTest::qWait(200);
            return window->grabWindow().save(directory + "/" + name + ".png");
        };
        QVERIFY(capture("omaware-empty"));
        window->resize(940, 660);
        inventory.connected = false;
        inventory.message = "The connection to the local session was lost.";
        emit inventory.changed();
        QVERIFY(capture("omaware-disconnected"));
        inventory.connected = true;
        // Many rows, long names, an external VM and a nonstandard state must fit.
        for (int i = 0; i < 30; ++i) {
            inventory.domains.append(QVariantMap{{"uuid", QString::number(i)},
                {"name", QString("external-development-workstation-with-a-long-name-%1").arg(i, 2, 10, QChar('0'))},
                {"state", "Suspended"}, {"stateCode", 7}, {"cpus", 16},
                {"memoryMiB", 65536}, {"owned", false}, {"diskless", false}});
        }
        emit inventory.changed();
        QTRY_COMPARE(window->property("selectedUuid").toString(), "0");
        auto primary = window->findChild<QQuickItem *>("primaryAction");
        QVERIFY(primary);
        QVERIFY(!primary->isVisible());
        QVERIFY(capture("omaware-read-only"));
        // Presentation fixture for disk paths/capacities at the minimum size.
        auto richVm = inventory.domains.first().toMap();
        richVm["disks"] = QVariantList{QVariantMap{{"target", "vda"}, {"device", "disk"},
            {"source", "/home/example/Virtual Machines/Ubuntu Development Workstation/disks/system-volume.qcow2"},
            {"capacityBytes", qulonglong(64) * 1024 * 1024 * 1024}, {"format", "qcow2"}, {"bus", "virtio"}}};
        richVm["videoModel"] = "virtio";
        richVm["videoHeads"] = 1;
        inventory.domains[0] = richVm;
        emit inventory.changed();
        window->setProperty("detailsOpen", true);
        auto detailsPanel = window->findChild<QObject *>("vmDetails");
        QVERIFY(detailsPanel);
        detailsPanel->setProperty("page", 1);
        QVERIFY(capture("details-disk-small"));
        window->setProperty("detailsOpen", false);
        window->setProperty("operationError", "The local session could not complete this operation. Check that the virtual machine is available, then reconnect and try again.");
        theme.setMode("light");
        QVERIFY(capture("omaware-error"));
        window->setProperty("detailsOpen", true);
        inventory.connected = false;
        inventory.domains.clear();
        emit inventory.changed();
        QTRY_VERIFY(!window->property("detailsOpen").toBool());
        // A lab an agent proposed: the plan, then the login; Build waits for a valid user and password.
        theme.setMode("dark");
        const QVariantMap vm{{"name", "web1"}, {"os", "ubuntu"}, {"cpus", 2}, {"memoryMiB", 2048}, {"diskGiB", 16},
            {"nics", QVariantList{QVariantMap{{"network", "dmz"}, {"ip", ""}}}}, {"packages", QVariantList{"nginx"}}, {"setup", QVariantList{"systemctl enable --now nginx"}}};
        const QVariantMap fw{{"name", "fw"}, {"os", "debian"}, {"cpus", 1}, {"memoryMiB", 1024}, {"diskGiB", 8},
            {"nics", QVariantList{QVariantMap{{"network", "dmz"}, {"ip", ""}}, QVariantMap{{"network", "lan"}, {"ip", "172.30.1.10/24"}}}}};
        uiAgent.proposal = {{"id", "p1"}, {"login", "Web lab"}, {"user", "alex"},
            {"plan", QVariantMap{{"name", "Web lab"}, {"networks", QVariantList{QVariantMap{{"name", "dmz"}, {"type", "internet"}, {"subnet", ""}}, QVariantMap{{"name", "lan"}, {"type", "isolated"}, {"subnet", "172.30.1.0/24"}}}}, {"vms", QVariantList{fw, vm}}}},
            {"warnings", QVariantList{"Together the VMs use most of this computer's memory."}}, {"images", QVariantList{QVariantMap{{"os", "debian"}, {"ready", false}}}}};
        emit uiAgent.changed();
        auto labDialog = window->findChild<QObject *>("labDialog"); QVERIFY(labDialog);
        QTRY_VERIFY(labDialog->property("visible").toBool());
        auto buildButton = window->findChild<QObject *>("buildLab"); QVERIFY(buildButton);
        QVERIFY(!buildButton->property("enabled").toBool());
        window->findChild<QObject *>("labPassword")->setProperty("text", "long-enough-1");
        QTRY_VERIFY(buildButton->property("enabled").toBool());
        QVERIFY(capture("lab-proposal"));
        uiAgent.proposal.clear();
        uiAgent.build = {{"id", "p1"}, {"name", "Web lab"}, {"state", "building"}, {"step", 2}, {"steps", QVariantList{"Getting the debian image", "Creating the network dmz", "Creating fw", "Starting fw"}}, {"message", "Creating fw"}};
        uiAgent.screen = {{"uuid", "x"}, {"name", "fw"}, {"at", double(QDateTime::currentMSecsSinceEpoch())}};
        uiAgent.enabled = true;
        emit uiAgent.changed();
        auto banner = window->findChild<QObject *>("agentBanner"); QVERIFY(banner);
        QTRY_VERIFY(banner->property("visible").toBool());
        QVERIFY(capture("lab-building"));
        QMetaObject::invokeMethod(labDialog, "close");
        uiAgent.build.clear(); uiAgent.screen.clear(); uiAgent.enabled = false; emit uiAgent.changed();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString{} : warnings.first().toString()));
    }
    void snapshotTabDeferredLoad() {
        UiInventory inventory;
        inventory.busy = true;
        inventory.domains = {QVariantMap{{"uuid", "snapshot-ui"}, {"name", "Snapshot tab fixture"}, {"owned", false}, {"diskless", false}, {"state", "Stopped"}, {"stateCode", 5}}};
        int loads = 0;
        inventory.query = [&](QString operation, QVariantMap input) {
            if (inventory.busy) return false;
            if (operation != "snapshots.list") return true;
            ++loads;
            QTimer::singleShot(0, &inventory, [&, input] {
                const QVariantMap result{{"uuid", input["uuid"]}, {"items", QVariantList{}}, {"currentId", ""}};
                inventory.management["snapshots.list"] = result;
                emit inventory.managementChanged();
                emit inventory.commandFinished("snapshots.list", true, result);
            });
            return true;
        };
        Theme theme("/nonexistent/palette"); QQmlApplicationEngine engine; QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](QList<QQmlError> items) { warnings += items; });
        engine.rootContext()->setContextProperty("backend", &inventory); engine.rootContext()->setContextProperty("theme", &theme); engine.rootContext()->setContextProperty("agent", &uiAgent);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        window->setProperty("selectedUuid", "snapshot-ui");
        auto details = window->findChild<QObject *>("vmDetails"); QVERIFY(details);
        auto snapshots = window->findChild<QQuickItem *>("snapshotsPage"); QVERIFY(snapshots);
        auto graph = window->findChild<QQuickItem *>("snapshotBranchMap"); QVERIFY(graph);
        details->setProperty("page", 3); window->setProperty("detailsOpen", true);
        QTest::qWait(100); QVERIFY(!snapshots->property("ready").toBool()); QCOMPARE(loads, 0);
        // No extra click or VM change: a request skipped while busy must retry.
        inventory.busy = false; emit inventory.changed();
        QTRY_VERIFY(snapshots->property("ready").toBool()); QTRY_VERIFY(graph->isVisible() && graph->height() > 100);
        QCOMPARE(loads, 1);
        // A failed query must not spin indefinitely on backend changed signals.
        inventory.query = [&](QString operation, QVariantMap input) {
            if (operation == "snapshots.list") {
                ++loads;
                QTimer::singleShot(0, &inventory, [&, input] {
                    emit inventory.changed();
                    emit inventory.commandFinished("snapshots.list", false, {{"uuid", input["uuid"]}, {"message", "Snapshot storage unavailable"}});
                });
            }
            return true;
        };
        inventory.management.clear(); emit inventory.managementChanged(); emit inventory.changed();
        QTRY_COMPARE(snapshots->property("failure").toString(), "Snapshot storage unavailable");
        const auto attempts = loads;
        for (int i = 0; i < 5; ++i) { emit inventory.changed(); QTest::qWait(20); }
        QCOMPARE(loads, attempts);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString{} : warnings.first().toString()));
    }
    void snapshotContextPermissions() {
        UiInventory inventory;
        QVariantMap vm{{"uuid", "context-ui"}, {"name", "Context fixture"}, {"owned", true}, {"diskless", false}, {"state", "Stopped"}, {"stateCode", 5}};
        inventory.domains = {vm}; inventory.details = vm;
        inventory.management["snapshots.list"] = QVariantMap{{"uuid", "context-ui"}, {"items", QVariantList{QVariantMap{{"id", "saved"}, {"name", "Saved point"}, {"kind", "copy"}, {"time", 1}, {"current", true}}}}, {"currentId", "saved"}};
        Theme theme("/nonexistent/palette"); QQmlApplicationEngine engine; QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](QList<QQmlError> items) { warnings += items; });
        engine.rootContext()->setContextProperty("backend", &inventory); engine.rootContext()->setContextProperty("theme", &theme); engine.rootContext()->setContextProperty("agent", &uiAgent);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        window->setProperty("selectedUuid", "context-ui"); window->setProperty("detailsOpen", true);
        window->findChild<QObject *>("vmDetails")->setProperty("page", 3);
        auto snapshots = window->findChild<QObject *>("snapshotsPage"); QVERIFY(snapshots);
        QTRY_VERIFY(snapshots->property("ready").toBool());
        QVERIFY(QMetaObject::invokeMethod(snapshots, "selectSnapshot", Q_ARG(QVariant, 0)));
        auto more = window->findChild<QQuickItem *>("snapshotMore"); QVERIFY(more); QTest::qWait(100);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, more->mapToScene({more->width()/2, more->height()/2}).toPoint());
        auto menu = window->findChild<QObject *>("snapshotContextMenu"); QVERIFY(menu); QTRY_VERIFY(menu->property("opened").toBool());
        auto remove = menu->findChild<QQuickItem *>("deleteSnapshot"); QVERIFY(remove); QVERIFY(remove->isEnabled());
        auto restore = menu->findChild<QQuickItem *>("contextRestore"); QVERIFY(restore); QVERIFY(restore->isEnabled());
        inventory.busy = true; emit inventory.changed(); QTRY_VERIFY(!remove->isEnabled() && !restore->isEnabled());
        inventory.details["owned"] = false; emit inventory.detailsChanged();
        inventory.busy = false; emit inventory.changed(); QTRY_VERIFY(!remove->isEnabled() && !restore->isEnabled());
        // Switching VMs dismisses the old target's menu, rather than carrying
        // an enabled destructive action into the next VM's workspace.
        vm["uuid"] = "another-context-ui"; inventory.domains.append(vm); emit inventory.changed();
        window->setProperty("selectedUuid", "another-context-ui");
        QTRY_VERIFY(!menu->property("visible").toBool());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString{} : warnings.first().toString()));
    }
    void uiThemeAndConsole() {
        backend_ = std::make_unique<Backend>("qemu:///session");
        QTRY_VERIFY(backend_->connected());
        connect(backend_.get(), &Backend::created, this, [this](QString uuid) { uuid_ = uuid; });
        backend_->createTest();
        QTRY_VERIFY(!backend_->busy());
        QVERIFY(!uuid_.isEmpty());
        backend_->action(uuid_, "start");
        QTRY_COMPARE(state(), VIR_DOMAIN_RUNNING);
        Theme theme("/nonexistent/omaware-test-palette.toml");
        QQmlApplicationEngine engine;
        QList<QQmlError> qmlWarnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &warnings) { qmlWarnings += warnings; });
        engine.rootContext()->setContextProperty("backend", backend_.get());
        engine.rootContext()->setContextProperty("theme", &theme); engine.rootContext()->setContextProperty("agent", &uiAgent);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        window->setProperty("selectedUuid", uuid_);
        auto console = window->findChild<Console *>("console");
        QVERIFY(console);
        // Selecting a VM opens its console without a separate button press.
        QTRY_VERIFY_WITH_TIMEOUT(!console->frame().isNull(), 10000);
        extraUuid_ = uuid_;
        QTRY_VERIFY(!backend_->busy());
        backend_->createTest();
        QTRY_VERIFY(!backend_->busy());
        QVERIFY(uuid_ != extraUuid_);
        // A newly created VM is selected, even with an existing library selection.
        QCOMPARE(window->property("selectedUuid").toString(), uuid_);
        window->setProperty("selectedUuid", extraUuid_);
        QTRY_VERIFY(!backend_->busy());
        backend_->action(uuid_, "start");
        QTRY_COMPARE(state(), VIR_DOMAIN_RUNNING);
        QTRY_VERIFY(!backend_->busy());
        QSignalSpy graphics(backend_.get(), &Backend::graphics);
        QSignalSpy frames(console, &Console::frameReceived);
        window->setProperty("selectedUuid", uuid_);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0, 10000);
        QVERIFY(!graphics.isEmpty());
        QCOMPARE(qvariant_cast<GraphicsHandle>(graphics.last()[0])->uuid, uuid_);
        // Close remains closed across unrelated inventory updates.
        QVERIFY(QMetaObject::invokeMethod(window, "closeConsole"));
        backend_->refresh();
        QTest::qWait(200);
        QVERIFY(console->frame().isNull());
        // Switch during an outstanding console request; only the final tab attaches.
        QVERIFY(QMetaObject::invokeMethod(window, "reopenConsole"));
        QVERIFY(QMetaObject::invokeMethod(window, "syncConsole"));
        QVERIFY(backend_->busy());
        window->setProperty("selectedUuid", extraUuid_);
        window->setProperty("selectedUuid", uuid_);
        window->setProperty("selectedUuid", extraUuid_);
        QTRY_VERIFY_WITH_TIMEOUT(!console->frame().isNull(), 10000);
        QTRY_VERIFY(!backend_->busy());
        QCOMPARE(qvariant_cast<GraphicsHandle>(graphics.last()[0])->uuid, extraUuid_);
        // A paused guest still has a display; selecting it must not resume it.
        auto paused = virDomainLookupByUUIDString(external_, extraUuid_.toUtf8().constData());
        QVERIFY(paused);
        QVERIFY(virDomainSuspend(paused) == 0);
        window->setProperty("selectedUuid", uuid_);
        QTRY_VERIFY(!backend_->busy());
        window->setProperty("selectedUuid", extraUuid_);
        QTRY_VERIFY_WITH_TIMEOUT(!console->frame().isNull(), 10000);
        virDomainInfo info{};
        QVERIFY(virDomainGetInfo(paused, &info) == 0);
        QCOMPARE(info.state, static_cast<unsigned char>(VIR_DOMAIN_PAUSED));
        virDomainFree(paused);
        auto screenshotDir = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR");
        auto capture = [&](QString name) {
            if (screenshotDir.isEmpty()) return true;
            QTest::qWait(180);
            auto image = window->grabWindow();
            return !image.isNull() && image.save(screenshotDir + "/" + name + ".png");
        };
        for (auto mode : {"dark", "light", "hacker"}) {
            theme.setMode(mode);
            QTRY_COMPARE(window->color(), QColor(theme.colors()["background"].toString()));
            if (!screenshotDir.isEmpty()) {
                QTest::qWait(1200); // Let firmware output and the changed palette settle for visual QA.
                auto image = window->grabWindow();
                QVERIFY(!image.isNull());
                QVERIFY(image.save(screenshotDir + "/omaware-" + mode + ".png"));
                QVERIFY(console->frame().save(screenshotDir + "/guest-" + mode + ".png"));
            }
        }
        // Search and state filters compose without changing the selected console.
        auto search = window->findChild<QQuickItem *>("librarySearch");
        auto library = window->findChild<QQuickItem *>("vmLibrary");
        QVERIFY(search && library);
        QString selectedName;
        for (auto row : backend_->domains())
            if (row.toMap()["uuid"].toString() == extraUuid_) selectedName = row.toMap()["name"].toString();
        QVERIFY(!selectedName.isEmpty());
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(search->hasActiveFocus());
        search->setProperty("text", selectedName.toUpper());
        QTRY_COMPARE(library->property("count").toInt(), 1);
        window->setProperty("stateFilter", 2);
        QTRY_COMPARE(library->property("count").toInt(), 0);
        QCOMPARE(window->property("selectedUuid").toString(), extraUuid_);
        QVERIFY(capture("omaware-no-results"));
        window->setProperty("stateFilter", 1);
        QTRY_COMPARE(library->property("count").toInt(), 1);
        window->setProperty("searchQuery", "");
        window->setProperty("stateFilter", 0);
        QTRY_COMPARE(search->property("text").toString(), QString{});

        // Switching away from a captured console always releases guest input.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            console->mapToScene(QPointF(console->width() / 2, console->height() / 2)).toPoint());
        QTRY_VERIFY(console->captured());
        auto details = window->findChild<QQuickItem *>("detailsTab");
        QVERIFY(details);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            details->mapToScene(QPointF(details->width() / 2, details->height() / 2)).toPoint());
        QTRY_VERIFY(window->property("detailsOpen").toBool());
        QVERIFY(!console->captured());
        QVERIFY(capture("omaware-details"));
        window->setProperty("detailsOpen", false);
        window->setProperty("consoleExpanded", true);
        QVERIFY(capture("omaware-focus"));
        auto tools = window->findChild<QQuickItem *>("consoleTools");
        auto focusTitle = window->findChild<QQuickItem *>("focusTitle");
        QVERIFY(tools && focusTitle);
        QTRY_VERIFY(tools->mapToScene(QPointF(tools->width(), 0)).x() > window->width() - 40);
        auto focusIcon = window->findChild<QQuickItem *>("focusIcon"); QVERIFY(focusIcon);
        QVERIFY(focusTitle->mapToScene(QPointF()).x() > focusIcon->mapToScene(QPointF(focusIcon->width(), 0)).x());
        QVERIFY(focusTitle->mapToScene(QPointF(focusTitle->width(), 0)).x() < tools->mapToScene(QPointF()).x());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("consoleExpanded").toBool());

        // Fleet statistics cover every running VM and the host in one query.
        QVERIFY(backend_->request("stats.all", {}));
        QTRY_VERIFY(backend_->management().contains("stats.all"));
        {
            const auto fleet = backend_->management()["stats.all"].toMap();
            QStringList sampled;
            for (const auto &entry : fleet["vms"].toList()) if (entry.toMap()["cpuTime"].toDouble() > 0) sampled << entry.toMap()["uuid"].toString();
            QVERIFY(sampled.contains(uuid_) && sampled.contains(extraUuid_));
            QVERIFY(fleet["host"].toMap()["cpus"].toInt() > 0);
            QVERIFY(fleet["host"].toMap().contains("cpu_idle"));
        }
        // Terminal workspace: monitor, keyboard navigation, log drawer, keyboard map and command prompt.
        theme.setMode("dark");
        QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
        QTRY_COMPARE(window->property("navigation").toString(), QString("monitor"));
        auto monitor = window->findChild<QQuickItem *>("monitorPage"); QVERIFY(monitor); QTRY_VERIFY(monitor->isVisible());
        auto monitorList = monitor->findChild<QQuickItem *>("monitorList"); QVERIFY(monitorList);
        QTRY_VERIFY(monitorList->property("count").toInt() >= 2);
        monitor->setProperty("sortKey", "name"); monitor->setProperty("descending", false);
        const auto rows = monitor->property("rows").toList(); QVERIFY(rows.size() >= 2);
        window->setProperty("selectedUuid", rows[0].toMap()["uuid"]);
        monitorList->forceActiveFocus(); QTRY_VERIFY(monitorList->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_J);
        QTRY_COMPARE(window->property("selectedUuid").toString(), rows[1].toMap()["uuid"].toString());
        monitor->setProperty("sortKey", "cpu"); monitor->setProperty("descending", true);
        QVERIFY(window->findChild<QObject *>("monitorHost"));
        QTest::qWait(4500); // Monitor samples every 2 s; two samples give rates and host CPU.
        QVERIFY(capture("monitor"));
        QTest::keyClick(window, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(window->property("navigation").toString(), QString("library"));
        window->setProperty("logOpen", true);
        auto drawer = window->findChild<QQuickItem *>("logDrawer"); QVERIFY(drawer); QTRY_VERIFY(drawer->isVisible());
        QVERIFY(capture("log-drawer"));
        window->setProperty("logOpen", false); QTRY_VERIFY(!drawer->isVisible());
        auto keyMap = window->findChild<QObject *>("keyMap"); QVERIFY(keyMap);
        QVERIFY(QMetaObject::invokeMethod(keyMap, "open")); QTRY_VERIFY(keyMap->property("opened").toBool());
        QVERIFY(capture("keymap"));
        QVERIFY(QMetaObject::invokeMethod(keyMap, "close")); QTRY_VERIFY(!keyMap->property("visible").toBool());
        auto commandPrompt = window->findChild<QObject *>("actionPalette"); QVERIFY(commandPrompt);
        QVERIFY(QMetaObject::invokeMethod(commandPrompt, "open")); QTRY_VERIFY(commandPrompt->property("opened").toBool());
        auto commandQuery = commandPrompt->findChild<QQuickItem *>("actionQuery"); QVERIFY(commandQuery);
        commandQuery->setProperty("text", "htop");
        QTRY_COMPARE(commandPrompt->property("matches").toList().size(), 1);
        QCOMPARE(commandPrompt->property("matches").toList().first().toMap()["key"].toString(), QString("monitor"));
        commandQuery->setProperty("text", "");
        QVERIFY(capture("command-prompt"));
        QVERIFY(QMetaObject::invokeMethod(commandPrompt, "close")); QTRY_VERIFY(!commandPrompt->property("visible").toBool());
        // Sidebar: right-click targets the clicked VM without changing the selection or its console.
        window->setProperty("navigation", "library"); window->setProperty("detailsOpen", false);
        window->setProperty("selectedUuid", uuid_); QTRY_VERIFY(!backend_->busy());
        auto stateCodeOf = [&](const QString &id) { for (const auto &row : backend_->domains()) if (row.toMap()["uuid"].toString() == id) return row.toMap()["stateCode"].toInt(); return -1; };
        auto center = [](QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); };
        // ListView delegates have no QObject parent, so reach the row through the view itself.
        auto rowFor = [&](const QString &id) -> QQuickItem * {
            const auto rows = window->property("filteredDomains").toList(); int index = -1;
            for (int i = 0; i < rows.size(); ++i) if (rows[i].toMap()["uuid"].toString() == id) index = i;
            QQuickItem *delegate = nullptr;
            if (index < 0 || !QMetaObject::invokeMethod(library, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, delegate), Q_ARG(int, index)) || !delegate) return nullptr;
            return delegate->findChild<QQuickItem *>("vmRow_" + id);
        };
        QQuickItem *otherRow = nullptr; QTRY_VERIFY((otherRow = rowFor(extraUuid_)) && otherRow->isVisible());
        auto vmMenu = window->findChild<QObject *>("vmContextMenu"); QVERIFY(vmMenu);
        QTRY_VERIFY((otherRow = rowFor(extraUuid_)) && otherRow->isVisible()); QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center(otherRow));
        QTRY_VERIFY(vmMenu->property("opened").toBool());
        QCOMPARE(vmMenu->property("vm").toMap()["uuid"].toString(), extraUuid_);
        QCOMPARE(window->property("selectedUuid").toString(), uuid_);
        QVERIFY(capture("sidebar-menu"));
        auto power = vmMenu->findChild<QQuickItem *>("vmMenuPower"); QVERIFY(power); QTRY_VERIFY(power->isEnabled());
        // The power item follows the clicked VM's own state; toggle it and back.
        const int originalState = stateCodeOf(extraUuid_); QVERIFY(originalState == 1 || originalState == 3);
        const int toggledState = originalState == 1 ? 3 : 1;
        QCOMPARE(power->property("text").toString(), QString(originalState == 1 ? "Pause" : "Resume"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(power));
        QTRY_COMPARE(stateCodeOf(extraUuid_), toggledState); QTRY_VERIFY(!backend_->busy());
        QCOMPARE(window->property("selectedUuid").toString(), uuid_); QTRY_VERIFY(console->hasFrame());   // its console was opened just above; the first picture arrives shortly
        QTRY_VERIFY((otherRow = rowFor(extraUuid_)) && otherRow->isVisible()); QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center(otherRow)); QTRY_VERIFY(vmMenu->property("opened").toBool());
        QTRY_COMPARE(power->property("text").toString(), QString(toggledState == 1 ? "Pause" : "Resume"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(power));
        QTRY_COMPARE(stateCodeOf(extraUuid_), originalState); QTRY_VERIFY(!backend_->busy());
        // Favorites form a collapsible group; the list count still counts VMs only.
        const int vmCount = library->property("count").toInt();
        QTRY_VERIFY((otherRow = rowFor(extraUuid_)) && otherRow->isVisible()); QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center(otherRow)); QTRY_VERIFY(vmMenu->property("opened").toBool());
        auto favorite = vmMenu->findChild<QQuickItem *>("vmMenuFavorite"); QVERIFY(favorite);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(favorite));
        QTRY_VERIFY(window->property("grouped").toBool()); QCOMPARE(library->property("count").toInt(), vmCount);
        QQuickItem *favorites = nullptr; QTRY_VERIFY((otherRow = rowFor(extraUuid_)) && (favorites = otherRow->parentItem()->findChild<QQuickItem *>("libraryGroup_favorites")) && favorites->isVisible());
        QVERIFY(capture("sidebar-grouped"));
        // Keyboard: the menu key opens the selected VM's menu.
        library->forceActiveFocus(); QTRY_VERIFY(library->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier); QTRY_VERIFY(vmMenu->property("opened").toBool());
        QCOMPARE(vmMenu->property("vm").toMap()["uuid"].toString(), window->property("selectedUuid").toString());
        QVERIFY(QMetaObject::invokeMethod(vmMenu, "close")); QTRY_VERIFY(!vmMenu->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window, "toggleFavorite", Q_ARG(QVariant, QVariant(QVariantMap{{"uuid", extraUuid_}}))));
        QTRY_VERIFY(!window->property("grouped").toBool());
        // Ctrl+B collapses the sidebar to an icon rail and back.
        const bool railBefore = window->property("sidebarRail").toBool(); QVERIFY(!railBefore);
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier); QTRY_VERIFY(window->property("sidebarRail").toBool());
        auto sidebar = window->findChild<QQuickItem *>("sidebar"); QVERIFY(sidebar); QTRY_VERIFY(sidebar->width() < 80);
        QVERIFY(capture("sidebar-rail"));
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier); QTRY_VERIFY(!window->property("sidebarRail").toBool());
        window->setProperty("navigation", "monitor");
        window->resize(940, 660);
        QVERIFY(capture("monitor-compact"));
        window->setProperty("navigation", "library");
        window->resize(940, 660);
        for (auto mode : {"dark", "light"}) {
            theme.setMode(mode);
            QVERIFY(capture(QString("omaware-compact-") + mode));
        }
        QVERIFY(QMetaObject::invokeMethod(window, "closeConsole"));
        QTRY_VERIFY(!console->hasFrame());
        QVERIFY(capture("omaware-closed"));
        window->resize(1280, 840);
        window->setProperty("selectedUuid", uuid_);
        QTRY_VERIFY(!backend_->busy());
        console->disconnectConsole();
        backend_->action(uuid_, "force-off");
        QTRY_COMPARE(state(), VIR_DOMAIN_SHUTOFF);
        QVERIFY(capture("omaware-stopped"));
        // Confirmations keep their original target even if the selection changes.
        auto dialog = window->findChild<QObject *>("confirmDialog");
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "confirm", Q_ARG(QVariant, true)));
        QCOMPARE(dialog->property("targetUuid").toString(), uuid_);
        window->setProperty("selectedUuid", extraUuid_);
        QCOMPARE(dialog->property("targetUuid").toString(), uuid_);
        QVERIFY(capture("omaware-confirmation"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        QTRY_VERIFY(!backend_->busy());
        QCOMPARE(state(), int(VIR_DOMAIN_SHUTOFF));
        window->setProperty("selectedUuid", uuid_);
        QVERIFY(QMetaObject::invokeMethod(dialog, "confirm", Q_ARG(QVariant, true)));
        window->setProperty("selectedUuid", extraUuid_);
        QTRY_VERIFY(!backend_->busy());
        auto confirm = window->findChild<QQuickItem *>("confirmAction");
        QVERIFY(confirm);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            confirm->mapToScene(QPointF(confirm->width() / 2, confirm->height() / 2)).toPoint());
        QTRY_COMPARE(state(), -1);
        uuid_.clear();
        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.isEmpty() ? QString{} : qmlWarnings.first().toString()));
    }
    void networkConfiguration() {
        backend_ = std::make_unique<Backend>("qemu:///session");
        QTRY_VERIFY(backend_->connected());
        connect(backend_.get(), &Backend::created, this, [this](QString uuid) { uuid_ = uuid; });
        backend_->createTest();
        QTRY_VERIFY(!backend_->busy());
        QVERIFY(!uuid_.isEmpty());
        const auto journalPath = qEnvironmentVariable("OMAWARE_TEST_UUID_FILE");
        if (!journalPath.isEmpty()) {
            QFile journal(journalPath);
            QVERIFY(journal.open(QIODevice::WriteOnly));
            QCOMPARE(journal.write(uuid_.toUtf8()), uuid_.toUtf8().size());
        }
        backend_->inspect(uuid_);
        QTRY_VERIFY(!backend_->detailsBusy());
        QCOMPARE(backend_->details()["interfaces"].toList().size(), 0);
        QCOMPARE(backend_->details()["architecture"].toString(), "x86_64");
        QCOMPARE(backend_->details()["vcpus"].toInt(), 1);

        Theme theme("/nonexistent/omaware-palette.toml");
        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &items) { warnings += items; });
        engine.rootContext()->setContextProperty("backend", backend_.get());
        engine.rootContext()->setContextProperty("theme", &theme); engine.rootContext()->setContextProperty("agent", &uiAgent);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        window->setProperty("selectedUuid", uuid_);
        window->setProperty("detailsOpen", true);
        auto panel = window->findChild<QObject *>("vmDetails");
        auto dialog = window->findChild<QObject *>("networkDialog");
        QVERIFY(panel && dialog);
        panel->setProperty("page", 2);
        QTRY_VERIFY(!backend_->detailsBusy());
        auto capture = [&](QString name) {
            const auto directory = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR");
            if (directory.isEmpty()) return true;
            QTest::qWait(200);
            return window->grabWindow().save(directory + "/" + name + ".png");
        };
        auto click = [&](const char *name) {
            auto button = window->findChild<QQuickItem *>(name);
            if (!button || !button->isEnabled()) return false;
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
            return true;
        };
        QVERIFY(click("addAdapter"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(capture("network-add-dialog"));
        QSignalSpy saved(backend_.get(), &Backend::networkConfigured);
        QVERIFY(click("saveAdapter"));
        QTRY_COMPARE(saved.count(), 1);
        QVERIFY2(saved.last()[1].toBool(), qPrintable(saved.last()[2].toString()));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(!backend_->detailsBusy());
        auto nics = backend_->details()["interfaces"].toList();
        QCOMPARE(nics.size(), 1);
        auto mac = nics.first().toMap()["mac"].toString();
        QVERIFY(!mac.isEmpty());

        // Add a second adapter, then edit the original without changing its MAC.
        QVERIFY(backend_->configureNetwork(uuid_, "", "user", "e1000", true, false, backend_->details()["revision"].toString()));
        QTRY_COMPARE(saved.count(), 2);
        QVERIFY2(saved.last()[1].toBool(), qPrintable(saved.last()[2].toString()));
        QTRY_VERIFY(!backend_->detailsBusy());
        QCOMPARE(backend_->details()["interfaces"].toList().size(), 2);
        const auto staleRevision = backend_->details()["revision"].toString();
        QVERIFY(backend_->configureNetwork(uuid_, mac, "user", "e1000e", true, false, staleRevision));
        QTRY_COMPARE(saved.count(), 3);
        QVERIFY2(saved.last()[1].toBool(), qPrintable(saved.last()[2].toString()));
        QTRY_VERIFY(!backend_->detailsBusy());
        QCOMPARE(backend_->details()["interfaces"].toList().first().toMap()["mac"].toString(), mac);
        QCOMPARE(backend_->details()["interfaces"].toList().first().toMap()["model"].toString(), "e1000e");
        QVERIFY(backend_->configureNetwork(uuid_, mac, "user", "virtio", true, false, staleRevision));
        QTRY_COMPARE(saved.count(), 4);
        QVERIFY(!saved.last()[1].toBool());
        QVERIFY(saved.last()[2].toString().contains("configuration changed"));
        QTRY_VERIFY(!backend_->detailsBusy());
        QCOMPARE(backend_->details()["interfaces"].toList().first().toMap()["model"].toString(), "e1000e");

        // An optional, uniquely created host bridge exercises real bridge attachment.
        const auto bridge = qEnvironmentVariable("OMAWARE_TEST_BRIDGE");
        if (!bridge.isEmpty()) {
            QVERIFY(backend_->configureNetwork(uuid_, mac, "bridge:" + bridge, "virtio", true, false, backend_->details()["revision"].toString()));
            QTRY_COMPARE(saved.count(), 5);
            QVERIFY2(saved.last()[1].toBool(), qPrintable(saved.last()[2].toString()));
            QTRY_VERIFY(!backend_->detailsBusy());
            QCOMPARE(backend_->details()["interfaces"].toList().first().toMap()["source"].toString(), bridge);
        }
        backend_->action(uuid_, "start");
        QTRY_COMPARE_WITH_TIMEOUT(state(), VIR_DOMAIN_RUNNING, 15000);
        QTRY_VERIFY(!backend_->busy());
        QTRY_VERIFY(!backend_->detailsBusy());
        QVERIFY(!backend_->details()["pendingNetworkChanges"].toBool());
        nics = backend_->details()["interfaces"].toList();
        const auto secondMac = nics.last().toMap()["mac"].toString();
        auto networkId = nics.first().toMap()["networkId"].toString();
        auto model = nics.first().toMap()["model"].toString();
        int count = saved.count();
        // Refreshing running-VM details can queue automatic console attachment.
        // Wait for that operation too before issuing a serialized network edit.
        QTRY_VERIFY(!backend_->busy());
        QVERIFY(backend_->configureNetwork(uuid_, mac, networkId, model, false, false, backend_->details()["revision"].toString()));
        ++count;
        QTRY_COMPARE(saved.count(), count);
        QVERIFY2(saved.last()[1].toBool(), qPrintable(saved.last()[2].toString()));
        QTRY_VERIFY(!backend_->detailsBusy());
        // A link-state change applies to the running VM straight away, so nothing is left pending.
        QVERIFY2(saved.last()[2].toString().contains("running VM"), qPrintable(saved.last()[2].toString()));
        QVERIFY(!backend_->details()["pendingNetworkChanges"].toBool());
        QVERIFY(!backend_->details()["liveInterfaces"].toList().first().toMap()["linkUp"].toBool());
        QVERIFY(!backend_->details()["interfaces"].toList().first().toMap()["linkUp"].toBool());
        QVERIFY(backend_->configureNetwork(uuid_, secondMac, "", "virtio", true, true, backend_->details()["revision"].toString()));
        ++count;
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), count, 20000);
        QVERIFY2(saved.last()[1].toBool(), qPrintable(saved.last()[2].toString()));
        QTRY_VERIFY(!backend_->detailsBusy());
        // This test VM has no OS to acknowledge a PCI unplug, so the removal waits for the next start.
        QVERIFY2(saved.last()[2].toString().contains("full shutdown and start"), qPrintable(saved.last()[2].toString()));
        QCOMPARE(backend_->details()["interfaces"].toList().size(), 1);
        QCOMPARE(backend_->details()["liveInterfaces"].toList().size(), 2);
        QVERIFY(backend_->details()["pendingNetworkChanges"].toBool());
        for (auto mode : {"dark", "light"}) {
            theme.setMode(mode);
            QVERIFY(capture(QString("network-details-") + mode));
            QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, backend_->details()), Q_ARG(QVariant, backend_->details()["interfaces"].toList().first()), Q_ARG(QVariant, false)));
            QVERIFY(capture(QString("network-edit-") + mode));
            QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        }
        panel->setProperty("page", 0);
        QVERIFY(capture("expanded-details-overview"));
        panel->setProperty("page", 1);
        QVERIFY(capture("expanded-details-hardware"));
        window->resize(940, 660);
        panel->setProperty("page", 2);
        QVERIFY(capture("network-details-small"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, backend_->details()), Q_ARG(QVariant, backend_->details()["interfaces"].toList().first()), Q_ARG(QVariant, false)));
        QVERIFY(capture("network-edit-small"));
        QVERIFY(click("networkPicker"));
        QVERIFY(capture("network-choices-small"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(dialog->property("visible").toBool());
        const auto choices = dialog->property("choices").toList();
        auto picker = window->findChild<QObject *>("networkPicker");
        auto saveButton = window->findChild<QQuickItem *>("saveAdapter");
        QVERIFY(picker && saveButton);
        for (int i = 0; i < choices.size(); ++i) {
            const auto choice = choices[i].toMap();
            if (!choice["available"].toBool() && choice["id"].toString().startsWith("unavailable:")) {
                picker->setProperty("currentIndex", i);
                QTRY_VERIFY(!saveButton->isEnabled());
                QVERIFY(capture("network-unavailable-small"));
                break;
            }
        }
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        backend_->action(uuid_, "force-off");
        QTRY_COMPARE(state(), VIR_DOMAIN_SHUTOFF);
        QTRY_VERIFY(!backend_->busy());
        backend_->action(uuid_, "start");
        QTRY_COMPARE_WITH_TIMEOUT(state(), VIR_DOMAIN_RUNNING, 15000);
        QTRY_VERIFY(!backend_->busy());
        QTRY_VERIFY(!backend_->detailsBusy());
        QCOMPARE(backend_->details()["liveInterfaces"].toList().size(), 1);
        QVERIFY(!backend_->details()["liveInterfaces"].toList().first().toMap()["linkUp"].toBool());
        QVERIFY(!backend_->details()["pendingNetworkChanges"].toBool());
        // Ownership is enforced by the worker even when the UI is bypassed.
        // Waiting for refreshed details above can also start the UI's console
        // reconnect. Lifecycle commands are refused while that operation is busy.
        QTRY_VERIFY(!backend_->busy());
        backend_->action(uuid_, "force-off");
        QTRY_COMPARE(state(), VIR_DOMAIN_SHUTOFF);
        QTRY_VERIFY(!backend_->busy());
        auto domain = virDomainLookupByUUIDString(external_, uuid_.toUtf8().constData());
        QVERIFY(domain);
        QVERIFY(virDomainSetMetadata(domain, VIR_DOMAIN_METADATA_ELEMENT, nullptr, nullptr,
            "https://omaware.org/xmlns/prototype/1", VIR_DOMAIN_AFFECT_CONFIG) == 0);
        backend_->inspect(uuid_);
        QTRY_VERIFY(!backend_->detailsBusy());
        QVERIFY(!backend_->details()["owned"].toBool());
        char *before = virDomainGetXMLDesc(domain, VIR_DOMAIN_XML_INACTIVE);
        QVERIFY(before);
        const QByteArray savedXml(before); free(before);
        count = saved.count();
        QVERIFY(backend_->configureNetwork(uuid_, mac, "user", "virtio", true, false, backend_->details()["revision"].toString()));
        ++count;
        QTRY_COMPARE(saved.count(), count);
        QVERIFY(!saved.last()[1].toBool());
        QVERIFY(saved.last()[2].toString().contains("VMs OmaWare created"));
        char *after = virDomainGetXMLDesc(domain, VIR_DOMAIN_XML_INACTIVE);
        QVERIFY(after);
        QCOMPARE(QByteArray(after), savedXml);
        free(after);
        virDomainFree(domain);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString{} : warnings.first().toString()));
    }
};
int main(int argc, char **argv) {
    qputenv("QT_NO_GLIB", "1");
    QGuiApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<Console>("Omaware", 1, 0, "VmConsole");
    qmlRegisterType<Workspace>("Omaware", 1, 0, "Workspace");
    qmlRegisterType<IsoLibrary>("Omaware", 1, 0, "IsoLibrary"); qmlRegisterType<Updater>("Omaware", 1, 0, "Updater");
    Integration test;
    return QTest::qExec(&test, argc, argv);
}
#include "integration.moc"
