// SPDX-License-Identifier: GPL-3.0-or-later
#include "bridgehelper.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <sys/stat.h>

#ifndef OMAWARE_HELPER_PATH
#define OMAWARE_HELPER_PATH "/usr/local/libexec/omaware/authorize-bridge"
#endif

namespace {
QString quoted(const QString &path) {
    return path.contains(QRegularExpression("[^A-Za-z0-9_./+-]")) ? "'" + QString(path).replace("'", "'\\''") + "'"
                                                                  : path;
}
}

QString BridgeHelper::destination() {
    return QStringLiteral(OMAWARE_HELPER_PATH);
}

QString BridgeHelper::check(const QString &candidate) {
    const QString path = QDir::cleanPath(candidate);
    struct stat file{};
    if (!QFileInfo(path).isAbsolute() || lstat(path.toUtf8().constData(), &file) != 0) return "helper_missing";
    for (QString current = path; current != "/"; current = QFileInfo(current).path()) {
        struct stat info{};
        if (lstat(current.toUtf8().constData(), &info) != 0 || S_ISLNK(info.st_mode) || info.st_uid != 0 ||
                (info.st_mode & (S_IWGRP | S_IWOTH)))
            return "helper_untrusted";
    }
    return S_ISREG(file.st_mode) && (file.st_mode & S_IXUSR) ? QString{} : "helper_untrusted";
}

QString BridgeHelper::trusted(QString *reason) {
    QString why = "helper_missing";
    for (const auto &candidate : QStringList{destination(), "/usr/local/libexec/omaware/authorize-bridge",
                 "/usr/libexec/omaware/authorize-bridge"}) {
        const auto found = check(candidate);
        if (found.isEmpty()) {
            if (reason) reason->clear();
            return QDir::cleanPath(candidate);
        }
        // A copy that exists but can't be trusted says more than a missing one somewhere else.
        if (found == "helper_untrusted") why = found;
    }
    if (reason) *reason = why;
    return {};
}

QString BridgeHelper::script() {
    // Packages ship it as `authorize-bridge` beside the app; builds copy it into the build folder.
    const auto beside = QCoreApplication::applicationDirPath() + "/authorize-bridge";
    return QFileInfo(beside).isFile() ? beside : QString{};
}

QStringList BridgeHelper::installCommands(const QString &script, const QString &buildDir) {
    QStringList commands;
    if (!script.isEmpty())
        commands << "sudo install -D -o root -g root -m 0755 " + quoted(script) + " " + quoted(destination());
    if (!buildDir.isEmpty()) commands << "sudo cmake --install " + quoted(buildDir) + " --component helper";
    if (commands.isEmpty())
        commands << "sudo install -D -o root -g root -m 0755 <OmaWare source>/scripts/authorize-bridge.py " +
                            quoted(destination());
    return commands;
}

QString BridgeHelper::classify(int exitCode, const QByteArray &errorOutput) {
    const auto text = QString::fromUtf8(errorOutput);
    // pkexec: 126 when the password dialog was dismissed, 127 when authorization was refused or
    // couldn't be asked for (no polkit authentication agent in this session, e.g. over SSH).
    if (text.contains("No authentication agent", Qt::CaseInsensitive)) return "authorization_unavailable";
    if (exitCode == 126) return "authorization_cancelled";
    if (exitCode == 127) return "authorization_denied";
    // The helper's own exit codes (scripts/authorize-bridge.py).
    static const QMap<int, QString> codes{{3, "network_not_owned"}, {4, "bridge_inactive"}, {5, "deny_rule"},
            {6, "bridge_policy_unsafe"}, {7, "qemu_bridge_helper_missing"}};
    if (codes.contains(exitCode)) return codes[exitCode];
    // Helpers from 1.9.0 and earlier exit with 1 and a message.
    if (text.contains("deny rule")) return "deny_rule";
    if (text.contains("not managed by OmaWare")) return "network_not_owned";
    if (text.contains("is not active")) return "bridge_inactive";
    if (text.contains("QEMU bridge helper")) return "qemu_bridge_helper_missing";
    if (text.contains("ownership") || text.contains("could not be safely checked") || text.contains("too large") ||
            text.contains("Too many included"))
        return "bridge_policy_unsafe";
    if (text.contains("Not authorized") || text.contains("not authorized")) return "authorization_denied";
    return "helper_failed";
}

QString BridgeHelper::agentMessage(const QString &code) {
    static const QMap<QString, QString> messages{
            {"helper_missing", "OmaWare's network helper isn't installed at " + destination() +
                                       ". Ask the user to install it; OmaWare shows the command when they use Allow "
                                       "VMs under Networks."},
            {"helper_untrusted", "The network helper at " + destination() +
                                         " isn't safely root-owned (it, or a folder above it, is writable by others or "
                                         "is a link), so OmaWare won't run it. Ask the user to reinstall it."},
            {"authorization_unavailable", "OmaWare couldn't ask for administrator authorization: pkexec (polkit) or a "
                                          "polkit authentication agent isn't available in the user's session."},
            {"authorization_cancelled",
                    "The user closed the administrator password prompt. Ask them before trying again."},
            {"authorization_denied",
                    "Administrator authorization was refused (wrong password, or polkit doesn't allow it)."},
            {"authorization_timeout", "Nobody answered the administrator password prompt in time."},
            {"deny_rule", "A deny rule in the host's QEMU bridge policy (bridge.conf) blocks this network. Only the "
                          "host administrator can change that."},
            {"bridge_inactive", "The network's bridge isn't active. Start the network, then authorize it again."},
            {"network_not_owned", "This network wasn't created by OmaWare, so the helper won't authorize it."},
            {"bridge_policy_unsafe", "The host's QEMU bridge policy files have unexpected ownership, permissions or "
                                     "size. The host administrator has to review them."},
            {"qemu_bridge_helper_missing", "QEMU's bridge helper (qemu-bridge-helper) is missing or not root-owned on "
                                           "this computer. Ask the user to install QEMU's bridge helper."},
            {"helper_failed", "The network helper failed. The details are in OmaWare's activity log."}};
    return messages.value(code, messages["helper_failed"]);
}
