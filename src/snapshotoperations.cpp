// SPDX-License-Identifier: GPL-3.0-or-later
// Snapshots for VmWorker::manage(): listing, creating, restoring, deleting, verifying and cloning them.
// The storage work itself is in checkpoints.cpp; this checks what may be done and reports progress.
// "Internal" snapshots are libvirt's own, made by earlier versions: they can still be restored or deleted.
#include "management.h"
#include "checkpoints.h"
#include "configuration.h"
#include "domainconfig.h"
#include <QDir>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

namespace {
// Why this VM can't have a snapshot taken (`live`: while running) or restored, or empty when it can.
QString snapshotBlocker(const QString &xml, bool live) {
    QString error;
    const auto info = DomainConfig::describe(xml, error);
    if (!error.isEmpty()) return error;
    bool disk = false;
    for (const auto &v : info["disks"].toList()) {
        const auto d = v.toMap();
        if (d["device"] != "disk" || d["readOnly"].toBool()) continue;
        disk = true;
        if (d["type"] != "file" || !QStringList{"qcow2", "raw"}.contains(d["format"].toString()))
            return "Every writable disk must be a local raw or qcow2 image for a checkpoint.";
    }
    if (!info["shares"].toList().isEmpty())
        return "Shared folders are not captured by checkpoints. Remove them before using this checkpoint workflow.";
    QDomDocument doc;
    doc.setContent(xml);
    const auto nvram = doc.documentElement().firstChildElement("os").firstChildElement("nvram");
    if (live && nvram.attribute("format", "raw") != "raw")
        return "Live UEFI checkpoints require a raw firmware variable store. This VM can be checkpointed while "
               "stopped.";
    if (!doc.elementsByTagName("hostdev").isEmpty() || !doc.elementsByTagName("shareable").isEmpty())
        return "Passthrough devices and shared writable disks are not supported by this checkpoint workflow.";
    const auto tpms = doc.elementsByTagName("tpm");
    for (int i = 0; i < tpms.size(); ++i)
        if (tpms.at(i).toElement().firstChildElement("backend").attribute("type") != "emulator")
            return "Only emulated TPMs can be captured in snapshots.";
    return disk ? QString{} : "Attach a local disk before creating a checkpoint.";
}

// Whether the VM was started again (it runs with a new domain ID) since it had `previousId`.
bool restartedSince(virConnectPtr conn, const QString &uuid, unsigned previousId) {
    Domain current(virDomainLookupByUUIDString(conn, uuid.toUtf8().constData()), virDomainFree);
    return current && virDomainIsActive(current.get()) == 1 && virDomainGetID(current.get()) != previousId;
}

// The libvirt snapshots of earlier versions, as list entries.
QVariantList internalSnapshots(virDomainPtr domain) {
    QVariantList rows;
    virDomainSnapshotPtr *snapshots = nullptr;
    const int count = virDomainListAllSnapshots(domain, &snapshots, 0);
    for (int i = 0; i < count; ++i) {
        char *raw = virDomainSnapshotGetXMLDesc(snapshots[i], 0);
        QDomDocument doc;
        doc.setContent(raw ? QString::fromUtf8(raw) : QString{});
        free(raw);
        const auto root = doc.documentElement();
        const auto parent = root.firstChildElement("parent").firstChildElement("name").text();
        rows.append(QVariantMap{{"kind", "internal"}, {"capture", "stopped"},
                {"name", root.firstChildElement("name").text()},
                {"notes", root.firstChildElement("description").text()},
                {"time", root.firstChildElement("creationTime").text().toLongLong()}, {"parent", parent},
                {"parentId", parent.isEmpty() ? QString{} : "internal:" + parent},
                {"state", root.firstChildElement("state").text()},
                {"current", virDomainSnapshotIsCurrent(snapshots[i], 0) == 1},
                {"leaf", virDomainSnapshotNumChildren(snapshots[i], 0) == 0}});
        virDomainSnapshotFree(snapshots[i]);
    }
    free(snapshots);
    return rows;
}

// "snapshots.create": a new independent snapshot, with the running VM's memory when in["memory"] asks for it.
void createSnapshot(const VmWorker::Request &request, const VmWorker::Target &vm, const std::atomic_bool &cancel,
        const Checkpoints::Progress &report) {
    auto &in = request.in;
    if (!vm.active && in["memory"].toBool())
        return request.fail("A stopped VM has no running memory to save. Start the VM or choose a disk-only snapshot.");
    const auto name = in["name"].toString().trimmed();
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9 _.-]{0,63}$").match(name).hasMatch())
        return request.fail("Choose a checkpoint name using 1–64 letters, numbers, spaces, dots or dashes.");
    if (Snapshot(virDomainSnapshotLookupByName(vm.domain, name.toUtf8().constData(), 0), virDomainSnapshotFree))
        return request.fail("A checkpoint with this name already exists.");
    for (const auto &v : Checkpoints::list(vm.uuid))
        if (v.toMap()["name"] == name) return request.fail("A checkpoint with this name already exists.");
    if (virDomainSnapshotNum(vm.domain, 0) != 0)
        return request.fail(
                "Remove existing internal snapshots before switching to independent disk-and-firmware checkpoints.");
    QString failure;
    const auto notes = in["notes"].toString().left(4096);
    bool ok;
    if (vm.active) {
        report("Preparing live checkpoint", 0, 0);
        ok = Checkpoints::createLive(vm.domain, vm.xml, name, notes, cancel, report, failure, in);
    } else {
        emit request.worker.progress("Copying stopped VM disks and firmware into an independent checkpoint…");
        ok = Checkpoints::create(vm.domain, vm.xml, name, notes, failure, in, &cancel, report);
    }
    if (!ok) return request.fail(failure);
    request.done(true, vm.active && in["memory"].toBool()
                               ? "Snapshot saved with memory, CPU/device state and disks. Restore resumes this "
                                 "captured state."
                       : vm.active ? "Disk-only snapshot created. Restoring it requires a fresh boot."
                                   : "Disk-only snapshot created from the stopped VM.");
}

// "snapshots.restore" of an independent snapshot (in["id"]): its disks, firmware and, if it saved them, its
// memory and devices.
void restoreSnapshot(const VmWorker::Request &request, const VmWorker::Target &vm, bool savedMemory,
        const std::atomic_bool &cancel, const Checkpoints::Progress &report) {
    auto &in = request.in;
    if (virDomainSnapshotNum(vm.domain, 0) != 0)
        return request.fail(
                "Remove existing internal snapshots before restoring independent disk-and-firmware copies.");
    const auto conn = virDomainGetConnect(vm.domain);
    const auto originalId = virDomainGetID(vm.domain);
    QString failure;
    const bool ok = Checkpoints::restore(conn, vm.domain, vm.xml, in["id"].toString(), failure,
            in["allowRestart"].toBool(), report, &cancel, in.value("safety", true).toBool());
    const QString message = !ok           ? failure
                            : savedMemory ? "Saved memory and disks restored without rebooting the guest OS."
                            : vm.active   ? "Disk-only snapshot restored. The guest was rebooted because this snapshot "
                                            "has no memory."
                                          : "Disk-only snapshot restored. The VM remains stopped.";
    request.done(ok, message,
            {{"uuid", vm.uuid}, {"restarted", restartedSince(conn, vm.uuid, originalId)},
                    {"restoredMemory", ok && savedMemory},
                    {"currentId", ok ? Checkpoints::history(vm.uuid)["currentId"] : QVariant{}}});
}

// "snapshots.restore" or "snapshots.remove" of an internal snapshot (in["name"]); only stopped-VM ones are
// supported.
void changeInternalSnapshot(const VmWorker::Request &request, const VmWorker::Target &vm, const QString &op) {
    auto &in = request.in;
    Snapshot snapshot(virDomainSnapshotLookupByName(vm.domain, in["name"].toString().toUtf8().constData(), 0),
            virDomainSnapshotFree);
    if (!snapshot) return request.fail(Virt::lastError("Find checkpoint"));
    char *raw = virDomainSnapshotGetXMLDesc(snapshot.get(), 0);
    QDomDocument doc;
    doc.setContent(raw ? QString::fromUtf8(raw) : QString{});
    free(raw);
    bool internal = doc.documentElement().firstChildElement("state").text() == "shutoff";
    for (auto d = doc.documentElement().firstChildElement("disks").firstChildElement("disk"); !d.isNull();
            d = d.nextSiblingElement("disk"))
        if (d.attribute("snapshot") == "external") internal = false;
    if (!internal)
        return request.fail("Only stopped-VM internal checkpoints can be restored or deleted in this workflow.");
    int result = -1;
    const auto originalId = virDomainGetID(vm.domain);
    if (op == "snapshots.restore") {
        int state = 0, reason = 0;
        if (virDomainGetState(vm.domain, &state, &reason, 0) < 0)
            return request.fail(Virt::lastError("Read VM state before restoration"));
        if (state != VIR_DOMAIN_SHUTOFF && state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED)
            return request.fail("Wait until the VM is running, paused or stopped before restoring.");
        unsigned flags = 0;
        if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
            if (!in["allowRestart"].toBool())
                return request.fail("Confirm Restore & restart before restoring an active VM.");
            virDomainJobInfo job{};
            if (virDomainGetJobInfo(vm.domain, &job) < 0 || job.type != VIR_DOMAIN_JOB_NONE)
                return request.fail("Another VM job is active or its status could not be read. Try again after it "
                                    "finishes.");
            flags = state == VIR_DOMAIN_PAUSED ? VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED : VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING;
        }
        emit request.worker.progress("Restoring the legacy checkpoint and returning the VM to its prior power state…");
        result = virDomainRevertToSnapshot(snapshot.get(), flags);
    } else {
        if (in["descendants"].toBool()) {
            // The whole branch goes, so it must still be the branch the user reviewed.
            QSet<QString> deleting{"internal:" + in["name"].toString()};
            virDomainSnapshotPtr *children = nullptr;
            const int count =
                    virDomainSnapshotListAllChildren(snapshot.get(), &children, VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS);
            if (count < 0) return request.fail(Virt::lastError("Read snapshot branch"));
            for (int i = 0; i < count; ++i) {
                deleting.insert("internal:" + QString::fromUtf8(virDomainSnapshotGetName(children[i])));
                virDomainSnapshotFree(children[i]);
            }
            free(children);
            const auto expected = in["expectedIds"].toStringList();
            if (deleting != QSet<QString>(expected.cbegin(), expected.cend()))
                return request.fail("This branch changed. Refresh and review its snapshots before deleting it.");
        }
        result = virDomainSnapshotDelete(
                snapshot.get(), in["descendants"].toBool() ? VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN : 0);
    }
    const auto failure = result == 0 ? QString{} : Virt::lastError("Update checkpoint");
    const bool restarted = vm.active && restartedSince(virDomainGetConnect(vm.domain), vm.uuid, originalId);
    request.done(result == 0, result == 0 ? "Checkpoint operation completed." : failure,
            {{"uuid", vm.uuid}, {"restarted", restarted}});
}
}

// "snapshots.list": the VM's snapshots, its storage use, and what currently prevents taking or restoring one.
void VmWorker::listSnapshots(const Request &request, const Target &vm) {
    auto rows = internalSnapshots(vm.domain);
    const bool hasInternal = !rows.isEmpty();
    const auto history = Checkpoints::history(vm.uuid);
    rows += history["items"].toList();
    PendingChanges pending(vm.uuid);
    const bool pendingSettings = !pending.items(vm.xml).isEmpty() || !pending.matches(vm.xml);
    QString createBlocker = snapshotBlocker(vm.xml, vm.active);
    if (createBlocker.isEmpty() && hasInternal)
        createBlocker = "This VM has legacy internal snapshots. Remove those before using independent checkpoints.";
    if (createBlocker.isEmpty() && pendingSettings)
        createBlocker = "Apply or discard pending hardware settings before creating a checkpoint.";
    auto restoreBlocker = snapshotBlocker(vm.xml, false);
    if (restoreBlocker.isEmpty() && pendingSettings)
        restoreBlocker = "Apply or discard pending hardware settings before restoring a checkpoint.";
    request.done(true, "Checkpoints loaded",
            {{"uuid", vm.uuid}, {"items", rows}, {"currentId", history["currentId"]},
                    {"storage", Checkpoints::storage(conn_, vm.uuid)}, {"blocker", createBlocker},
                    {"restoreBlocker", restoreBlocker}});
}

// Every other "snapshots.*" operation. One at a time per VM, guarded by a lock file in its checkpoint folder.
void VmWorker::manageSnapshots(const Request &request, const Target &vm) {
    static const QStringList operations{"snapshots.edit", "snapshots.verify", "snapshots.cleanup", "snapshots.recover",
            "snapshots.clone", "snapshots.undo", "snapshots.remove", "snapshots.create", "snapshots.restore"};
    if (!operations.contains(request.op)) return request.fail("Unknown snapshot operation.");
    auto &in = request.in;
    const auto checkpointRoot =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/checkpoints/" + vm.uuid;
    if (!QDir().mkpath(checkpointRoot)) return request.fail("Could not open checkpoint storage.");
    QLockFile lock(checkpointRoot + "/.operation.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return request.fail("Another OmaWare checkpoint operation is active for this VM.");

    // Progress for the Snapshots page. Undoing a restore reports as a restore while it runs.
    QString op = request.op;
    const auto report = [&](QString phase, qulonglong completed, qulonglong total) {
        static const QStringList uninterruptible{"Saving VM memory and device state",
                "Restoring saved memory and devices", "Restarting the VM from the checkpoint", "Registering the new VM",
                "Switching VM configuration", "Deleting snapshots and checking unused storage"};
        emit checkpointProgress({{"active", true}, {"uuid", vm.uuid}, {"operation", op}, {"phase", phase},
                {"completed", completed}, {"total", total}, {"cancellable", !uninterruptible.contains(phase)}});
        emit progress(phase + "…");
    };
    QString failure;
    if (op == "snapshots.edit") {
        const bool ok = Checkpoints::edit(vm.uuid, in["id"].toString(), in, failure);
        return request.done(ok, ok ? "Checkpoint details saved." : failure);
    }
    if (op == "snapshots.verify") {
        const bool ok = Checkpoints::verify(vm.uuid, in["id"].toString(), checkpointCancel_, report, failure);
        return request.done(ok, ok ? "Checkpoint and its disk dependencies verified." : failure);
    }
    if (op == "snapshots.cleanup") {
        const bool ok = Checkpoints::cleanup(conn_, vm.uuid, in["key"].toString(), failure);
        return request.done(ok, ok ? "Unused checkpoint storage removed." : failure);
    }
    if (op == "snapshots.recover") {
        const auto originalId = virDomainGetID(vm.domain);
        const bool ok = Checkpoints::recover(conn_, vm.domain, failure);
        return request.done(ok,
                ok ? "Interrupted checkpoint work reconciled. Refresh storage to review retained files." : failure,
                {{"uuid", vm.uuid}, {"restarted", restartedSince(conn_, vm.uuid, originalId)}});
    }
    if (op == "snapshots.clone") {
        const auto copy = Checkpoints::clone(
                conn_, vm.uuid, in["id"].toString(), in["name"].toString(), checkpointCancel_, report, failure);
        if (copy.isEmpty()) return request.fail(failure, {{"uuid", copy}});
        emit created(copy);
        return request.done(
                true, "VM created from checkpoint, stopped with its adapters disconnected.", {{"uuid", copy}});
    }
    if (op == "snapshots.undo") {
        // Undo goes back to the safety snapshot the last restore took, without taking another.
        in["id"] = Checkpoints::undoId(vm.uuid);
        in["safety"] = false;
        if (in["id"].toString().isEmpty()) return request.fail("There is no previous restore to undo.");
        op = "snapshots.restore";
    }
    if (op == "snapshots.remove" && !in["id"].toString().isEmpty()) {
        // Deletion publishes history changes first and reclaims only unused copies; the VM's current working
        // disks are never changed.
        report("Deleting snapshots and checking unused storage", 0, 0);
        const bool ok = Checkpoints::remove(conn_, vm.uuid, in["id"].toString(), failure, in["descendants"].toBool(),
                in["allowPinned"].toBool(), in["expectedIds"].toStringList());
        return request.done(ok,
                ok ? "Snapshot selection deleted. The current VM is unchanged; disk files still needed by other "
                     "snapshots are retained in Storage."
                   : failure,
                {{"uuid", vm.uuid}});
    }
    if (vm.active && op == "snapshots.remove")
        return request.fail("Shut down the VM before deleting a legacy internal snapshot.");
    QVariantMap selected;
    for (const auto &value : Checkpoints::list(vm.uuid))
        if (value.toMap()["id"] == in["id"]) selected = value.toMap();
    const bool savedMemory = !selected["memory"].toString().isEmpty();
    if (vm.active && op == "snapshots.restore" && !in["allowRestart"].toBool())
        return request.fail(savedMemory ? "Confirm Restore state to return to the snapshot's memory and disks."
                                        : "Confirm Restore & restart: this disk-only snapshot has no saved memory.");
    if (const auto blocker = snapshotBlocker(vm.xml, vm.active && op == "snapshots.create"); !blocker.isEmpty())
        return request.fail(blocker);
    PendingChanges pending(vm.uuid);
    if (!pending.items(vm.xml).isEmpty() || !pending.matches(vm.xml))
        return request.fail("Apply or discard pending settings before managing checkpoints.");
    if (op == "snapshots.create") return createSnapshot(request, vm, checkpointCancel_, report);
    if (!in["id"].toString().isEmpty()) return restoreSnapshot(request, vm, savedMemory, checkpointCancel_, report);
    changeInternalSnapshot(request, vm, op);
}
