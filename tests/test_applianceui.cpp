// SPDX-License-Identifier: GPL-3.0-or-later
// Actual QML components with an inert backend: no Backend, libvirt or application socket.
#include <QtTest>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSignalSpy>
#include "isolibrary.h"
#include "theme.h"

class UiWorkspace : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    Q_INVOKABLE QVariant get(const QString &, const QVariant &fallback = {}) { return fallback; }

    Q_INVOKABLE void set(const QString &, const QVariant &) {}

    Q_INVOKABLE QString localPath(const QString &url) { return QUrl(url).toLocalFile(); }

    Q_INVOKABLE void copy(const QString &) {}
};

class UiBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap management MEMBER management CONSTANT)
    Q_PROPERTY(QVariantMap checkpointJob MEMBER checkpointJob CONSTANT)
    Q_PROPERTY(bool connected MEMBER connected CONSTANT)
    Q_PROPERTY(bool busy MEMBER busy CONSTANT)
    Q_PROPERTY(QString message MEMBER message CONSTANT)
public:
    UiBackend() {
        const QVariantList presets{QVariantMap{{"id", "generic"}, {"label", "Generic OS"}}};
        management["capabilities"] = QVariantMap{{"virtInstall", true}, {"presets", presets}};
    }

    QVariantMap management;
    QVariantMap checkpointJob, lastInput;
    QString message, lastOperation;
    bool connected = true, busy = false;

    Q_INVOKABLE bool request(const QString &op, const QVariantMap &input) {
        lastOperation = op;
        lastInput = input;
        return true;
    }

    Q_INVOKABLE QVariantMap errorAdvice(const QString &) { return {}; }

    Q_INVOKABLE void showRecovery(const QString &, const QString &) {}

    Q_INVOKABLE void cancelCheckpoint() {}

signals:
    void commandFinished(const QString &op, bool ok, const QVariantMap &result);
};

class ApplianceUiTests : public QObject {
    Q_OBJECT
private slots:

    void routesActualQml() {
        qmlRegisterType<UiWorkspace>("Omaware", 1, 0, "Workspace");
        QTemporaryDir dir;
        IsoLibrary library;
        library.setFolder(dir.filePath("isos"));
        library.setProperty("autoCheck", false);
        UiWorkspace workspace;
        UiBackend backend;
        Theme theme(dir.filePath("absent.toml"));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("workspaceStub", &workspace);
        engine.rootContext()->setContextProperty("libraryStub", &library);
        QQmlComponent component(&engine);
        const QByteArray document = "import QtQuick\nimport QtQuick.Controls\nimport \"" +
                                    QUrl::fromLocalFile(QString(OMAWARE_SOURCE_DIR) + "/qml").toString().toUtf8() +
                                    "\" as Ui\nApplicationWindow { width: 1000; height: 800; Ui.CreateVmDialog { "
                                    "workspace: workspaceStub; isoLibrary: libraryStub } Ui.IsoShopPage { library: "
                                    "libraryStub } Ui.IsoDropZone { library: libraryStub } }";
        component.setData(document, QUrl());
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root != nullptr, qPrintable(component.errorString()));
        auto dialog = root->findChild<QObject *>("createVmDialog"), shop = root->findChild<QObject *>("isoShop"),
             drop = root->findChild<QObject *>("isoDropZone");
        QVERIFY(dialog && shop && drop);
        auto mode = dialog->findChild<QObject *>("newVmSourceMode"),
             source = dialog->findChild<QObject *>("newVmSource"), name = dialog->findChild<QObject *>("newVmName");
        QVERIFY(mode && source && name);
        for (const auto &path : {"/fixture/remnux.ova", "/fixture/remnux.QCOW2", "/fixture/ubuntu.iso"}) {
            QVERIFY(QMetaObject::invokeMethod(
                    dialog, "useMedia", Q_ARG(QVariant, QString(path)), Q_ARG(QVariant, QString())));
            const bool disk = !QString(path).endsWith(".iso");
            QCOMPARE(mode->property("currentIndex").toInt(), disk ? 1 : 0);
            QCOMPARE(source->property("text").toString(), QString(path));
            name->setProperty("text", "fixture");
            dialog->setProperty("reviewing", true);
            QVERIFY(QMetaObject::invokeMethod(dialog, "submitted"));
            QCOMPARE(backend.lastOperation, QString("vm.create"));
            QCOMPARE(backend.lastInput["sourceMode"].toString(), disk ? QString("disk") : QString("iso"));
            QCOMPARE(backend.lastInput["source"].toString(), QString(path));
            dialog->setProperty("applying", false);
        }
        QSignalSpy isoSignal(shop, SIGNAL(useIso(QString))), diskSignal(shop, SIGNAL(useAppliance(QString)));
        QVERIFY(QMetaObject::invokeMethod(
                shop, "useMedia", Q_ARG(QVariant, QVariantMap({{"type", "disk"}, {"path", "/fixture/remnux.qcow2"}}))));
        QCOMPARE(diskSignal.size(), 1);
        QCOMPARE(isoSignal.size(), 0);
        QVERIFY(QMetaObject::invokeMethod(
                shop, "useMedia", Q_ARG(QVariant, QVariantMap({{"type", "iso"}, {"path", "/fixture/ubuntu.iso"}}))));
        QCOMPARE(isoSignal.size(), 1);
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(drop, "hasIso", Q_RETURN_ARG(QVariant, result),
                Q_ARG(QVariant, QVariantList{"file:///fixture/remnux.OVA"})));
        QVERIFY(result.toBool());
        QVERIFY(QMetaObject::invokeMethod(drop, "hasIso", Q_RETURN_ARG(QVariant, result),
                Q_ARG(QVariant, QVariantList{"file:///fixture/remnux.qcow2"})));
        QVERIFY(result.toBool());
    }
};
QTEST_MAIN(ApplianceUiTests)
#include "test_applianceui.moc"
