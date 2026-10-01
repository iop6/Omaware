// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "console.h"
#include "instance.h"
#include "isolibrary.h"
#include "updater.h"
#include <QThread>
#include "theme.h"
#include "workspace.h"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QDir>

int main(int argc, char **argv) {
    // libvirt's default GLib context belongs exclusively to its event thread.
    // Qt must not acquire that same context while waiting for worker shutdown.
    qputenv("QT_NO_GLIB", "1");
    QGuiApplication app(argc, argv);
    // Keep the storage identity stable so existing preferences/checkpoints reopen.
    app.setOrganizationName("Omaware");
    app.setApplicationName("Omaware");
    app.setApplicationDisplayName("OmaWare");
    app.setApplicationVersion(OMAWARE_VERSION);
    QCommandLineParser args;
    args.setApplicationDescription("OmaWare — a virtual machine manager for QEMU/KVM and libvirt");
    args.addHelpOption();
    args.addVersionOption();
    args.addOption({"open-vm", "Select a VM by UUID and open its console if already running", "uuid"});
    args.addOption({"restarted", "Started by OmaWare itself after an update; wait for the previous copy to close"});
    args.addOption({"theme-file", "Read an alternate Omarchy colors.toml (data only)", "path",
        QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml"});
    args.process(app);
    QQuickStyle::setStyle("Basic");
    InstanceGuard instance(InstanceGuard::defaultPath());
    bool acquired = instance.acquire();
    // After an update the previous copy is still closing; give it a moment instead of refusing to start.
    for (int i = 0; !acquired && args.isSet("restarted") && i < 75; ++i) { QThread::msleep(200); acquired = instance.acquire(); }
    if (!acquired) {
        // Explain in a small window instead of silently exiting: OmaWare is usually started from a launcher.
        Theme theme(args.value("theme-file"));
        QQmlApplicationEngine notice;
        notice.rootContext()->setContextProperty("theme", &theme);
        notice.rootContext()->setContextProperty("blocker", instance.blocker());
        notice.load(QUrl("qrc:/qml/AlreadyRunning.qml"));
        if (notice.rootObjects().isEmpty()) { qWarning("%s", qPrintable(instance.blocker())); return 1; }
        app.exec();
        return 1;
    }
    qmlRegisterType<Console>("Omaware", 1, 0, "VmConsole");
    qmlRegisterType<Workspace>("Omaware", 1, 0, "Workspace");
    qmlRegisterType<IsoLibrary>("Omaware", 1, 0, "IsoLibrary");
    qmlRegisterType<Updater>("Omaware", 1, 0, "Updater");
    Backend backend("qemu:///session");
    Theme theme(args.value("theme-file"));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    auto console = engine.rootObjects().first()->findChild<Console *>("console");
    if (!console) return 1;
    if (args.isSet("restarted")) engine.rootObjects().first()->setProperty("restarted", true);
    if (args.isSet("open-vm"))
        engine.rootObjects().first()->setProperty("selectedUuid", args.value("open-vm"));
    return app.exec();
}
