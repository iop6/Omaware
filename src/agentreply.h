// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QVariantMap>

// Tool results as AgentBridge sends them to agents: {ok: true, result} on success, and
// {ok: false, error, result} on failure, where the result carries details for the agent.
namespace AgentReply {
inline QVariantMap success(const QVariantMap &result) {
    return {{"ok", true}, {"result", result}};
}

inline QVariantMap failure(const QString &message, const QVariantMap &result = {}) {
    return {{"ok", false}, {"error", message}, {"result", result}};
}

// A failure with a stable reason agents can act on: invalid_argument, declined, unavailable, …
inline QVariantMap failure(const QString &message, const char *code) {
    return failure(message, QVariantMap{{"code", code}});
}
}
