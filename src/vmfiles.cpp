// SPDX-License-Identifier: GPL-3.0-or-later
#include "vmfiles.h"
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QFileInfo>
#include <QRegularExpression>
#include <sys/stat.h>

namespace {
// Paths are compared after resolving "..", so a disk path can't climb out of a folder.
QString clean(const QString &path) {
    return path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool inside(const QString &path, const QString &folder) {
    return !folder.isEmpty() && path.startsWith(folder + "/");
}

qint64 used(const QString &path) {
    struct stat s{};
    return ::lstat(QFile::encodeName(path).constData(), &s) == 0 && S_ISREG(s.st_mode) ? qint64(s.st_blocks) * 512 : 0;
}
}

VmFiles::Plan VmFiles::plan(
        const QStringList &xmls, const QString &uuid, const QStringList &ownedTrees, const QString &nvramFolder) {
    Plan result;
    for (const auto &tree : ownedTrees)
        if (!tree.isEmpty() && QFileInfo(tree).isDir() && !QFileInfo(tree).isSymLink()) result.trees << clean(tree);
    // vm.create names a new VM's folder "<name>-<first 8 characters of its UUID>".
    const auto marker = QRegularExpression("^[0-9a-f]{8}$").match(uuid.left(8).toLower()).hasMatch()
                                ? "-" + uuid.left(8).toLower()
                                : QString();
    const auto firmwareFolder = clean(nvramFolder);
    auto consider = [&](const QString &path, bool owned) {
        const auto source = clean(path);
        if (source.isEmpty()) return;
        const QFileInfo info(source);
        for (const auto &tree : std::as_const(result.trees))
            if (inside(source, tree)) owned = true;
        const auto folder = info.absolutePath();
        if (!owned && !marker.isEmpty() && QFileInfo(folder).fileName().toLower().endsWith(marker) &&
                !QFileInfo(folder).isSymLink()) {
            owned = true;
            if (!result.folders.contains(folder)) result.folders << folder;
        }
        if (owned && !info.isSymLink()) {
            if (!result.files.contains(source)) {
                result.files << source;
                result.kept.removeAll(source);
            }
        } else if (!result.files.contains(source) && !result.kept.contains(source))
            result.kept << source;
    };
    for (const auto &xml : xmls) {
        QDomDocument doc;
        if (!doc.setContent(xml)) continue;
        const auto root = doc.documentElement();
        for (auto disk = root.firstChildElement("devices").firstChildElement("disk"); !disk.isNull();
                disk = disk.nextSiblingElement("disk"))
            consider(disk.firstChildElement("source").attribute("file"), false);
        // libvirt keeps each VM's UEFI variables as "<name>_VARS.fd" in its nvram folder.
        const auto nvram = clean(root.firstChildElement("os").firstChildElement("nvram").text().trimmed()),
                   name = root.firstChildElement("name").text().trimmed();
        if (!nvram.isEmpty())
            consider(nvram, !firmwareFolder.isEmpty() && !name.isEmpty() &&
                                    QFileInfo(nvram).absolutePath() == firmwareFolder &&
                                    QFileInfo(nvram).fileName().startsWith(name + "_VARS"));
    }
    return result;
}

void VmFiles::exclude(Plan &plan, const QSet<QString> &referenced) {
    for (auto path : referenced) {
        path = clean(path);
        if (plan.files.removeAll(path) && !plan.kept.contains(path)) plan.kept << path;
        for (int i = plan.trees.size() - 1; i >= 0; --i)
            if (inside(path, plan.trees[i])) {
                if (!plan.kept.contains(plan.trees[i])) plan.kept << plan.trees[i];
                plan.trees.removeAt(i);
            }
    }
    // A disk inside a folder that has to stay stays with it.
    for (int i = plan.files.size() - 1; i >= 0; --i)
        for (const auto &kept : std::as_const(plan.kept))
            if (inside(plan.files[i], kept)) {
                plan.files.removeAt(i);
                break;
            }
}

qint64 VmFiles::remove(const Plan &plan, QStringList &failures) {
    qint64 freed = 0;
    for (const auto &file : plan.files) {
        if (!QFileInfo::exists(file) && !QFileInfo(file).isSymLink()) continue;
        const auto size = used(file);
        if (QFile::remove(file))
            freed += size;
        else
            failures << file;
    }
    for (const auto &tree : plan.trees) {
        if (!QFileInfo(tree).isDir() || QFileInfo(tree).isSymLink()) continue;
        qint64 size = 0;
        QDirIterator it(tree, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext())
            size += used(it.next());
        if (QDir(tree).removeRecursively())
            freed += size;
        else
            failures << tree;
    }
    for (const auto &folder : plan.folders)
        if (QFileInfo(folder).isDir() &&
                QDir(folder).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            QDir().rmdir(folder);
    return freed;
}
