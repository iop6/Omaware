// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

// OmaWare's root helper (scripts/authorize-bridge.py), which pkexec runs to let session VMs use the
// bridge of a network OmaWare created. Only a root-owned copy at a fixed location is ever run.
namespace BridgeHelper {
// Where the helper is installed: the one path OmaWare documents and accepts first.
QString destination();
// The installed helper OmaWare may run, or empty. `reason` gets "helper_missing" or "helper_untrusted".
QString trusted(QString *reason = nullptr);
// Whether this file may be run as root: it and every folder above it are root-owned, not writable
// by group or others and not symlinks. "" when trusted, else "helper_missing" or "helper_untrusted".
QString check(const QString &path);
// The script to install: `authorize-bridge` next to the running app (packages, builds), or empty.
QString script();
// The commands that install the helper from `script` (cmake only for a build folder).
QStringList installCommands(const QString &script, const QString &buildDir = {});
// Why running the helper failed, from pkexec's exit code and the helper's error output:
// a stable code (see agentMessage) for agents.
QString classify(int exitCode, const QByteArray &errorOutput);
// A short explanation of a code for agents: no output, credentials or paths other than destination().
QString agentMessage(const QString &code);
}
