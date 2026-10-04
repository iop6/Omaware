// SPDX-License-Identifier: GPL-3.0-or-later
#include "labs.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
bool validSlug(const QString &slug) {
    return QRegularExpression("^[a-z0-9][a-z0-9-]{0,23}$").match(slug).hasMatch();
}
}

QString Labs::folder() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/labs";
}

QString Labs::folderOf(const QString &slug) {
    return validSlug(slug) ? folder() + "/" + slug : QString{};
}

QString Labs::keyPath(const QString &slug) {
    const auto dir = folderOf(slug);
    return dir.isEmpty() ? QString{} : dir + "/agent_ed25519";
}

QVariantMap Labs::load(const QString &slug) {
    const auto dir = folderOf(slug);
    if (dir.isEmpty()) return {};
    QFile file(dir + "/lab.json");
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
}

QVariantList Labs::list() {
    QVariantList out;
    for (const auto &slug : QDir(folder()).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (auto lab = load(slug); !lab.isEmpty()) out.append(lab);
    return out;
}

bool Labs::save(const QVariantMap &lab) {
    const auto dir = folderOf(lab["slug"].toString());
    if (dir.isEmpty() || !QDir().mkpath(dir)) return false;
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile file(dir + "/lab.json");
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument::fromVariant(lab).toJson());
    return file.commit();
}

bool Labs::remove(const QString &slug) {
    const auto dir = folderOf(slug);
    return !dir.isEmpty() && QDir(dir).removeRecursively();
}
