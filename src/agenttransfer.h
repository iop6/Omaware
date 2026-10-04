// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>

namespace AgentTransfer {
inline constexpr int limit = 32768;
bool validName(const QString &name);
bool validGuestPath(const QString &path);
bool read(const QString &name, QByteArray &bytes, QString &error);
bool write(const QString &name, const QByteArray &bytes, bool overwrite, QString &error);
QString command(const QString &direction, const QString &guest, const QByteArray &bytes, bool overwrite);
}
