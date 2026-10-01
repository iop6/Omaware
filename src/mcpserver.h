// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

// `omaware mcp`: a Model Context Protocol server on standard input and output, started by an AI agent
// (for example `claude mcp add omaware -- omaware mcp`). It describes OmaWare's tools and passes each
// call to the running OmaWare app, which must be open with agent access turned on in Settings.
namespace Mcp {
int run();
// Answers one JSON-RPC message (empty for notifications). `forward` sends a tool call to the app.
QByteArray respond(const QByteArray &message, const std::function<QVariantMap(const QString &tool, const QVariantMap &args)> &forward);
// The tool list, for tests.
QVariantList tools();
// Strict validation for the eight management tools; legacy tools keep their existing validators.
bool validateManagementArguments(const QString &tool, const QVariantMap &args, QString &error);
}
