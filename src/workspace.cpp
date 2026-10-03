// SPDX-License-Identifier: GPL-3.0-or-later
#include "workspace.h"
#include <QFile>
#include <QClipboard>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QUrl>
#include <QDirIterator>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
Workspace::Workspace(QObject *parent) : QObject(parent), settings_(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/workspace.ini", QSettings::IniFormat) {}
QVariant Workspace::get(QString key, QVariant fallback) const { return settings_.value("preferences/" + key, fallback); }
void Workspace::set(QString key, QVariant value) { settings_.setValue("preferences/" + key, value); settings_.sync(); ++revision_; emit changed(); }
QVariantMap Workspace::vm(QString uuid) const { return settings_.value("vms/" + uuid).toMap(); }
void Workspace::saveVm(QString uuid, QVariantMap value) {
    QVariantMap clean;
    for (auto key : {"alias", "folder", "tags", "notes"}) clean[key] = value.value(key).toString().left(key == QString("notes") ? 8192 : 256);
    clean["favorite"] = value.value("favorite").toBool();
    settings_.setValue("vms/" + uuid, clean); settings_.sync(); ++revision_; emit changed();
}
void Workspace::copy(QString text) { QGuiApplication::clipboard()->setText(text); }
QString Workspace::localPath(QString url) const { return url.startsWith("file:") ? QUrl(url).toLocalFile() : url; }
bool Workspace::saveText(QString url, QString text) const {
    const auto path = localPath(url);
    if (path.isEmpty()) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text.toUtf8()) >= 0 && file.commit();
}
QVariantMap Workspace::mediaFiles(QString folder) {
    const QFileInfo directory(folder);
    QVariantMap result{{"folder", folder}, {"items", QVariantList{}}};
    if (!directory.isAbsolute() || !directory.isDir() || !directory.isReadable()) {
        result["error"] = "Choose a readable local folder for your ISO library."; return result;
    }
    QVariantList items;
    // Read only this folder, on the management worker. Do not traverse disks or
    // follow directory symlinks recursively just to populate a media selector.
    QDirIterator it(directory.absoluteFilePath(), QDir::Files | QDir::Readable | QDir::NoDotAndDotDot);
    int inspected = 0;
    while (it.hasNext() && inspected++ < 20000) {
        it.next(); const auto file = it.fileInfo();
        if (file.suffix().compare("iso", Qt::CaseInsensitive) != 0) continue;
        items.append(QVariantMap{{"name", file.fileName()}, {"path", file.absoluteFilePath()}, {"bytes", file.size()}});
    }
    std::sort(items.begin(), items.end(), [](const QVariant &a, const QVariant &b) { return a.toMap()["name"].toString().localeAwareCompare(b.toMap()["name"].toString()) < 0; });
    result["items"] = items;
    if (it.hasNext()) result["notice"] = "This folder is very large. Showing ISOs from the first 20,000 files; choose a smaller folder to see the rest.";
    return result;
}
QString Workspace::guide(QString name) const {
    if (name != "USER-GUIDE.md" && name != "NETWORKS.md") return {};
    QFile file(":/docs/" + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}
