// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentbridge.h"
#include "backend.h"
#include "console.h"
#include "instance.h"
#include "isolibrary.h"
#include "mcpserver.h"
#include "theme.h"
#include "updater.h"
#include "workspace.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QThread>

namespace {
// The organization and application name stay "Omaware", so existing settings, snapshots and disk paths
// keep working; the name people see is OmaWare.
void identify(QCoreApplication &app) {
    app.setOrganizationName("Omaware");
    app.setApplicationName("Omaware");
    app.setApplicationVersion(OMAWARE_VERSION);
}
}

int main(int argc, char **argv) {
    // libvirt's default GLib context belongs exclusively to its event thread.
    // Qt must not acquire that same context while waiting for worker shutdown.
    qputenv("QT_NO_GLIB", "1");
    // `omaware mcp`: the Model Context Protocol server an AI agent starts; no window.
    if (argc == 2 && QByteArray(argv[1]) == "mcp") {
        QCoreApplication app(argc, argv);
        identify(app);
        return Mcp::run();
    }
    QGuiApplication app(argc, argv);
    identify(app);
    app.setApplicationDisplayName("OmaWare");
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
    for (int i = 0; !acquired && args.isSet("restarted") && i < 75; ++i) {
        QThread::msleep(200);
        acquired = instance.acquire();
    }
    if (!acquired) {
        // Explain in a small window instead of silently exiting: OmaWare is usually started from a launcher.
        Theme theme(args.value("theme-file"));
        QQmlApplicationEngine notice;
        notice.rootContext()->setContextProperty("theme", &theme);
        notice.rootContext()->setContextProperty("blocker", instance.blocker());
        notice.load(QUrl("qrc:/qml/AlreadyRunning.qml"));
        if (notice.rootObjects().isEmpty()) {
            qWarning("%s", qPrintable(instance.blocker()));
            return 1;
        }
        app.exec();
        return 1;
    }
    qmlRegisterType<Console>("Omaware", 1, 0, "VmConsole");
    qmlRegisterType<Workspace>("Omaware", 1, 0, "Workspace");
    qmlRegisterType<IsoLibrary>("Omaware", 1, 0, "IsoLibrary");
    qmlRegisterType<Updater>("Omaware", 1, 0, "Updater");
    Backend backend("qemu:///session");
    Theme theme(args.value("theme-file"));
    // Declared before the engine, so the window is gone before it is.
    AgentBridge agent(&backend);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("agent", &agent);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    QObject *window = engine.rootObjects().first();
    if (!window->findChild<Console *>("console")) return 1;
    // The OS Shop's library also serves agents' get_media.
    agent.setMedia(window->findChild<IsoLibrary *>("isoLibrary"));
    if (args.isSet("restarted")) window->setProperty("restarted", true);
    if (args.isSet("open-vm")) window->setProperty("selectedUuid", args.value("open-vm"));
    return app.exec();
}
