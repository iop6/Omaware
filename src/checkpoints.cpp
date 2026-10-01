// SPDX-License-Identifier: GPL-3.0-or-later
#include "checkpoints.h"
#include "paths.h"
#include "snapshothistory.h"
#include "domainconfig.h"
#include "configuration.h"
#include "containment.h"
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#include <QElapsedTimer>
#include <QThread>
#include <QDirIterator>
#include <QRegularExpression>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QUrl>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QCoreApplication>
#include <csignal>
#include <cerrno>
#include <sys/stat.h>
#include <libvirt/virterror.h>
#include <libvirt/libvirt-qemu.h>
#include <algorithm>

namespace {
QString rootPath(QString uuid) { return QUuid(uuid).isNull() ? QString{} : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + uuid; }
QVariantMap read(QString path) { QFile f(path); if (!f.open(QIODevice::ReadOnly) || f.size() > 8 * 1024 * 1024) return {}; return QJsonDocument::fromJson(f.readAll()).toVariant().toMap(); }
bool write(QString path, QVariantMap values, QString &error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) || file.write(QJsonDocument::fromVariant(values).toJson()) < 0 || !file.commit()) { error = "Could not save checkpoint metadata: " + file.errorString(); return false; }
    return true;
}
QString uuidOf(virDomainPtr domain) { char uuid[VIR_UUID_STRING_BUFLEN]; return virDomainGetUUIDString(domain, uuid) == 0 ? QString::fromUtf8(uuid) : QString{}; }
// A libvirt numeric domain ID can be reused after a daemon/host restart.
// Bind incremental tracking and pause recovery to the actual QEMU process epoch.
QString runtimeToken(QString uuid) {
    QFile boot("/proc/sys/kernel/random/boot_id"); if (!boot.open(QIODevice::ReadOnly)) return {};
    const auto bootId = QString::fromLatin1(boot.readAll().trimmed());
    for (const auto &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!pid.front().isDigit()) continue;
        QFile cmd("/proc/" + pid + "/cmdline"); if (!cmd.open(QIODevice::ReadOnly)) continue;
        auto args = cmd.read(65536).split('\0');
        if (args.isEmpty() || !QFileInfo(QString::fromLocal8Bit(args.first())).fileName().startsWith("qemu-system-")) continue;
        int flag = args.indexOf("-uuid"); if (flag < 0 || args.value(flag + 1) != uuid.toUtf8()) continue;
        QFile stat("/proc/" + pid + "/stat"); if (!stat.open(QIODevice::ReadOnly)) return {};
        auto line = stat.readAll(); auto ticks = line.mid(line.lastIndexOf(')') + 2).split(' ').value(19);
        return ticks.toULongLong() ? bootId + ":" + pid + ":" + QString::fromLatin1(ticks) : QString{};
    }
    return {};
}
QString lastError(QString context) { auto e = virGetLastError(); return context + ": " + QString::fromUtf8(e && e->message ? e->message : "Unknown libvirt error"); }
QString xmlOf(virDomainPtr domain, unsigned flags) { char *raw = virDomainGetXMLDesc(domain, flags | VIR_DOMAIN_XML_SECURE); QString xml = raw ? QString::fromUtf8(raw) : QString{}; free(raw); return xml; }
qulonglong allocated(QString path) { struct stat info{}; return ::stat(path.toUtf8().constData(), &info) == 0 ? qulonglong(info.st_blocks) * 512 : 0; }
QVariantMap jobStats(virDomainPtr domain, unsigned flags, QString &error) {
    int type = VIR_DOMAIN_JOB_NONE, count = 0; virTypedParameterPtr params = nullptr;
    if (virDomainGetJobStats(domain, &type, &params, &count, flags) < 0) { error = lastError("Read checkpoint copy status"); return {}; }
    QVariantMap result{{"type", type}};
    for (int i = 0; i < count; ++i) {
        auto &p = params[i];
        if (p.type == VIR_TYPED_PARAM_INT) result[p.field] = p.value.i;
        else if (p.type == VIR_TYPED_PARAM_ULLONG) result[p.field] = qulonglong(p.value.ul);
        else if (p.type == VIR_TYPED_PARAM_BOOLEAN) result[p.field] = bool(p.value.b);
        else if (p.type == VIR_TYPED_PARAM_STRING) result[p.field] = QString::fromUtf8(p.value.s);
    }
    virTypedParamsFree(params, count); return result;
}
bool ownBackup(virDomainPtr domain, QString directory) {
    char *raw = virDomainBackupGetXMLDesc(domain, 0); if (!raw) return false;
    QDomDocument doc; bool valid = bool(doc.setContent(QString::fromUtf8(raw))); free(raw);
    bool found = false;
    for (auto d = doc.documentElement().firstChildElement("disks").firstChildElement("disk"); valid && !d.isNull(); d = d.nextSiblingElement("disk")) {
        if (d.attribute("backup") == "no") continue;
        if (QFileInfo(d.firstChildElement("target").attribute("file")).absolutePath() != directory) return false;
        found = true;
    }
    return valid && found;
}
bool unchanged(virDomainPtr domain, QString xml) {
    if (virDomainIsActive(domain) != 0) return false;
    char *raw = virDomainGetXMLDesc(domain, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE);
    bool same = raw && DomainConfig::revision(QString::fromUtf8(raw)) == DomainConfig::revision(xml); free(raw); return same;
}
bool process(QStringList args, const std::atomic_bool *cancel, Checkpoints::Progress progress, QString &error, QByteArray *output = nullptr) {
    if (cancel && *cancel) { error = "Checkpoint operation cancelled."; return false; }
    QProcess p; p.start("qemu-img", args); QElapsedTimer timer; timer.start(); QByteArray data;
    if (!p.waitForStarted(5000)) { error = "Could not start qemu-img."; return false; }
    while (!p.waitForFinished(100)) {
        data += p.readAllStandardOutput();
        if ((cancel && *cancel) || timer.elapsed() > 600000) {
            p.kill(); p.waitForFinished(3000); error = cancel && *cancel ? "Checkpoint operation cancelled." : "Disk operation exceeded ten minutes."; return false;
        }
        if (progress) {
            auto matches = QRegularExpression("\\(([0-9.]+)/100%\\)").globalMatch(QString::fromUtf8(data.right(4096)));
            double percent = -1; while (matches.hasNext()) percent = matches.next().captured(1).toDouble();
            if (percent >= 0) progress("Copying disk", qulonglong(percent * 100), 10000);
        }
        if (data.size() > 8 * 1024 * 1024) data = data.right(4096);
    }
    data += p.readAllStandardOutput(); if (output) *output = data;
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode()) { error = "Disk operation failed: " + QString::fromUtf8(p.readAllStandardError()).left(4000); return false; }
    return true;
}
bool copyDisk(QString source, QString format, QString destination, QString &error, const std::atomic_bool *cancel = nullptr, Checkpoints::Progress progress = {}, QString base = {}) {
    if (!QFileInfo(source).isAbsolute() || !QFileInfo(source).isFile() || !QStringList{"raw", "qcow2"}.contains(format)) { error = "Checkpoints require readable local raw or qcow2 disks."; return false; }
    QStringList args{"convert", "-p", "-f", format, "-O", "qcow2"};
    if (!base.isEmpty()) args << "-B" << base << "-F" << "qcow2";
    args << source << destination;
    QByteArray info;
    if (!process({"info", "--output=json", "-f", format, source}, cancel, {}, error, &info)) return false;
    const auto size = QJsonDocument::fromJson(info).toVariant().toMap()["virtual-size"].toULongLong();
    if (progress) progress("Copying disk", 0, size);
    auto report = [&](QString phase, qulonglong done, qulonglong total) { if (progress) progress(phase, total ? size * double(done) / total : 0, size); };
    if (!process(args, cancel, report, error)) return false;
    if (progress) progress("Copying disk", size, size);
    QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner); return true;
}
bool validManifest(QVariantMap manifest, QString uuid, QString id) { return !QUuid(id).isNull() && manifest["uuid"] == uuid && manifest["id"] == id && manifest["kind"] == "copy"; }
QString vmRoot(QString uuid) { return Paths::vmDir(uuid); }
// An emulated TPM (Windows 11 needs one) keeps its state outside the VM definition: libvirt's session
// daemon stores it per VM UUID in the user's config folder and keeps it when the VM is redefined.
QString tpmState(const QString &uuid) {
    return qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/libvirt/qemu/swtpm/" + uuid + "/tpm2/tpm2-00.permall";
}
bool hasTpm(const QString &xml) {
    QDomDocument doc; doc.setContent(xml);
    const auto tpms = doc.elementsByTagName("tpm");
    for (int i = 0; i < tpms.size(); ++i) if (tpms.at(i).toElement().firstChildElement("backend").attribute("type") == "emulator") return true;
    return false;
}
// Records the TPM state in a checkpoint ("tpm" is empty when the TPM has never been started).
bool captureTpm(const QString &uuid, const QString &xml, const QString &directory, QVariantMap &manifest, QString &error) {
    if (!hasTpm(xml)) return true;
    manifest["tpm"] = QString();
    if (!QFile::exists(tpmState(uuid))) return true;
    if (!QFile::copy(tpmState(uuid), directory + "/tpm-state")) { error = "Could not copy the VM's TPM state."; return false; }
    QFile::setPermissions(directory + "/tpm-state", QFile::ReadOwner | QFile::WriteOwner);
    manifest["tpm"] = "tpm-state";
    return true;
}
// Puts a saved TPM state in place (none: a TPM that hadn't started yet), keeping the current one in backup.
bool installTpm(const QString &uuid, const QString &from, const QString &backup, QString &error) {
    const auto target = tpmState(uuid);
    QFile::remove(backup);
    if (QFile::exists(target) && !QFile::copy(target, backup)) { error = "Could not keep the VM's current TPM state."; return false; }
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::remove(target);
    if (from.isEmpty()) return true;
    if (!QFile::copy(from, target)) { error = "Could not restore the VM's TPM state."; return false; }
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}
void putBackTpm(const QString &uuid, const QString &backup) {
    if (backup.isEmpty()) return;
    QFile::remove(tpmState(uuid));
    if (QFile::exists(backup)) QFile::copy(backup, tpmState(uuid));
}
bool safeFile(QString directory, QString file) { return !file.isEmpty() && QFileInfo(file).fileName() == file && !QFileInfo(directory + "/" + file).isSymLink() && QFileInfo(directory + "/" + file).isFile(); }
// Save images retain the live CPU/device ABI. Only relocate storage in that XML;
// an inactive definition can omit runtime details needed to load saved devices.
QString memoryRestoreXml(virConnectPtr connection, QString file, QString targetXml, QString &error) {
    char *raw = virDomainSaveImageGetXMLDesc(connection, file.toUtf8().constData(), VIR_DOMAIN_SAVE_IMAGE_XML_SECURE);
    if (!raw) { error = lastError("Read saved VM memory"); return {}; }
    QDomDocument saved, target;
    const bool valid = saved.setContent(QString::fromUtf8(raw)) && target.setContent(targetXml); free(raw);
    if (!valid || saved.documentElement().firstChildElement("uuid").text() != target.documentElement().firstChildElement("uuid").text()) { error = "Saved memory belongs to a different VM or has invalid configuration."; return {}; }
    auto disks = target.documentElement().firstChildElement("devices");
    for (auto disk = saved.documentElement().firstChildElement("devices").firstChildElement("disk"); !disk.isNull(); disk = disk.nextSiblingElement("disk")) {
        if (disk.attribute("device") != "disk" || !disk.firstChildElement("readonly").isNull()) continue;
        bool found = false;
        for (auto dest = disks.firstChildElement("disk"); !dest.isNull(); dest = dest.nextSiblingElement("disk")) {
            if (dest.firstChildElement("target").attribute("dev") != disk.firstChildElement("target").attribute("dev")) continue;
            disk.replaceChild(saved.importNode(dest.firstChildElement("source"), true), disk.firstChildElement("source"));
            disk.firstChildElement("driver").setAttribute("type", dest.firstChildElement("driver").attribute("type"));
            disk.removeChild(disk.firstChildElement("backingStore")); found = true; break;
        }
        if (!found) { error = "Saved memory and snapshot disks do not match."; return {}; }
    }
    auto nvram = saved.documentElement().firstChildElement("os").firstChildElement("nvram");
    auto desired = target.documentElement().firstChildElement("os").firstChildElement("nvram");
    if (!nvram.isNull()) {
        if (desired.isNull()) { error = "Saved memory is missing its firmware variables."; return {}; }
        while (!nvram.firstChild().isNull()) nvram.removeChild(nvram.firstChild());
        nvram.appendChild(saved.createTextNode(desired.text()));
    }
    return saved.toString(-1);
}
qulonglong directoryBytes(QString path) { qulonglong bytes = 0; QDirIterator it(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories); while (it.hasNext()) bytes += allocated(it.next()); return bytes; }
void fields(QVariantMap &manifest, QVariantMap options, QString directory) {
    manifest["tags"] = options.value("tags", manifest.value("tags")).toString().left(256);
    manifest["pinned"] = options.value("pinned", manifest.value("pinned", false)).toBool();
    manifest["knownGood"] = options.value("knownGood", manifest.value("knownGood", false)).toBool();
    manifest["safety"] = options.value("safety", manifest.value("safety", false)).toBool();
    auto preview = options["preview"].toString();
    if (preview.startsWith("data:image/png;base64,") && preview.size() < 2 * 1024 * 1024) {
        QByteArray bytes = QByteArray::fromBase64(preview.mid(22).toLatin1()); QBuffer input(&bytes); input.open(QIODevice::ReadOnly);
        QImageReader reader(&input, "png"); auto size = reader.size();
        if (size.isValid() && size.width() <= 1280 && size.height() <= 720) {
            auto image = reader.read();
            if (!image.isNull() && image.scaled(640, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(directory + "/preview.png")) QFile::setPermissions(directory + "/preview.png", QFile::ReadOwner | QFile::WriteOwner);
        }
    }
}
bool chain(QString uuid, QString id, QString &error, QSet<QString> *seen = nullptr, bool verifying = false) {
    QSet<QString> local; if (!seen) seen = &local;
    const auto directory = rootPath(uuid) + "/" + id;
    if (QUuid(id).isNull() || seen->contains(id) || seen->size() > 32 || QFileInfo(directory).isSymLink()) { error = "Invalid or cyclic checkpoint dependency."; return false; }
    seen->insert(id); auto manifest = read(directory + "/manifest.json");
    if (!verifying && !manifest["verificationError"].toString().isEmpty()) { error = manifest["verificationError"].toString(); return false; }
    if (!verifying) for (auto value : manifest["fingerprints"].toMap().toStdMap()) {
        QFileInfo file(directory + "/" + value.first); const auto stamp = value.second.toMap();
        if (file.size() != stamp["size"].toLongLong() || file.lastModified().toMSecsSinceEpoch() != stamp["modified"].toLongLong()) { error = "Checkpoint files changed since verification. Run Verify before restoring."; return false; }
    }
    if (!validManifest(manifest, uuid, id) || manifest["disks"].toList().isEmpty()) { error = "Missing checkpoint metadata or disks."; return false; }
    for (auto disk : manifest["disks"].toList()) if (!safeFile(directory, disk.toMap()["file"].toString())) { error = "Missing checkpoint disk."; return false; }
    if (!manifest["memory"].toString().isEmpty()) {
        if (!safeFile(directory, manifest["memory"].toString()) || QFileInfo(directory + "/" + manifest["memory"].toString()).size() == 0) { error = "Missing saved VM memory. This snapshot cannot resume its captured state."; return false; }
        if (manifest["memoryState"].toInt() != VIR_DOMAIN_RUNNING && manifest["memoryState"].toInt() != VIR_DOMAIN_PAUSED) { error = "Invalid saved memory state."; return false; }
    }
    QDomDocument doc; doc.setContent(manifest["xml"].toString());
    if (!doc.documentElement().firstChildElement("os").firstChildElement("nvram").text().isEmpty() && !safeFile(directory, manifest["nvram"].toString())) { error = "Missing firmware variables."; return false; }
    if (!manifest.value("tpm").toString().isEmpty() && !safeFile(directory, manifest.value("tpm").toString())) { error = "Missing TPM state."; return false; }
    auto base = manifest["baseId"].toString(); return base.isEmpty() || chain(uuid, base, error, seen, verifying);
}
bool diskChain(QString uuid, QString id, const std::atomic_bool *cancel, QString &error) {
    QSet<QString> ancestry;
    if (!chain(uuid, id, error, &ancestry, true)) return false;
    for (const auto &point : ancestry) {
        const auto directory = rootPath(uuid) + "/" + point;
        const auto manifest = read(directory + "/manifest.json");
        const auto base = read(rootPath(uuid) + "/" + manifest["baseId"].toString() + "/manifest.json");
        for (auto value : manifest["disks"].toList()) {
            const auto disk = value.toMap(); QString expected;
            if (!manifest["baseId"].toString().isEmpty()) {
                for (auto d : base["disks"].toList()) if (d.toMap()["target"] == disk["target"]) expected = rootPath(uuid) + "/" + base["id"].toString() + "/" + d.toMap()["file"].toString();
                if (expected.isEmpty()) { error = "Missing checkpoint backing disk record."; return false; }
            }
            QByteArray output;
            if (!process({"info", "--output=json", "-f", "qcow2", directory + "/" + disk["file"].toString()}, cancel, {}, error, &output)) return false;
            auto image = QJsonDocument::fromJson(output).toVariant().toMap();
            if (image["format"] != "qcow2" || image["full-backing-filename"].toString() != expected || (!expected.isEmpty() && image["backing-filename-format"] != "qcow2")) { error = "Checkpoint disk dependencies no longer match their saved metadata."; return false; }
        }
    }
    return true;
}
bool finishManifest(QString directory, QVariantMap manifest, QVariantMap options, QString &error) {
    manifest.remove("pid");
    fields(manifest, options, directory);
    const auto uuid = manifest["uuid"].toString();
    const auto marker = read(rootPath(uuid) + "/current.json");
    if (!write(directory + "/manifest.json", manifest, error)) return false;
    QFile::remove(directory + "/building.json");
    return write(rootPath(uuid) + "/current.json", SnapshotHistory::captured(marker, manifest), error);
}
void forgetBitmap(virDomainPtr domain, QString name) {
    if (!name.startsWith("omaware-cp-")) return;
    auto checkpoint = virDomainCheckpointLookupByName(domain, name.toUtf8().constData(), 0);
    if (!checkpoint) return;
    if (virDomainCheckpointDelete(checkpoint, 0) < 0) virDomainCheckpointDelete(checkpoint, VIR_DOMAIN_CHECKPOINT_DELETE_METADATA_ONLY);
    virDomainCheckpointFree(checkpoint);
}

}
QVariantList Checkpoints::list(QString uuid) {
    const auto root = rootPath(uuid); if (root.isEmpty()) return {};
    const auto current = read(root + "/current.json"); QMap<QString, QVariantMap> all; QSet<QString> parents;
    for (auto id : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QUuid(id).isNull() || QFileInfo(root + "/" + id).isSymLink()) continue;
        if (current["deletedSnapshots"].toMap().contains(id)) continue;
        auto data = read(root + "/" + id + "/manifest.json"); if (!validManifest(data, uuid, id)) continue;
        data["parentId"] = SnapshotHistory::survivingParent(current, data["parentId"].toString());
        parents.insert(data["parentId"].toString()); data["bytes"] = directoryBytes(root + "/" + id);
        data["diskCount"] = data["disks"].toList().size(); data["capture"] = data.value("capture", "stopped");
        QString error; data["healthy"] = chain(uuid, id, error); if (!data["verificationError"].toString().isEmpty()) { error = data["verificationError"].toString(); data["healthy"] = false; } data["health"] = error.isEmpty() ? (data["verifiedAt"].toLongLong() > 0 ? "Verified" : "Not verified") : error;
        data.remove("xml"); data.remove("disks"); data["current"] = id == current["id"];
        data["currentLabel"] = current.value("action", "captured") == "restored" ? "Last restored" : "Last captured";
        data["state"] = data["capture"] == "live" ? "Captured while running" : data["capture"] == "paused" ? "Captured while paused" : "Captured while stopped";
        data["previewUrl"] = safeFile(root + "/" + id, "preview.png") ? QUrl::fromLocalFile(root + "/" + id + "/preview.png").toString() : QString{};
        all[id] = data;
    }
    QStringList order = all.keys();
    std::stable_sort(order.begin(), order.end(), [&](QString a, QString b) { return all[a]["time"].toLongLong() < all[b]["time"].toLongLong(); });
    QVariantList items; QSet<QString> visited;
    std::function<void(QString, int)> add = [&](QString id, int depth) {
        if (visited.contains(id)) return;
        visited.insert(id); auto item = all[id]; auto parent = item["parentId"].toString();
        item["parent"] = all.contains(parent) ? all[parent]["name"] : QVariant{};
        item["depth"] = depth; item["leaf"] = !parents.contains(id); items.append(item);
        for (const auto &child : order) if (all[child]["parentId"] == id) add(child, depth + 1);
    };
    for (const auto &id : order) if (!all.contains(all[id]["parentId"].toString())) add(id, 0);
    for (const auto &id : order) if (!visited.contains(id)) add(id, 0);
    return items;
}
bool Checkpoints::create(virDomainPtr domain, QString xml, QString name, QString notes, QString &error, QVariantMap options, const std::atomic_bool *cancel, Progress progress) {
    const auto uuid = uuidOf(domain), root = rootPath(uuid), id = QUuid::createUuid().toString(QUuid::WithoutBraces), directory = root + "/" + id;
    if (root.isEmpty() || !unchanged(domain, xml)) { error = "The VM must remain stopped and unchanged while creating a checkpoint."; return false; }
    for (auto v : list(uuid)) if (v.toMap()["name"] == name) { error = "A checkpoint with this name already exists."; return false; }
    if (QFile::exists(root + "/freeze.json") || QFile::exists(root + "/restore.json")) { error = "Recover interrupted checkpoint work from Checkpoint storage first."; return false; }
    if (!QDir().mkpath(root) || !QDir().mkdir(directory)) { error = "Could not create the checkpoint directory."; return false; }
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto rollback = [&] { QDir(directory).removeRecursively(); };
    if (!write(directory + "/building.json", {{"uuid", uuid}, {"id", id}, {"name", name}, {"pid", QCoreApplication::applicationPid()}}, error)) { rollback(); return false; }
    QString failure; auto info = DomainConfig::describe(xml, failure); QVariantList disks;
    qulonglong capacity = 0;
    for (auto v : info["disks"].toList()) {
        auto disk = v.toMap(); if (disk["device"] != "disk" || disk["readOnly"].toBool()) continue;
        virDomainBlockInfo size{};
        if (virDomainGetBlockInfo(domain, disk["target"].toString().toUtf8().constData(), &size, 0) < 0) { rollback(); error = "Could not read disk capacity before making the checkpoint."; return false; }
        capacity += size.capacity;
    }
    if (QStorageInfo(directory).bytesAvailable() < qint64(capacity)) { rollback(); error = "Not enough free space for independent checkpoint disk copies."; return false; }
    for (auto v : info["disks"].toList()) {
        auto disk = v.toMap(); if (disk["device"] != "disk" || disk["readOnly"].toBool()) continue;
        const auto file = QString("disk-%1.qcow2").arg(disks.size());
        if (!copyDisk(disk["source"].toString(), disk["format"].toString(), directory + "/" + file, error, cancel, progress)) { rollback(); return false; }
        disks.append(QVariantMap{{"target", disk["target"]}, {"file", file}, {"capacity", qulonglong(0)}});
    }
    QString nvram;
    if (!info["nvram"].toString().isEmpty()) {
        nvram = "firmware-vars";
        QDomDocument firmware; firmware.setContent(xml);
        auto format = firmware.documentElement().firstChildElement("os").firstChildElement("nvram").attribute("format", "raw");
        auto vars = info["nvram"].toString();
        if (!QFileInfo(vars).exists()) vars = firmware.documentElement().firstChildElement("os").firstChildElement("nvram").attribute("template");
        const bool copied = format == "qcow2" ? copyDisk(vars, format, directory + "/" + nvram, error, cancel, progress) : format == "raw" && !vars.isEmpty() && QFile::copy(vars, directory + "/" + nvram);
        if (!copied) { rollback(); error = "Could not copy the UEFI variable store. The checkpoint was not created. " + error; return false; }
        QFile::setPermissions(directory + "/" + nvram, QFile::ReadOwner | QFile::WriteOwner);
    }
    QVariantMap tpm;
    if (!captureTpm(uuid, xml, directory, tpm, error)) { rollback(); return false; }
    if (!unchanged(domain, xml)) { rollback(); error = "The VM started or its configuration changed during the copy. The checkpoint was discarded."; return false; }
    const auto current = read(root + "/current.json");
    QVariantMap manifest{{"uuid", uuid}, {"id", id}, {"name", name}, {"notes", notes}, {"kind", "copy"}, {"time", QDateTime::currentSecsSinceEpoch()}, {"xml", xml}, {"disks", disks}, {"nvram", nvram}, {"capacity", capacity}, {"parentId", current["id"]}, {"parent", current["name"]}};
    manifest.insert(tpm);
    if (!finishManifest(directory, manifest, options, error)) { rollback(); return false; }
    return true;
}
bool Checkpoints::createLive(virDomainPtr domain, QString xml, QString name, QString notes, const std::atomic_bool &cancel, Progress progress, QString &error, QVariantMap options, bool holdForRestore) {
    const auto uuid = uuidOf(domain), root = rootPath(uuid), id = QUuid::createUuid().toString(QUuid::WithoutBraces), directory = root + "/" + id;
    const bool memory = options.value("memory", false).toBool();
    if (memory && options.value("clean", false).toBool()) { error = "Memory snapshots already preserve the running filesystem state. Disable filesystem freezing for this capture."; return false; }
    int state = 0, reason = 0;
    if (root.isEmpty() || virDomainGetState(domain, &state, &reason, 0) < 0 || (state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED)) { error = "A live checkpoint requires a running or paused VM."; return false; }
    for (auto v : list(uuid)) if (v.toMap()["name"] == name) { error = "A checkpoint with this name already exists."; return false; }
    const auto liveXml = xmlOf(domain, 0);
    if (liveXml.isEmpty() || !Configuration::changes(xml, liveXml).isEmpty()) { error = "Saved hardware differs from the running VM. Discard pending changes or apply them before creating a checkpoint."; return false; }
    QString failure;
    auto existing = jobStats(domain, 0, failure);
    if (!failure.isEmpty() || existing["type"].toInt() != VIR_DOMAIN_JOB_NONE) { error = failure.isEmpty() ? "Another VM job is active. Wait for it to finish before creating a checkpoint." : failure; return false; }
    if (cancel) { error = "Checkpoint cancelled. The VM was not changed."; return false; }
    if (QFile::exists(root + "/freeze.json") || QFile::exists(root + "/restore.json")) { error = "Recover interrupted checkpoint work from Checkpoint storage first."; return false; }
    if (!QDir().mkpath(root) || !QDir().mkdir(directory)) { error = "Could not create the checkpoint directory."; return false; }
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto rollback = [&] { QDir(directory).removeRecursively(); };
    auto info = DomainConfig::describe(xml, failure); QVariantList disks; qulonglong capacity = 0;
    QDomDocument backup; auto b = backup.createElement("domainbackup"); b.setAttribute("mode", "push"); backup.appendChild(b);
    auto diskList = backup.createElement("disks"); b.appendChild(diskList);
    for (auto v : info["disks"].toList()) {
        auto disk = v.toMap(); if (disk["device"] != "disk" || disk["readOnly"].toBool()) continue;
        virDomainBlockInfo size{};
        if (virDomainGetBlockInfo(domain, disk["target"].toString().toUtf8().constData(), &size, 0) < 0) { error = lastError("Read checkpoint disk capacity"); rollback(); return false; }
        capacity += size.capacity;
        const auto file = QString("disk-%1.qcow2").arg(disks.size());
        auto d = backup.createElement("disk"); d.setAttribute("name", disk["target"].toString()); d.setAttribute("type", "file"); d.setAttribute("backup", "yes"); diskList.appendChild(d);
        auto target = backup.createElement("target"); target.setAttribute("file", directory + "/" + file); d.appendChild(target);
        auto driver = backup.createElement("driver"); driver.setAttribute("type", "qcow2"); d.appendChild(driver);
        disks.append(QVariantMap{{"target", disk["target"]}, {"file", file}});
    }
    if (disks.isEmpty()) { error = "Attach a writable disk first."; rollback(); return false; }
    const auto current = read(root + "/current.json");
    const auto epoch = runtimeToken(uuid);
    bool tracked = !memory && options.value("incremental", false).toBool() && !epoch.isEmpty();
    for (auto v : info["disks"].toList()) if (v.toMap()["device"] == "disk" && !v.toMap()["readOnly"].toBool() && v.toMap()["format"] != "qcow2") tracked = false;
    const auto bitmap = "omaware-cp-" + id;
    bool published = false;
    auto bitmapGuard = qScopeGuard([&] { if (published || !tracked) return; QString ignored; auto job = jobStats(domain, 0, ignored); if (virDomainIsActive(domain) == 0 || (ignored.isEmpty() && job["type"].toInt() == VIR_DOMAIN_JOB_NONE)) forgetBitmap(domain, bitmap); });
    auto base = read(root + "/" + current["id"].toString() + "/manifest.json");
    QString chainError;
    bool incremental = tracked && base["runtimeToken"] == epoch && base["runtimeId"].toUInt() == virDomainGetID(domain) && base["sourceRevision"] == DomainConfig::revision(xml) && base["chainDepth"].toInt() < 8 && !base["bitmap"].toString().isEmpty() && chain(uuid, base["id"].toString(), chainError);
    qulonglong required = capacity;
    if (incremental && !diskChain(uuid, base["id"].toString(), &cancel, error)) { rollback(); return false; }
    if (incremental) {
        auto prior = virDomainCheckpointLookupByName(domain, base["bitmap"].toString().toUtf8().constData(), 0);
        incremental = prior;
        if (prior) {
            char *raw = virDomainCheckpointGetXMLDesc(prior, VIR_DOMAIN_CHECKPOINT_XML_SIZE);
            QDomDocument sizes;
            if (raw && sizes.setContent(QString::fromUtf8(raw))) {
                required = 64 * 1024 * 1024;
                for (auto d = sizes.documentElement().firstChildElement("disks").firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk")) required += d.attribute("size").toULongLong();
            }
            free(raw); virDomainCheckpointFree(prior);
        }
    }
    if (memory) required += info["memoryMiB"].toULongLong() * 1048576 + 256 * 1048576;
    if (QStorageInfo(directory).bytesAvailable() < qint64(required)) { error = "Not enough free space for the snapshot's disks and requested memory."; rollback(); return false; }
    if (incremental) {
        auto inc = backup.createElement("incremental"); inc.appendChild(backup.createTextNode(base["bitmap"].toString())); b.appendChild(inc);
        const auto previousDisks = base["disks"].toList();
        for (auto disk : disks) {
            auto record = disk.toMap(); QString backing;
            for (auto v : previousDisks) if (v.toMap()["target"] == record["target"]) backing = root + "/" + base["id"].toString() + "/" + v.toMap()["file"].toString();
            if (backing.isEmpty() || !process({"create", "-f", "qcow2", "-F", "qcow2", "-b", backing, directory + "/" + record["file"].toString()}, &cancel, {}, error)) { rollback(); return false; }
        }
    }
    QDomDocument checkpoint; auto cp = checkpoint.createElement("domaincheckpoint"); checkpoint.appendChild(cp);
    auto cpName = checkpoint.createElement("name"); cpName.appendChild(checkpoint.createTextNode(bitmap)); cp.appendChild(cpName);
    auto cpDisks = checkpoint.createElement("disks"); cp.appendChild(cpDisks);
    for (auto disk : disks) { auto e = checkpoint.createElement("disk"); e.setAttribute("name", disk.toMap()["target"].toString()); e.setAttribute("checkpoint", "bitmap"); cpDisks.appendChild(e); }

    QVariantMap manifest{{"uuid", uuid}, {"id", id}, {"name", name}, {"notes", notes}, {"kind", "copy"}, {"time", QDateTime::currentSecsSinceEpoch()}, {"xml", xml}, {"disks", disks}, {"capacity", capacity}, {"parentId", current["id"]}, {"parent", current["name"]}, {"capture", state == VIR_DOMAIN_RUNNING ? "live" : "paused"}, {"consistency", "crash-consistent"}};
    manifest["storageMode"] = incremental ? "incremental" : "full";
    if (tracked) { manifest["bitmap"] = bitmap; manifest["sourceRevision"] = DomainConfig::revision(xml); manifest["runtimeId"] = virDomainGetID(domain); manifest["runtimeToken"] = epoch; }
    if (incremental) { manifest["baseId"] = base["id"]; manifest["chainDepth"] = base["chainDepth"].toInt() + 1; }
    // Incomplete work is never listed as a usable checkpoint. Keep a private journal
    // for diagnosis if the process or connection disappears during the backup.
    manifest["pid"] = QCoreApplication::applicationPid();
    if (!write(directory + "/building.json", manifest, error)) { rollback(); return false; }
    QVariantMap captureJournal{{"uuid", uuid}, {"id", id}, {"runtimeId", virDomainGetID(domain)}, {"runtimeToken", epoch}, {"time", QDateTime::currentSecsSinceEpoch()}, {"frozen", false}, {"paused", false}};
    auto saveCaptureJournal = [&] { return write(root + "/freeze.json", captureJournal, error); };
    auto clearCaptureJournal = [&] { if (!captureJournal["frozen"].toBool() && !captureJournal["paused"].toBool()) QFile::remove(root + "/freeze.json"); else saveCaptureJournal(); };
    auto buildingGuard = qScopeGuard([&] { if (QFile::exists(directory + "/building.json")) { auto record = read(directory + "/building.json"); record["pid"] = 0; QString ignored; write(directory + "/building.json", record, ignored); } });
    bool pausedByUs = false;
    auto resume = [&] {
        if (!pausedByUs) return true;
        int currentState = 0, currentReason = 0;
        if (virDomainGetState(domain, &currentState, &currentReason, 0) == 0 && currentState == VIR_DOMAIN_RUNNING) { pausedByUs = false; captureJournal["paused"] = false; clearCaptureJournal(); return true; }
        if (currentState != VIR_DOMAIN_PAUSED || currentReason != VIR_DOMAIN_PAUSED_USER || virDomainResume(domain) < 0) { error += " The VM could not be resumed automatically; check its state and use Resume."; return false; }
        pausedByUs = false; captureJournal["paused"] = false; clearCaptureJournal(); return true;
    };
    bool frozen = false;
    auto thaw = [&] {
        if (!frozen) return true;
        if (virDomainFSThaw(domain, nullptr, 0, 0) < 0) { error += " Guest filesystems could not be thawed. Use Recover guest writes in Checkpoint storage."; return false; }
        frozen = false; captureJournal["frozen"] = false; clearCaptureJournal(); return true;
    };
    auto thawGuard = qScopeGuard([&] { resume(); thaw(); });
    if (options["clean"].toBool()) {
        if (state != VIR_DOMAIN_RUNNING || !info.value("agentConnected").toBool()) {
            QString ignored; auto runningInfo = DomainConfig::describe(liveXml, ignored);
            if (state != VIR_DOMAIN_RUNNING || !runningInfo["agentConnected"].toBool()) { error = "Cleaner capture requires a running VM with a connected QEMU guest agent."; rollback(); return false; }
        }
        char *status = virDomainQemuAgentCommand(domain, "{\"execute\":\"guest-fsfreeze-status\"}", 5, 0);
        const auto state = status ? QJsonDocument::fromJson(status).toVariant().toMap()["return"].toString() : QString{}; free(status);
        if (state != "thawed") { error = "Guest filesystems are already frozen or their status is unavailable. Finish the other guest operation first."; rollback(); return false; }
        captureJournal["frozen"] = true;
        if (!saveCaptureJournal()) { rollback(); return false; }
        frozen = true;
        const int frozenCount = virDomainFSFreeze(domain, nullptr, 0, 0);
        if (frozenCount <= 0) { error = frozenCount < 0 ? lastError("Flush and freeze guest filesystems") : "The guest agent did not freeze any filesystems. Use an ordinary disk capture for this guest."; thaw(); rollback(); return false; }
        manifest["consistency"] = "filesystem-consistent";
    }
    progress("Preparing live checkpoint", 0, 0);
    // Hold CPUs between saving device/RAM state and the atomic disk backup start.
    // The guest then resumes while libvirt copies the captured disks in background.
    if (memory || !info["nvram"].toString().isEmpty() || hasTpm(xml)) {
        if (state == VIR_DOMAIN_RUNNING) {
            captureJournal["paused"] = true;
            if (!saveCaptureJournal()) { rollback(); return false; }
            pausedByUs = true;
            if (virDomainSuspend(domain) < 0) { error = lastError("Pause for a consistent snapshot"); resume(); rollback(); return false; }
        }
    }
    if (memory) {
        QDomDocument snapshot; auto root = snapshot.createElement("domainsnapshot"); snapshot.appendChild(root);
        auto mem = snapshot.createElement("memory"); mem.setAttribute("snapshot", "external"); mem.setAttribute("file", directory + "/memory.save"); root.appendChild(mem);
        auto skipped = snapshot.createElement("disks"); root.appendChild(skipped);
        for (auto value : info["disks"].toList()) {
            auto disk = snapshot.createElement("disk"); disk.setAttribute("name", value.toMap()["target"].toString()); disk.setAttribute("snapshot", "no"); skipped.appendChild(disk);
        }
        progress("Saving VM memory and device state", 0, 0);
        auto point = virDomainSnapshotCreateXML(domain, snapshot.toString(-1).toUtf8().constData(), VIR_DOMAIN_SNAPSHOT_CREATE_NO_METADATA);
        if (!point) { error = lastError("Save VM memory"); resume(); rollback(); return false; }
        virDomainSnapshotFree(point);
        QFile::setPermissions(directory + "/memory.save", QFile::ReadOwner | QFile::WriteOwner);
        int actual = 0, why = 0;
        if (virDomainGetState(domain, &actual, &why, 0) < 0 || actual != VIR_DOMAIN_PAUSED || runtimeToken(uuid) != epoch) { error = "The VM changed state while saving memory. The snapshot was not published."; resume(); rollback(); return false; }
        manifest["memory"] = "memory.save"; manifest["memoryState"] = state;
        manifest["consistency"] = "full-system";
    }
    if (!info["nvram"].toString().isEmpty()) {
        QDomDocument def; def.setContent(xml);
        if (def.documentElement().firstChildElement("os").firstChildElement("nvram").attribute("format", "raw") != "raw") {
            error = "Live UEFI checkpoints currently require a raw firmware variable store."; resume(); rollback(); return false;
        }
        if (!QFile::copy(info["nvram"].toString(), directory + "/firmware-vars")) { error = "Could not capture UEFI variables."; resume(); rollback(); return false; }
        QFile::setPermissions(directory + "/firmware-vars", QFile::ReadOwner | QFile::WriteOwner);
        manifest["nvram"] = "firmware-vars";
    }
    if (!captureTpm(uuid, xml, directory, manifest, error)) { resume(); rollback(); return false; }
    if (cancel || DomainConfig::revision(xmlOf(domain, VIR_DOMAIN_XML_INACTIVE)) != DomainConfig::revision(xml) || !Configuration::changes(xml, xmlOf(domain, 0)).isEmpty()) {
        error = cancel ? "Checkpoint cancelled." : "VM configuration changed while preparing the checkpoint. Try again."; resume(); rollback(); return false;
    }
    const auto checkpointXml = checkpoint.toString(-1).toUtf8();
    const auto started = virDomainBackupBegin(domain, backup.toString(-1).toUtf8().constData(), tracked ? checkpointXml.constData() : nullptr, incremental ? VIR_DOMAIN_BACKUP_BEGIN_REUSE_EXTERNAL : 0);
    if (started < 0) { error = lastError("Start live disk checkpoint"); resume(); rollback(); return false; }
    bool paired = true;
    if (memory) {
        int actual = 0, why = 0;
        paired = virDomainGetState(domain, &actual, &why, 0) == 0 && actual == VIR_DOMAIN_PAUSED && runtimeToken(uuid) == epoch;
        if (!paired) error = "The VM changed state before its memory and disks were paired. This snapshot was cancelled.";
    }
    const bool resumed = (memory && holdForRestore ? true : resume()) && thaw();
    progress("Copying live disks", 0, capacity);
    QElapsedTimer timer, abortTimer; timer.start(); bool abortSent = false, failed = !resumed || !paired;
    while (timer.elapsed() < 1800000) {
        if ((cancel || failed) && !abortSent) {
            abortSent = true;
            abortTimer.start();
            if (ownBackup(domain, directory)) virDomainAbortJob(domain);
            progress("Cancelling checkpoint", 0, 0);
        }
        failure.clear(); auto job = jobStats(domain, 0, failure);
        if (!failure.isEmpty()) {
            error += " " + failure;
            if (virDomainIsActive(domain) == 0) rollback();
            else {
                if (ownBackup(domain, directory)) virDomainAbortJob(domain);
                error += " Cancellation was requested; incomplete checkpoint files were retained at " + directory + ".";
            }
            return false;
        }
        if (job["type"].toInt() == VIR_DOMAIN_JOB_NONE) {
            auto completed = jobStats(domain, VIR_DOMAIN_JOB_STATS_COMPLETED | VIR_DOMAIN_JOB_STATS_KEEP_COMPLETED, failure);
            if (cancel || failed || completed["type"].toInt() != VIR_DOMAIN_JOB_COMPLETED || completed[VIR_DOMAIN_JOB_OPERATION].toInt() != VIR_DOMAIN_JOB_OPERATION_BACKUP || !completed[VIR_DOMAIN_JOB_SUCCESS].toBool()) {
                if (error.isEmpty()) error = cancel ? "Checkpoint cancelled. The VM's disks were not changed." : "Live checkpoint failed: " + completed.value(VIR_DOMAIN_JOB_ERRMSG, "completion could not be verified").toString();
                rollback(); return false;
            }
            for (auto v : disks) {
                const auto path = directory + "/" + v.toMap()["file"].toString();
                if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) { error = "A completed checkpoint disk is missing."; rollback(); return false; }
                QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
                QByteArray output;
                if (!process({"info", "--output=json", "-f", "qcow2", path}, &cancel, {}, error, &output)) { rollback(); return false; }
                const auto image = QJsonDocument::fromJson(output).toVariant().toMap(); QString expected;
                if (incremental) for (auto d : base["disks"].toList()) if (d.toMap()["target"] == v.toMap()["target"]) expected = root + "/" + base["id"].toString() + "/" + d.toMap()["file"].toString();
                if (image["format"] != "qcow2" || image["full-backing-filename"].toString() != expected) { error = "Completed backup has an unexpected disk dependency; it was not published."; rollback(); return false; }
            }
            if (!finishManifest(directory, manifest, options, error)) { rollback(); return false; }
            published = true;
            // The restore transaction now owns this pause. Its journal keeps the
            // original disks paired with recovery RAM until the final switch.
            if (memory && holdForRestore) pausedByUs = false;
            progress("Checkpoint complete", capacity, capacity);
            return true;
        }
        if (job[VIR_DOMAIN_JOB_OPERATION].toInt() != VIR_DOMAIN_JOB_OPERATION_BACKUP) { error = "Another VM job replaced the checkpoint job. Incomplete files retained at " + directory; return false; }
        if (abortSent && abortTimer.elapsed() > 10000) { error += " Cancellation is still pending. Incomplete files were retained at " + directory + "."; return false; }
        if (!abortSent) progress("Copying live disks", job.value(VIR_DOMAIN_JOB_DISK_PROCESSED, 0).toULongLong(), job.value(VIR_DOMAIN_JOB_DISK_TOTAL, capacity).toULongLong());
        QThread::msleep(100);
    }
    if (ownBackup(domain, directory)) virDomainAbortJob(domain);
    error = "The live checkpoint exceeded thirty minutes and cancellation was requested. Incomplete files were retained at " + directory;
    return false;
}
QVariantMap Checkpoints::history(QString uuid) {
    return SnapshotHistory::view(list(uuid), read(rootPath(uuid) + "/current.json"));
}
bool Checkpoints::restore(virConnectPtr connection, virDomainPtr domain, QString xml, QString id, QString &error, bool allowRestart, Progress progress, const std::atomic_bool *cancel, bool safety) {
    const auto uuid = uuidOf(domain), root = rootPath(uuid), directory = root + "/" + id;
    if (root.isEmpty() || QUuid(id).isNull() || QFileInfo(directory).isSymLink()) { error = "Invalid checkpoint identity."; return false; }
    auto manifest = read(directory + "/manifest.json");
    if (!validManifest(manifest, uuid, id)) { error = "The checkpoint does not match this VM."; return false; }
    const bool memory = !manifest["memory"].toString().isEmpty();
    if (SnapshotHistory::restored(list(uuid), read(root + "/current.json"), id, error).isEmpty()) return false;
    if (!chain(uuid, id, error) || !diskChain(uuid, id, cancel, error)) return false;
    if (!manifest["verificationError"].toString().isEmpty()) { error = manifest["verificationError"].toString(); return false; }
    int initialState = 0, reason = 0;
    if (virDomainGetState(domain, &initialState, &reason, 0) < 0) { error = lastError("Read VM state before restoration"); return false; }
    const bool restart = initialState == VIR_DOMAIN_RUNNING || initialState == VIR_DOMAIN_PAUSED;
    if (restart && !allowRestart) { error = memory ? "Confirm Restore state to replace the current VM with its saved memory and disks." : "Confirm Restore & restart: this disk-only snapshot has no saved memory."; return false; }
    if (!restart && initialState != VIR_DOMAIN_SHUTOFF) { error = "Wait until the VM is running, paused or stopped before restoring."; return false; }
    auto currentId = [&] {
        auto current = virDomainLookupByUUIDString(connection, uuid.toUtf8().constData());
        const auto id = current ? virDomainGetID(current) : unsigned(-1);
        if (current) virDomainFree(current);
        return id;
    };
    const auto runtimeId = currentId();
    bool safetyHeld = false;
    auto releaseSafetyPause = qScopeGuard([&] {
        if (!safetyHeld) return;
        int state = 0, reason = 0;
        if (currentId() == runtimeId && virDomainGetState(domain, &state, &reason, 0) == 0 && state == VIR_DOMAIN_PAUSED) {
            if (reason != VIR_DOMAIN_PAUSED_USER || virDomainResume(domain) < 0) return;
        }
        QFile::remove(root + "/freeze.json");
    });
    auto ready = [&] {
        int state = 0, why = 0;
        if (virDomainGetState(domain, &state, &why, 0) < 0 || state != (safetyHeld ? VIR_DOMAIN_PAUSED : initialState) || currentId() != runtimeId || DomainConfig::revision(xmlOf(domain, VIR_DOMAIN_XML_INACTIVE)) != DomainConfig::revision(xml)) {
            error = "The VM state or configuration changed during restore preparation. Try again."; return false;
        }
        if (restart) {
            QString failure; auto job = jobStats(domain, 0, failure);
            if (!failure.isEmpty() || job["type"].toInt() != VIR_DOMAIN_JOB_NONE) { error = failure.isEmpty() ? "Another VM job is active. Wait for it to finish before restoring." : failure; return false; }
            if (!Configuration::changes(xml, xmlOf(domain, 0)).isEmpty()) { error = "Apply or discard pending hardware settings before restoring."; return false; }
        }
        return true;
    };
    if (!ready()) return false;
    if (QFile::exists(root + "/restore.json")) { error = "Recover the interrupted restore from Checkpoint storage first."; return false; }
    auto report = [&](QString phase) { if (progress) progress(phase, 0, 0); };
    report(restart ? "Preparing restored disks; the current VM stays on" : "Preparing restored disks");
    auto destination = vmRoot(uuid) + "/restore-" + QUuid::createUuid().toString(QUuid::Id128);
    if (!QDir().mkpath(destination)) { error = "Could not create a new restore directory."; return false; }
    QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    if (!write(destination + "/building.json", {{"uuid", uuid}, {"pid", QCoreApplication::applicationPid()}}, error)) { QDir(destination).removeRecursively(); return false; }
    auto buildingGuard = qScopeGuard([&] { QFile::remove(destination + "/building.json"); });
    auto rollback = [&] { QDir(destination).removeRecursively(); };
    // Restored disks are thin overlays on the snapshot's immutable images, so only headroom for new guest writes is needed.
    const qint64 headroom = std::min<qint64>(manifest["capacity"].toLongLong(), qint64(2) * 1024 * 1024 * 1024);
    if (QStorageInfo(destination).bytesAvailable() < headroom) { rollback(); error = "Not enough free space for the restored VM's new writes. Free at least " + QString::number(headroom / 1048576) + " MiB."; return false; }
    QDomDocument doc; if (!doc.setContent(manifest["xml"].toString())) { rollback(); error = "Invalid checkpoint configuration."; return false; }
    if (doc.documentElement().firstChildElement("uuid").text() != uuid || !doc.documentElement().firstChildElement("name").text().startsWith("omaware-")) { rollback(); error = "Checkpoint configuration belongs to a different VM."; return false; }
    auto devices = doc.documentElement().firstChildElement("devices");
    QSet<QString> expectedTargets, recordedTargets, files;
    for (auto disk = devices.firstChildElement("disk"); !disk.isNull(); disk = disk.nextSiblingElement("disk"))
        if (disk.attribute("device") == "disk" && disk.firstChildElement("readonly").isNull()) expectedTargets.insert(disk.firstChildElement("target").attribute("dev"));
    for (auto disk : manifest["disks"].toList()) {
        const auto target = disk.toMap()["target"].toString(), file = disk.toMap()["file"].toString();
        if (target.isEmpty() || file.isEmpty() || QFileInfo(file).fileName() != file || recordedTargets.contains(target) || files.contains(file)) { rollback(); error = "The checkpoint disk list is invalid."; return false; }
        recordedTargets.insert(target); files.insert(file);
    }
    if (expectedTargets.isEmpty() || expectedTargets != recordedTargets || doc.documentElement().firstChildElement("os").firstChildElement("nvram").text().isEmpty() != manifest["nvram"].toString().isEmpty()) {
        rollback(); error = "The checkpoint is missing a disk or firmware record. The VM was not changed."; return false;
    }
    for (auto v : manifest["disks"].toList()) {
        auto record = v.toMap(); auto file = record["file"].toString();
        // A thin overlay makes restoration instant regardless of disk size. The snapshot image stays
        // read-only underneath; snapshot deletion already retains files that a live backing chain uses.
        if (QFileInfo(file).fileName() != file || QFileInfo(directory + "/" + file).isSymLink() || !QFileInfo(directory + "/" + file).isFile()) { rollback(); error = "A checkpoint disk image is missing."; return false; }
        if (!process({"create", "-q", "-f", "qcow2", "-b", directory + "/" + file, "-F", "qcow2", destination + "/" + file}, cancel, {}, error)) { rollback(); return false; }
        QFile::setPermissions(destination + "/" + file, QFile::ReadOwner | QFile::WriteOwner);
        bool found = false;
        for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk")) if (d.firstChildElement("target").attribute("dev") == record["target"].toString()) {
            auto source = d.firstChildElement("source"); source.setAttribute("file", destination + "/" + file); d.firstChildElement("driver").setAttribute("type", "qcow2");
            d.removeChild(d.firstChildElement("backingStore")); found = true;
        }
        if (!found) { rollback(); error = "A checkpoint disk could not be matched to its VM configuration."; return false; }
    }
    if (!manifest["nvram"].toString().isEmpty()) {
        const auto file = manifest["nvram"].toString();
        if (QFileInfo(file).fileName() != file || QFileInfo(directory + "/" + file).isSymLink() || !QFile::copy(directory + "/" + file, destination + "/firmware-vars")) { rollback(); error = "Could not restore the UEFI variable store."; return false; }
        auto nvram = doc.documentElement().firstChildElement("os").firstChildElement("nvram");
        while (!nvram.firstChild().isNull()) nvram.removeChild(nvram.firstChild());
        nvram.appendChild(doc.createTextNode(destination + "/firmware-vars"));
    }
    auto memoryXml = memory ? memoryRestoreXml(connection, directory + "/" + manifest["memory"].toString(), doc.toString(-1), error) : QString{};
    if (memory && memoryXml.isEmpty()) { rollback(); return false; }
    if (Containment::enabled(xml)) {
        // A contained VM stays contained, even when returning to a snapshot taken before containment.
        doc.setContent(Containment::withMarker(doc.toString(-1), true));
        if (memory) memoryXml = Containment::withMarker(memoryXml, true);
        auto verdict = Containment::check(doc.toString(-1));
        if (memory) { auto live = Containment::check(memoryXml); auto list = verdict["violations"].toStringList() + live["violations"].toStringList(); list.removeDuplicates(); verdict["violations"] = list; }
        if (!verdict["violations"].toStringList().isEmpty()) { rollback(); error = "This VM is contained, but the snapshot was saved with: " + Containment::summary(verdict) + " Remove containment deliberately to return to it."; return false; }
    }
    report("Checking restored files before switching the VM");
    if (!ready()) { rollback(); return false; }
    if (cancel && *cancel) { rollback(); error = "Checkpoint restore cancelled before changing the VM."; return false; }
    const auto beforeMarker = read(root + "/current.json"); QString safetyId;
    if (safety) {
        report("Saving a recovery checkpoint before restoration");
        const auto recoveryName = "Recovery " + QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HHmmss") + " " + QUuid::createUuid().toString(QUuid::Id128).left(4);
        std::atomic_bool neverCancel{false}; const auto &cancellation = cancel ? *cancel : neverCancel;
        QVariantMap options{{"safety", true}, {"pinned", true}, {"tags", "recovery"}, {"incremental", true}, {"memory", memory}};
        bool ok = restart ? createLive(domain, xml, recoveryName, "Automatically saved before restoring " + manifest["name"].toString(), cancellation, progress ? progress : Progress([](QString, qulonglong, qulonglong) {}), error, options, memory)
                          : create(domain, xml, recoveryName, "Automatically saved before restoration", error, options, cancel, progress);
        if (!ok) { rollback(); return false; }
        safetyHeld = memory && initialState == VIR_DOMAIN_RUNNING;
        safetyId = read(root + "/current.json")["id"].toString();
        if (!ready() || (cancel && *cancel)) { rollback(); if (error.isEmpty()) error = "Restore cancelled; its recovery checkpoint was retained."; return false; }
    }
    const auto afterMarker = SnapshotHistory::restored(list(uuid), read(root + "/current.json"), id, error);
    if (afterMarker.isEmpty()) { rollback(); return false; }
    QVariantMap journal{{"uuid", uuid}, {"id", id}, {"beforeXml", xml}, {"afterXml", doc.toString(-1)}, {"beforeState", initialState}, {"beforeMarker", beforeMarker}, {"afterMarker", afterMarker}, {"safetyId", safetyId}, {"destination", destination}, {"memory", memory}};
    if (memory && restart && !safetyId.isEmpty()) journal["memoryRollback"] = safetyId;
    if (!write(root + "/restore.json", journal, error)) { rollback(); return false; }
    // The potentially slow copies are complete before interrupting the guest.
    // Disk-only checkpoints need a fresh boot, not current RAM with restored disks.
    const unsigned startFlags = initialState == VIR_DOMAIN_PAUSED ? VIR_DOMAIN_START_PAUSED : 0;
    auto recover = [&](QString expectedXml) {
        if (virDomainIsActive(domain) != 0 || DomainConfig::revision(xmlOf(domain, VIR_DOMAIN_XML_INACTIVE)) != DomainConfig::revision(expectedXml)) {
            error += " VM state changed again; automatic rollback was skipped. Previous files and prepared files were retained at " + destination + "."; return;
        }
        auto previous = virDomainDefineXMLFlags(connection, xml.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
        if (!previous) { error += " " + lastError("Restore previous VM configuration") + ". Previous files and prepared files were retained at " + destination + "."; return; }
        virDomainFree(previous);
        putBackTpm(uuid, journal["tpmBackup"].toString());
        rollback();
        QString markerError;
        if (write(root + "/current.json", beforeMarker, markerError)) QFile::remove(root + "/restore.json");
        else error += " " + markerError;
        const auto prior = read(root + "/" + safetyId + "/manifest.json");
        if (restart && !prior["memory"].toString().isEmpty()) {
            QString failure; const auto file = root + "/" + safetyId + "/" + prior["memory"].toString();
            const auto restoredMemory = memoryRestoreXml(connection, file, xml, failure);
            if (restoredMemory.isEmpty() || virDomainRestoreFlags(connection, file.toUtf8().constData(), restoredMemory.toUtf8().constData(), initialState == VIR_DOMAIN_PAUSED ? VIR_DOMAIN_SAVE_PAUSED : VIR_DOMAIN_SAVE_RUNNING) < 0) error += " Previous memory is retained in the recovery snapshot. " + (failure.isEmpty() ? lastError("Resume previous VM") : failure);
            else error += " The previous running state was recovered without rebooting.";
        } else if (restart && virDomainCreateWithFlags(domain, startFlags) < 0) error += " " + lastError("Restart previous VM") + ". Its previous disks and configuration are selected; try Start again.";
        else error += restart ? " The previous disks and configuration were restored and restarted." : " The previous disks and configuration were kept.";
    };
    report(memory ? "Restoring saved memory and devices" : restart ? "Restarting the VM from the checkpoint" : "Switching VM configuration");
    if (restart) {
        if (virDomainDestroy(domain) < 0) {
            error = lastError("Stop VM for checkpoint restoration");
            if (virDomainIsActive(domain) == 0) recover(xml); else { rollback(); QFile::remove(root + "/restore.json"); }
            return false;
        }
    }
    if (!unchanged(domain, xml)) { rollback(); error = "The VM started or changed before the checkpoint switch. Its configuration was kept; check its power state."; return false; }
    // The TPM's state goes back too (the current one is kept until the restore has finished).
    if (manifest.contains("tpm")) {
        journal["tpmBackup"] = destination + "/tpm-before";
        if (!write(root + "/restore.json", journal, error)) { recover(xml); return false; }
        const auto saved = manifest.value("tpm").toString();
        if ((!saved.isEmpty() && !safeFile(directory, saved)) || !installTpm(uuid, saved.isEmpty() ? QString() : directory + "/" + saved, journal["tpmBackup"].toString(), error)) { recover(xml); return false; }
    }
    auto saved = virDomainDefineXMLFlags(connection, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!saved) { error = lastError("Restore checkpoint configuration"); recover(xml); return false; }
    virDomainFree(saved);
    const auto restoredXml = xmlOf(domain, VIR_DOMAIN_XML_INACTIVE);
    journal["afterXml"] = restoredXml;
    if (!write(root + "/restore.json", journal, error)) { recover(restoredXml); return false; }
    if (memory) {
        const unsigned flags = manifest["memoryState"].toInt() == VIR_DOMAIN_PAUSED ? VIR_DOMAIN_SAVE_PAUSED : VIR_DOMAIN_SAVE_RUNNING;
        if (virDomainRestoreFlags(connection, (directory + "/" + manifest["memory"].toString()).toUtf8().constData(), memoryXml.toUtf8().constData(), flags) < 0) { error = lastError("Resume snapshot memory"); recover(restoredXml); return false; }
    } else if (restart && virDomainCreateWithFlags(domain, startFlags) < 0) { error = lastError("Start restored VM"); recover(restoredXml); return false; }
    if (!write(root + "/current.json", afterMarker, error)) { error = "The checkpoint was restored, but its current marker could not be saved. " + error; return false; }
    if (!safetyId.isEmpty() && !write(root + "/undo.json", {{"id", safetyId}, {"restoredId", id}}, error)) return false;
    if (!safety) QFile::remove(root + "/undo.json");
    QFile::remove(root + "/restore.json");
    QFile::remove(destination + "/tpm-before");
    return true;
}
namespace {
// Paths in a disk's backing chain, excluding the disk itself. Live XML lists the chain QEMU has open;
// a stopped disk is inspected directly.
QStringList backingChain(QDomElement disk, bool active, QString &error) {
    QStringList chain;
    if (active) {
        for (auto store = disk.firstChildElement("backingStore"); !store.isNull(); store = store.firstChildElement("backingStore")) {
            const auto file = store.firstChildElement("source").attribute("file");
            if (!file.isEmpty()) chain << QDir::cleanPath(file);
        }
        return chain;
    }
    QByteArray output;
    if (!process({"info", "--output=json", "--backing-chain", "-U", "-f", "qcow2", disk.firstChildElement("source").attribute("file")}, nullptr, {}, error, &output)) return {};
    const auto records = QJsonDocument::fromJson(output).toVariant().toList();
    for (int i = 1; i < records.size(); ++i) chain << QDir::cleanPath(records[i].toMap()["filename"].toString());
    return chain;
}
// Reverting makes a VM's disks thin overlays on a snapshot image. When that snapshot is deleted, merge
// the data it provides into the live disk (VMware-style consolidation) so its files become reclaimable.
bool consolidate(virConnectPtr connection, QString uuid, QString &error) {
    const auto root = rootPath(uuid);
    const auto deleted = read(root + "/current.json")["deletedSnapshots"].toMap();
    if (deleted.isEmpty()) return true;
    auto fromDeleted = [&](const QString &path) {
        for (auto it = deleted.cbegin(); it != deleted.cend(); ++it) if (path.startsWith(root + "/" + it.key() + "/")) return true;
        return false;
    };
    auto domain = virDomainLookupByUUIDString(connection, uuid.toUtf8().constData());
    if (!domain) { error = lastError("Find VM to consolidate"); return false; }
    auto release = qScopeGuard([&] { virDomainFree(domain); });
    const bool active = virDomainIsActive(domain) == 1;
    QDomDocument doc; if (!doc.setContent(xmlOf(domain, active ? 0 : VIR_DOMAIN_XML_INACTIVE))) { error = "Could not read the VM's disks to consolidate them."; return false; }
    for (auto disk = doc.documentElement().firstChildElement("devices").firstChildElement("disk"); !disk.isNull(); disk = disk.nextSiblingElement("disk")) {
        const auto target = disk.firstChildElement("target").attribute("dev"), source = disk.firstChildElement("source").attribute("file");
        if (disk.attribute("device") != "disk" || source.isEmpty() || disk.firstChildElement("driver").attribute("type") != "qcow2") continue;
        QString failure; const auto chain = backingChain(disk, active, failure);
        if (!failure.isEmpty()) { error = failure; return false; }
        if (std::none_of(chain.cbegin(), chain.cend(), fromDeleted)) continue;
        if (!active) {
            // Safe-mode rebase onto no backing file copies the needed data, then drops the reference.
            if (!process({"rebase", "-q", "-f", "qcow2", "-b", "", source}, nullptr, {}, error)) return false;
            continue;
        }
        if (virDomainBlockPull(domain, target.toUtf8().constData(), 0, 0) < 0) { error = lastError("Consolidate disk " + target); return false; }
        for (;;) {
            virDomainBlockJobInfo job{};
            const int status = virDomainGetBlockJobInfo(domain, target.toUtf8().constData(), &job, 0);
            if (status < 0) { error = lastError("Read consolidation progress for " + target); return false; }
            if (status == 0) break; // The pull finished and libvirt pivoted the chain.
            QThread::msleep(100);
        }
    }
    return true;
}
}

bool Checkpoints::remove(virConnectPtr connection, QString uuid, QString id, QString &error, bool descendants, bool allowPinned, QStringList expectedIds) {
    const auto root = rootPath(uuid), directory = root + "/" + id;
    if (root.isEmpty() || QUuid(id).isNull() || QFileInfo(directory).isSymLink()) { error = "Invalid checkpoint identity."; return false; }
    auto manifest = read(directory + "/manifest.json");
    if (!validManifest(manifest, uuid, id)) { error = "Checkpoint identity does not match this VM."; return false; }
    if (QFile::exists(root + "/restore.json") || QFile::exists(root + "/freeze.json")) { error = "Recover interrupted snapshot work before deleting snapshots."; return false; }
    auto marker = read(root + "/current.json");
    if (marker["deletedSnapshots"].toMap().contains(id)) { error = "This snapshot has already been deleted."; return false; }
    const auto items = list(uuid); QSet<QString> deleting{id};
    if (descendants) {
        bool added;
        do {
            added = false;
            for (const auto &v : items) {
                const auto row = v.toMap(); const auto child = row["id"].toString();
                if (!deleting.contains(child) && deleting.contains(row["parentId"].toString())) { deleting.insert(child); added = true; }
            }
        } while (added);
        if (deleting != QSet<QString>(expectedIds.cbegin(), expectedIds.cend())) { error = "This branch changed. Refresh and review its snapshots before deleting it."; return false; }
    }
    for (const auto &v : items) {
        const auto row = v.toMap();
        if (!deleting.contains(row["id"].toString())) continue;
        if (row["pinned"].toBool() && !allowPinned) { error = "Confirm removal of pinned snapshots before deleting this selection."; return false; }
        SnapshotHistory::removed(marker, row);
    }
    marker["id"] = SnapshotHistory::survivingParent(marker, marker["id"].toString());
    marker["name"] = read(root + "/" + marker["id"].toString() + "/manifest.json")["name"];
    if (!write(root + "/current.json", marker, error)) return false;
    if (deleting.contains(read(root + "/undo.json")["id"].toString())) QFile::remove(root + "/undo.json");
    // The deletion is already durable; a failed merge only leaves the files retained and listed in Storage.
    if (!consolidate(connection, uuid, error)) { error = "The snapshot was deleted, but the current disk still uses its files and could not be consolidated: " + error; return false; }
    // Reclaim only files proven unused. Dependent incremental images remain
    // immutable; their deleted bases are visible in Storage until no longer used.
    // The atomic marker is already durable, so interrupted cleanup is retryable.
    bool reclaimed;
    do {
        reclaimed = false;
        for (const auto &value : storage(connection, uuid)["candidates"].toList()) {
            const auto row = value.toMap();
            if (!row["key"].toString().startsWith("deleted:") || !row["available"].toBool()) continue;
            QString ignored;
            if (cleanup(connection, uuid, row["key"].toString(), ignored)) reclaimed = true;
        }
    } while (reclaimed);
    return true;
}

bool Checkpoints::edit(QString uuid, QString id, QVariantMap values, QString &error) {
    const auto root = rootPath(uuid), directory = root + "/" + id;
    if (read(root + "/current.json")["deletedSnapshots"].toMap().contains(id)) { error = "This snapshot has been deleted."; return false; }
    auto manifest = read(directory + "/manifest.json");
    if (root.isEmpty() || QFileInfo(directory).isSymLink() || !validManifest(manifest, uuid, id)) { error = "Invalid checkpoint identity."; return false; }
    const auto name = values.value("name", manifest["name"]).toString().trimmed();
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9 _.-]{0,63}$").match(name).hasMatch()) { error = "Use 1–64 letters, numbers, spaces, dots or dashes for the name."; return false; }
    for (auto v : list(uuid)) if (v.toMap()["id"] != id && v.toMap()["name"] == name) { error = "A checkpoint with this name already exists."; return false; }
    manifest["name"] = name; manifest["notes"] = values.value("notes", manifest["notes"]).toString().left(4096); values.remove("safety"); fields(manifest, values, directory);
    if (!write(directory + "/manifest.json", manifest, error)) return false;
    auto current = read(root + "/current.json");
    if (current["id"] == id) { current["name"] = name; return write(root + "/current.json", current, error); }
    return true;
}

bool Checkpoints::verify(QString uuid, QString id, const std::atomic_bool &cancel, Progress progress, QString &error) {
    if (read(rootPath(uuid) + "/current.json")["deletedSnapshots"].toMap().contains(id)) { error = "This snapshot has been deleted."; return false; }
    QSet<QString> ancestry;
    if (!chain(uuid, id, error, &ancestry, true) || !diskChain(uuid, id, &cancel, error)) return false;
    for (auto point : ancestry) {
        const auto directory = rootPath(uuid) + "/" + point; auto manifest = read(directory + "/manifest.json");
        QVariantMap hashes, fingerprints; QStringList files;
        for (auto v : manifest["disks"].toList()) {
            const auto file = v.toMap()["file"].toString(); files << file;
            if (progress) progress("Checking checkpoint disk structure", 0, 0);
            if (!process({"check", "-f", "qcow2", "--output=json", directory + "/" + file}, &cancel, {}, error)) { if (!cancel) { manifest["verificationError"] = error; QString ignored; write(directory + "/manifest.json", manifest, ignored); } return false; }
        }
        if (!manifest["nvram"].toString().isEmpty()) files << manifest["nvram"].toString();
        if (!manifest.value("tpm").toString().isEmpty()) files << manifest.value("tpm").toString();
        if (!manifest["memory"].toString().isEmpty()) files << manifest["memory"].toString();
        for (auto name : files) {
            QFile file(directory + "/" + name); if (!file.open(QIODevice::ReadOnly)) { error = "Could not read checkpoint data."; return false; }
            QCryptographicHash hash(QCryptographicHash::Sha256); qulonglong done = 0;
            while (!file.atEnd()) {
                if (cancel) { error = "Checkpoint verification cancelled."; return false; }
                auto bytes = file.read(1024 * 1024); if (bytes.isEmpty() && file.error() != QFile::NoError) { error = "Checkpoint read failed."; return false; }
                hash.addData(bytes); done += bytes.size(); if (progress) progress("Verifying checkpoint contents", done, file.size());
            }
            const auto digest = QString::fromLatin1(hash.result().toHex()); auto old = manifest["hashes"].toMap().value(name).toString();
            if (!old.isEmpty() && digest != old) { error = "Checkpoint contents changed since their last verification: " + name; manifest["verificationError"] = error; write(directory + "/manifest.json", manifest, error); return false; }
            hashes[name] = digest; QFileInfo info(file); fingerprints[name] = QVariantMap{{"size", info.size()}, {"modified", info.lastModified().toMSecsSinceEpoch()}};
        }
        manifest["hashes"] = hashes; manifest["fingerprints"] = fingerprints; manifest["verifiedAt"] = QDateTime::currentSecsSinceEpoch(); manifest.remove("verificationError");
        if (!write(directory + "/manifest.json", manifest, error)) return false;
    }
    return true;
}

namespace {
void xmlReferences(QString xml, QSet<QString> &paths) {
    QDomDocument doc; if (!doc.setContent(xml)) return;
    std::function<void(QDomElement)> visit = [&](QDomElement e) {
        for (auto key : {"file", "dir", "dev", "path"}) {
            auto path = e.attribute(key); if (QFileInfo(path).isAbsolute()) paths.insert(QDir::cleanPath(path));
        }
        if (e.tagName() == "nvram" && QFileInfo(e.text()).isAbsolute()) paths.insert(QDir::cleanPath(e.text()));
        for (auto child = e.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) visit(child);
    }; visit(doc.documentElement());
}
// For a disk unreadable by this user, libvirt may expose its pool metadata.
// Use it only if its recorded modification time still matches the actual file.
bool volumeReferences(virConnectPtr connection, QString path, QSet<QString> &paths, QSet<QString> &seen) {
    if (!QFileInfo(path).isAbsolute() || seen.contains(path) || seen.size() > 32) return false;
    seen.insert(path); paths.insert(QDir::cleanPath(path));
    auto volume = virStorageVolLookupByPath(connection, path.toUtf8().constData()); if (!volume) return false;
    char *raw = virStorageVolGetXMLDesc(volume, 0); virStorageVolFree(volume); if (!raw) return false;
    QDomDocument doc; const bool valid = bool(doc.setContent(QString::fromUtf8(raw))); free(raw); if (!valid) return false;
    auto target = doc.documentElement().firstChildElement("target");
    const auto stamp = target.firstChildElement("timestamps").firstChildElement("mtime").text();
    const auto seconds = stamp.section('.', 0, 0).toLongLong(), millis = stamp.section('.', 1, 1).leftJustified(3, '0').left(3).toLongLong();
    QFileInfo info(path);
    if (!info.exists() || stamp.isEmpty() || info.lastModified().toMSecsSinceEpoch() != seconds * 1000 + millis || info.size() != doc.documentElement().firstChildElement("physical").text().toLongLong()) return false;
    auto backing = doc.documentElement().firstChildElement("backingStore").firstChildElement("path").text();
    return backing.isEmpty() || volumeReferences(connection, backing, paths, seen);
}
bool references(virConnectPtr connection, QSet<QString> &paths, QString &error) {
    QSet<QString> activePaths, checkedImages;
    auto images = [&](virConnectPtr c, QString xml) {
        QDomDocument doc; if (!doc.setContent(xml)) return true;
        bool ok = true;
        for (auto disk = doc.documentElement().firstChildElement("devices").firstChildElement("disk"); !disk.isNull(); disk = disk.nextSiblingElement("disk")) {
            const auto source = disk.firstChildElement("source").attribute("file");
            if (source.isEmpty() || disk.firstChildElement("driver").attribute("type") != "qcow2" || activePaths.contains(source) || checkedImages.contains(source)) continue;
            QByteArray output; QString failure;
            if (!process({"info", "--output=json", "--backing-chain", "-f", "qcow2", source}, nullptr, {}, failure, &output)) {
                QSet<QString> seen; if (!volumeReferences(c, source, paths, seen)) ok = false;
            } else {
                auto records = QJsonDocument::fromJson(output).toVariant().toList(); if (records.isEmpty()) ok = false;
                for (auto value : records) {
                    const auto path = value.toMap()["filename"].toString();
                    if (QFileInfo(path).isAbsolute()) paths.insert(QDir::cleanPath(path)); else ok = false;
                }
            }
            checkedImages.insert(source);
        }
        return ok;
    };
    auto collect = [&](virConnectPtr c) {
        virDomainPtr *domains = nullptr; int n = virConnectListAllDomains(c, &domains, 0); if (n < 0) return false;
        bool ok = true; QStringList savedXml;
        for (int i = 0; i < n; ++i) {
            const bool active = virDomainIsActive(domains[i]) == 1;
            for (unsigned flags : {unsigned(0), unsigned(VIR_DOMAIN_XML_INACTIVE)}) {
                char *raw = virDomainGetXMLDesc(domains[i], flags);
                if (!raw) ok = false;
                else { const auto xml = QString::fromUtf8(raw); xmlReferences(xml, paths); if (active && flags == 0) xmlReferences(xml, activePaths); if (flags) savedXml.append(xml); }
                free(raw);
            }
            if (active) { virDomainJobInfo job{}; if (virDomainGetJobInfo(domains[i], &job) < 0 || job.type != VIR_DOMAIN_JOB_NONE) ok = false; }
            virDomainFree(domains[i]);
        }
        free(domains);
        for (const auto &xml : savedXml) if (!images(c, xml)) ok = false;
        return ok;
    };
    if (!collect(connection)) { error = "A VM job is active or disk dependencies could not be checked. Finish jobs or reconcile unavailable storage before cleanup."; return false; }
    auto system = virConnectOpenReadOnly("qemu:///system");
    if (!system) { error = "Host VM references could not be checked; cleanup is unavailable."; return false; }
    bool ok = collect(system); virConnectClose(system);
    if (!ok) { error = "Host disk dependencies could not be checked; cleanup is unavailable."; return false; }
    auto app = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    for (auto file : QDir(app + "/pending").entryList({"*.json"}, QDir::Files)) {
        auto record = read(app + "/pending/" + file);
        for (auto it = record.cbegin(); it != record.cend(); ++it) { const auto xml = it.value().toString(); xmlReferences(xml, paths); if (!images(connection, xml)) ok = false; }
    }
    QDirIterator records(app + "/checkpoints", {"restore.json"}, QDir::Files, QDirIterator::Subdirectories);
    while (records.hasNext()) {
        auto record = read(records.next());
        for (auto key : {"beforeXml", "afterXml"}) { const auto xml = record[key].toString(); xmlReferences(xml, paths); if (!images(connection, xml)) ok = false; }
        for (auto key : {"id", "safetyId"}) if (!QUuid(record[key].toString()).isNull() && !QUuid(record["uuid"].toString()).isNull()) paths.insert(rootPath(record["uuid"].toString()) + "/" + record[key].toString() + "/manifest.json");
    }
    QDirIterator checkpoints(app + "/checkpoints", {"manifest.json", "building.json"}, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (checkpoints.hasNext()) {
        const auto path = checkpoints.next(); auto record = read(path);
        const auto directory = QFileInfo(path).dir();
        const auto expectedId = directory.dirName(), expectedUuid = QFileInfo(directory.absolutePath()).dir().dirName();
        const auto uuid = record["uuid"].toString(), base = record["baseId"].toString();
        if (QUuid(expectedUuid).isNull() || QUuid(expectedId).isNull() || uuid != expectedUuid || record["id"] != expectedId ||
            (QFileInfo(path).fileName() == "manifest.json" && !validManifest(record, expectedUuid, expectedId)) || (!base.isEmpty() && QUuid(base).isNull())) {
            error = "Snapshot metadata could not be checked. Recover unreadable records before reclaiming files."; return false;
        }
        if (!QUuid(uuid).isNull() && !QUuid(base).isNull()) paths.insert(rootPath(uuid) + "/" + base + "/manifest.json");
    }
    if (!ok) error = "Pending or recovery disk dependencies could not be checked; cleanup is unavailable.";
    return ok;
}

bool used(QString directory, const QSet<QString> &paths) {
    for (const auto &path : paths) if (path == directory || path.startsWith(directory + "/")) return true;
    return false;
}
}

QVariantMap Checkpoints::storage(virConnectPtr connection, QString uuid) {
    const auto root = rootPath(uuid); if (root.isEmpty()) return {};
    QSet<QString> paths; QString failure; const bool checked = references(connection, paths, failure);
    QVariantList candidates; qulonglong total = 0, retained = 0, reclaimable = 0;
    for (auto row : list(uuid)) total += row.toMap()["bytes"].toULongLong();
    auto candidate = [&](QString directory, QString key, QString label) {
        auto bytes = directoryBytes(directory); bool available = checked && !QFileInfo(directory).isSymLink() && !used(directory, paths);
        const auto pid = read(directory + "/building.json")["pid"].toLongLong();
        if (pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM)) available = false;
        QDirIterator links(directory, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (links.hasNext()) if (QFileInfo(links.next()).isSymLink()) available = false;
        retained += bytes; if (available) reclaimable += bytes;
        candidates.append(QVariantMap{{"key", key}, {"name", label}, {"bytes", bytes}, {"available", available}, {"reason", available ? "Unused by VM definitions and pending changes" : failure.isEmpty() ? "In use or retained for recovery" : failure}});
    };
    for (auto dir : QDir(vmRoot(uuid)).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QRegularExpression("^restore-[a-f0-9]{32}$").match(dir).hasMatch()) candidate(vmRoot(uuid) + "/" + dir, "restore:" + dir, "Retained restore · " + dir.right(8));
    for (const auto &allVms : {Paths::legacyVms(), Paths::vms()})
        for (auto dir : QDir(allVms).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (!QUuid(dir).isNull() && read(allVms + "/" + dir + "/building.json")["sourceUuid"] == uuid) candidate(allVms + "/" + dir, "clone:" + dir, "Interrupted clone · " + dir.left(8));
    const auto deleted = read(root + "/current.json")["deletedSnapshots"].toMap();
    for (auto dir : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QUuid(dir).isNull()) continue;
        if (deleted.contains(dir)) candidate(root + "/" + dir, "deleted:" + dir, "Deleted snapshot · " + deleted[dir].toMap()["name"].toString());
        else if (QFile::exists(root + "/" + dir + "/building.json") && !QFile::exists(root + "/" + dir + "/manifest.json")) candidate(root + "/" + dir, "incomplete:" + dir, "Interrupted checkpoint · " + dir.left(8));
    }
    return {{"checkpointBytes", total}, {"retainedBytes", retained}, {"reclaimableBytes", reclaimable}, {"candidates", candidates}, {"cleanupBlocker", failure}, {"undoId", undoId(uuid)}, {"restoreRecovery", QFile::exists(root + "/restore.json")}, {"guestFrozen", QFile::exists(root + "/freeze.json")}};
}

bool Checkpoints::cleanup(virConnectPtr connection, QString uuid, QString key, QString &error) {
    const auto root = rootPath(uuid); if (root.isEmpty()) { error = "Invalid VM identity."; return false; }
    if (key.startsWith("checkpoint:")) return remove(connection, uuid, key.mid(11), error);
    QSet<QString> paths; if (!references(connection, paths, error)) return false;
    for (auto value : storage(connection, uuid)["candidates"].toList()) {
        const auto row = value.toMap(); if (row["key"] != key) continue;
        if (!row["available"].toBool()) { error = row["reason"].toString(); return false; }
        const auto dir = key.startsWith("restore:") ? vmRoot(uuid) + "/" + key.mid(8) : key.startsWith("clone:") ? vmRoot(key.mid(6)) : root + "/" + key.mid(key.startsWith("deleted:") ? 8 : 11);
        const auto bitmap = key.startsWith("deleted:") ? read(dir + "/manifest.json")["bitmap"].toString() : QString{};
        if (!QDir(dir).removeRecursively()) { error = "Could not remove all retained files."; return false; }
        if (!bitmap.isEmpty()) { auto d = virDomainLookupByUUIDString(connection, uuid.toUtf8().constData()); if (d) { forgetBitmap(d, bitmap); virDomainFree(d); } }
        return true;
    }
    error = "The selected storage item is no longer eligible for cleanup."; return false;
}
QString Checkpoints::undoId(QString uuid) { const auto id = read(rootPath(uuid) + "/undo.json")["id"].toString(); return !read(rootPath(uuid) + "/current.json")["deletedSnapshots"].toMap().contains(id) && validManifest(read(rootPath(uuid) + "/" + id + "/manifest.json"), uuid, id) ? id : QString{}; }

bool Checkpoints::recover(virConnectPtr connection, virDomainPtr domain, QString &error) {
    const auto uuid = uuidOf(domain), root = rootPath(uuid);
    if (QFile::exists(root + "/freeze.json")) {
        auto capture = read(root + "/freeze.json");
        if (capture["uuid"] != uuid) { error = "Guest recovery record does not match this VM."; return false; }
        if (virDomainIsActive(domain) == 1 && capture["runtimeId"].toUInt() == virDomainGetID(domain) && (capture["runtimeToken"].toString().isEmpty() || capture["runtimeToken"] == runtimeToken(uuid))) {
            int state = 0, reason = 0;
            if (virDomainGetState(domain, &state, &reason, 0) < 0) { error = lastError("Read interrupted capture state"); return false; }
            if (capture["paused"].toBool() && state == VIR_DOMAIN_PAUSED) {
                if (reason != VIR_DOMAIN_PAUSED_USER || virDomainResume(domain) < 0) { error = "The guest is paused for another reason. Resolve its power state before recovery."; return false; }
            }
            if (capture["frozen"].toBool() && virDomainFSThaw(domain, nullptr, 0, 0) < 0) { error = lastError("Recover guest writes"); return false; }
        }
        QFile::remove(root + "/freeze.json");
    }
    auto record = read(root + "/restore.json"); if (record.isEmpty()) return true;
    if (record["uuid"] != uuid) { error = "Restore recovery record does not match this VM."; return false; }
    const auto actual = xmlOf(domain, VIR_DOMAIN_XML_INACTIVE), before = record["beforeXml"].toString(), after = record["afterXml"].toString();
    const bool matchesBefore = DomainConfig::revision(actual) == DomainConfig::revision(before);
    const bool matchesAfter = !after.isEmpty() && Configuration::changes(after, actual).isEmpty();
    if (!matchesBefore && !matchesAfter) { error = "VM configuration changed after the interruption. Recovery files were retained for review."; return false; }
    if (virDomainIsActive(domain) == 1) {
        if (record["memory"].toBool()) {
            QString failure; const auto job = jobStats(domain, 0, failure);
            if (!failure.isEmpty() || job["type"].toInt() != VIR_DOMAIN_JOB_NONE) { error = "Memory restoration is still in progress. Wait before recovering it."; return false; }
        }
        if (matchesAfter && !matchesBefore) {
            auto marker = record["afterMarker"].toMap();
            if (marker.isEmpty()) marker = SnapshotHistory::restored(list(uuid), read(root + "/current.json"), record["id"].toString(), error);
            if (marker.isEmpty() || !write(root + "/current.json", marker, error)) return false;
            if (!record["safetyId"].toString().isEmpty()) { if (!write(root + "/undo.json", {{"id", record["safetyId"]}, {"restoredId", record["id"]}}, error)) return false; } else QFile::remove(root + "/undo.json");
        }
    } else {
        auto previous = virDomainDefineXMLFlags(connection, before.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
        if (!previous) { error = lastError("Recover previous configuration"); return false; }
        putBackTpm(uuid, record["tpmBackup"].toString());
        int state = record["beforeState"].toInt();
        bool ok = true;
        if (!record["memoryRollback"].toString().isEmpty()) {
            const auto id = record["memoryRollback"].toString();
            auto memory = read(root + "/" + id + "/manifest.json");
            const auto file = root + "/" + id + "/" + memory["memory"].toString();
            QString adjusted;
            if (chain(uuid, id, error) && !memory["memory"].toString().isEmpty()) adjusted = memoryRestoreXml(connection, file, before, error);
            ok = !adjusted.isEmpty() && virDomainRestoreFlags(connection, file.toUtf8().constData(), adjusted.toUtf8().constData(), state == VIR_DOMAIN_PAUSED ? VIR_DOMAIN_SAVE_PAUSED : VIR_DOMAIN_SAVE_RUNNING) == 0;
        } else ok = (state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED) || virDomainCreateWithFlags(previous, state == VIR_DOMAIN_PAUSED ? VIR_DOMAIN_START_PAUSED : 0) == 0;
        virDomainFree(previous);
        if (!ok) { error = lastError("Restart previous VM after interruption"); return false; }
        if (!write(root + "/current.json", record["beforeMarker"].toMap(), error)) return false;
    }
    return QFile::remove(root + "/restore.json");
}

QString Checkpoints::clone(virConnectPtr connection, QString uuid, QString id, QString name, const std::atomic_bool &cancel, Progress progress, QString &error) {
    if (read(rootPath(uuid) + "/current.json")["deletedSnapshots"].toMap().contains(id)) { error = "This snapshot has been deleted."; return {}; }
    name = name.trimmed();
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]{0,47}$").match(name).hasMatch()) { error = "Use 1–48 letters, numbers, dots or dashes for the new VM name."; return {}; }
    name = "omaware-" + name; auto existing = virDomainLookupByName(connection, name.toUtf8().constData());
    if (existing) { virDomainFree(existing); error = "A VM with that name already exists."; return {}; }
    if (!chain(uuid, id, error) || !diskChain(uuid, id, &cancel, error)) return {};
    const auto source = rootPath(uuid) + "/" + id, newUuid = QUuid::createUuid().toString(QUuid::WithoutBraces), destination = vmRoot(newUuid);
    auto manifest = read(source + "/manifest.json");
    if (!manifest["verificationError"].toString().isEmpty()) { error = manifest["verificationError"].toString(); return {}; }
    if (!QDir().mkpath(destination)) { error = "Could not create clone storage."; return {}; }
    QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto rollback = qScopeGuard([&] { QDir(destination).removeRecursively(); });
    if (!write(destination + "/building.json", {{"sourceUuid", uuid}, {"uuid", newUuid}, {"id", id}, {"pid", QCoreApplication::applicationPid()}}, error)) return {};
    if (QStorageInfo(destination).bytesAvailable() < manifest["capacity"].toLongLong()) { error = "Not enough free space for an independent clone."; return {}; }
    QDomDocument doc; if (!doc.setContent(manifest["xml"].toString())) { error = "Invalid checkpoint configuration."; return {}; }
    auto root = doc.documentElement(); if (root.firstChildElement("uuid").text() != uuid) { error = "Checkpoint identity mismatch."; return {}; }
    auto text = [&](QDomElement e, QString value) { while (!e.firstChild().isNull()) e.removeChild(e.firstChild()); e.appendChild(doc.createTextNode(value)); };
    root.removeAttribute("id"); text(root.firstChildElement("uuid"), newUuid); text(root.firstChildElement("name"), name);
    if (!root.firstChildElement("genid").isNull()) text(root.firstChildElement("genid"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    root.removeChild(root.firstChildElement("title"));
    auto devices = root.firstChildElement("devices"); int copied = 0, expected = 0;
    for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk")) {
        if (d.attribute("device") != "disk" || !d.firstChildElement("readonly").isNull()) continue;
        ++expected;
        for (auto value : manifest["disks"].toList()) if (value.toMap()["target"] == d.firstChildElement("target").attribute("dev")) {
            const auto file = value.toMap()["file"].toString();
            if (!safeFile(source, file) || !copyDisk(source + "/" + file, "qcow2", destination + "/" + file, error, &cancel, progress)) return {};
            d.firstChildElement("source").setAttribute("file", destination + "/" + file); d.firstChildElement("driver").setAttribute("type", "qcow2"); d.removeChild(d.firstChildElement("backingStore")); ++copied;
        }
    }
    if (!expected || expected != copied) { error = "Incomplete checkpoint disk list."; return {}; }
    if (!manifest["nvram"].toString().isEmpty()) {
        if (!QFile::copy(source + "/" + manifest["nvram"].toString(), destination + "/firmware-vars")) { error = "Could not copy firmware variables."; return {}; }
        text(root.firstChildElement("os").firstChildElement("nvram"), destination + "/firmware-vars");
    }
    // The new VM gets its own copy of the TPM, so whatever Windows stored in it (BitLocker, sign-in keys) still works.
    if (!manifest.value("tpm").toString().isEmpty()) {
        QDir().mkpath(QFileInfo(tpmState(newUuid)).absolutePath());
        if (!safeFile(source, manifest.value("tpm").toString()) || !QFile::copy(source + "/" + manifest.value("tpm").toString(), tpmState(newUuid))) { error = "Could not copy the TPM state."; return {}; }
    }
    for (auto nic = devices.firstChildElement("interface"); !nic.isNull(); nic = nic.nextSiblingElement("interface")) {
        nic.firstChildElement("mac").setAttribute("address", "52:54:00:" + QString::fromLatin1(QUuid::createUuid().toRfc4122().right(3).toHex(':')));
        auto link = nic.firstChildElement("link"); if (link.isNull()) { link = doc.createElement("link"); nic.appendChild(link); } link.setAttribute("state", "down");
        nic.removeChild(nic.firstChildElement("target"));
    }
    for (auto c = devices.firstChildElement("channel"); !c.isNull(); c = c.nextSiblingElement("channel")) if (c.attribute("type") == "unix") c.removeChild(c.firstChildElement("source"));
    auto entries = root.firstChildElement("sysinfo").elementsByTagName("entry"); for (int i = 0; i < entries.size(); ++i) if (entries.at(i).toElement().attribute("name") == "uuid") text(entries.at(i).toElement(), newUuid);
    if (cancel) { error = "Clone cancelled."; return {}; }
    // A clone of a contained VM (possibly holding untrusted software) is contained too; it starts only once compliant.
    if (auto original = virDomainLookupByUUIDString(connection, uuid.toUtf8().constData())) {
        if (Containment::enabled(xmlOf(original, VIR_DOMAIN_XML_INACTIVE))) doc.setContent(Containment::withMarker(doc.toString(-1), true));
        virDomainFree(original);
    }
    if (progress) progress("Registering the new VM", 0, 0);
    auto domain = virDomainDefineXMLFlags(connection, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!domain) { error = lastError("Create VM from checkpoint"); QDir(QFileInfo(tpmState(newUuid)).absolutePath() + "/..").removeRecursively(); return {}; }
    virDomainFree(domain); rollback.dismiss(); QFile::remove(destination + "/building.json"); return newUuid;
}
