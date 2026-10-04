// SPDX-License-Identifier: GPL-3.0-or-later
// Operations on one existing VM for VmWorker::manage(): details and readiness, power, deletion, the screen,
// input, serial console and guest agent (used by AI agents), addresses, containment, and configuration
// edits (hardware, disks, pending changes).
#include "management.h"
#include "checkpoints.h"
#include "configuration.h"
#include "containment.h"
#include "domainconfig.h"
#include "guestinput.h"
#include "networkcatalog.h"
#include "paths.h"
#include "vmfiles.h"
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <libvirt/libvirt-qemu.h>

using Management::child;

namespace {
// Sends an agent's mouse and keyboard actions to a running VM: pointer events through QEMU's monitor
// (input-send-event, passed through libvirt), keys through libvirt. Positions are in the coordinates of a
// screenshot of `width` × `height`.
class InputSender {
public:
    InputSender(virDomainPtr domain, double width, double height) : domain_(domain), width_(width), height_(height) {
        QDomDocument live;
        live.setContent(Virt::definition(domain, true));
        const auto devices = live.documentElement().firstChildElement("devices");
        for (auto e = devices.firstChildElement("input"); !e.isNull(); e = e.nextSiblingElement("input"))
            tablet_ |= e.attribute("type") == "tablet";
    }

    // Performs one action: {type: click, double_click, move, drag, scroll, type, key or wait, …}.
    bool perform(const QVariantMap &action, QString &why) {
        const auto type = action["type"].toString();
        if (type == "click" || type == "double_click" || type == "move" || type == "scroll")
            return pointer(type, action, why);
        if (type == "drag") return drag(action, why);
        if (type == "type") {
            const auto text = action["text"].toString();
            if (text.size() > 4000) {
                why = "Type at most 4000 characters at a time.";
                return false;
            }
            QList<GuestInput::Chord> chords;
            return GuestInput::chordsForText(text, chords, why) && press(chords, why);
        }
        if (type == "key") {
            GuestInput::Chord chord;
            return GuestInput::chordForKeys(action["keys"].toString(), chord, why) && press({chord}, why);
        }
        if (type == "wait") {
            QThread::msleep(ulong(std::clamp(action.value("seconds", 1.0).toDouble(), 0.0, 10.0) * 1000));
            return true;
        }
        why = "Unknown action “" + type + "”. Use click, double_click, move, drag, scroll, type, key or wait.";
        return false;
    }

private:
    bool send(const QJsonArray &events, QString &why) const {
        const auto command = QJsonDocument(
                QJsonObject{{"execute", "input-send-event"}, {"arguments", QJsonObject{{"events", events}}}})
                                     .toJson(QJsonDocument::Compact);
        char *reply = nullptr;
        const bool ok = virDomainQemuMonitorCommand(domain_, command.constData(), &reply, 0) == 0 && reply &&
                        !QByteArray(reply).contains("\"error\"");
        if (!ok) why = reply ? QString::fromUtf8(reply).left(300) : Virt::lastError("Send pointer input");
        free(reply);
        return ok;
    }

    // Events that move the tablet pointer to (x, y), or none (with `why`) when that isn't possible.
    QJsonArray at(double x, double y, QString &why) const {
        if (!tablet_) {
            why = "This VM has no tablet pointer, so OmaWare can't click at a position. Use keys instead.";
            return {};
        }
        if (width_ < 2 || height_ < 2 || x < 0 || y < 0 || x > width_ || y > height_) {
            why = "The position is outside the screen. Take a screenshot and use its coordinates.";
            return {};
        }
        auto axis = [](const char *name, double value, double size) {
            const int scaled = int(std::lround(value * 32767.0 / (size - 1)));
            return QJsonObject{{"type", "abs"}, {"data", QJsonObject{{"axis", name}, {"value", scaled}}}};
        };
        return {axis("x", std::min(x, width_ - 1), width_), axis("y", std::min(y, height_ - 1), height_)};
    }

    static QJsonObject button(const QString &name, bool down) {
        return {{"type", "btn"}, {"data", QJsonObject{{"down", down}, {"button", name}}}};
    }

    bool press(const QList<GuestInput::Chord> &chords, QString &why) const {
        for (const auto &chord : chords) {
            std::vector<unsigned> codes(chord.begin(), chord.end());
            if (virDomainSendKey(domain_, VIR_KEYCODE_SET_LINUX, 20, codes.data(), int(codes.size()), 0) < 0) {
                why = Virt::lastError("Type");
                return false;
            }
            // Lets the guest keep up with long text.
            QThread::msleep(25);
        }
        return true;
    }

    bool pointer(const QString &type, const QVariantMap &action, QString &why) const {
        const QString name = action.value("button", "left").toString();
        if (!QStringList{"left", "right", "middle"}.contains(name)) {
            why = "button must be left, right or middle";
            return false;
        }
        const auto events = at(action["x"].toDouble(), action["y"].toDouble(), why);
        if (events.isEmpty()) return false;
        if (type == "move") return send(events, why);
        if (type == "click" || type == "double_click") {
            for (int i = 0; i < (type == "double_click" ? 2 : 1); ++i) {
                auto down = events;
                down.append(button(name, true));
                if (!send(down, why) || !send({button(name, false)}, why)) break;
                QThread::msleep(60);
            }
            return why.isEmpty();
        }
        // Scrolling: wheel clicks at the position, downwards for a positive amount.
        const int amount = std::clamp(action.value("amount", 3).toInt(), -30, 30);
        if (!send(events, why)) return false;
        for (int i = 0; i < std::abs(amount) && why.isEmpty(); ++i) {
            const QString wheel = amount > 0 ? "wheel-down" : "wheel-up";
            if (send({button(wheel, true)}, why)) send({button(wheel, false)}, why);
        }
        return why.isEmpty();
    }

    bool drag(const QVariantMap &action, QString &why) const {
        const double x = action["x"].toDouble(), y = action["y"].toDouble();
        const double toX = action["to_x"].toDouble(), toY = action["to_y"].toDouble();
        auto from = at(x, y, why);
        const auto to = from.isEmpty() ? QJsonArray{} : at(toX, toY, why);
        if (to.isEmpty()) return false;
        from.append(button("left", true));
        if (!send(from, why)) return false;
        // A few steps in between, so the guest sees a real drag.
        for (int i = 1; i <= 5 && why.isEmpty(); ++i) {
            const double t = i / 5.0;
            const auto step = at(x + (toX - x) * t, y + (toY - y) * t, why);
            if (!step.isEmpty()) {
                send(step, why);
                QThread::msleep(30);
            }
        }
        return why.isEmpty() && send({button("left", false)}, why);
    }

    virDomainPtr domain_;
    double width_, height_;
    bool tablet_ = false;
};

// Runs one QEMU guest agent command and parses its JSON reply.
bool agentCommand(virDomainPtr domain, const QJsonObject &command, int timeout, QJsonObject &result, QString &why) {
    char *reply = virDomainQemuAgentCommand(
            domain, QJsonDocument(command).toJson(QJsonDocument::Compact).constData(), timeout, 0);
    if (!reply) {
        why = Virt::lastError("Guest agent");
        return false;
    }
    result = QJsonDocument::fromJson(reply).object();
    free(reply);
    return true;
}

// The definition with a new qcow2 disk at `path` on the first free virtio target, or empty (with `error`).
QString withNewDisk(const QString &xml, const QString &path, QString &error) {
    QDomDocument doc;
    doc.setContent(xml);
    auto devices = doc.documentElement().firstChildElement("devices");
    QSet<QString> used;
    for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk"))
        used.insert(d.firstChildElement("target").attribute("dev"));
    QString target;
    for (char c = 'a'; c <= 'z' && target.isEmpty(); ++c)
        if (!used.contains("vd" + QString(QChar(c)))) target = "vd" + QString(QChar(c));
    if (target.isEmpty()) {
        error = "No free virtio disk target is available.";
        return {};
    }
    auto disk = child(doc, devices, "disk");
    disk.setAttribute("type", "file");
    disk.setAttribute("device", "disk");
    auto driver = child(doc, disk, "driver");
    driver.setAttribute("name", "qemu");
    driver.setAttribute("type", "qcow2");
    child(doc, disk, "source").setAttribute("file", path);
    auto targetElement = child(doc, disk, "target");
    targetElement.setAttribute("dev", target);
    targetElement.setAttribute("bus", "virtio");
    return doc.toString(-1);
}

// The definition without the disk on this target, or empty when it isn't attached.
QString withoutDisk(const QString &xml, const QString &target) {
    QDomDocument doc;
    doc.setContent(xml);
    auto devices = doc.documentElement().firstChildElement("devices");
    for (auto d = devices.firstChildElement("disk"); !d.isNull(); d = d.nextSiblingElement("disk"))
        if (d.firstChildElement("target").attribute("dev") == target) {
            devices.removeChild(d);
            return doc.toString(-1);
        }
    return {};
}

// Metadata checks of each disk's file, for agents' diagnostics. Never reads disk contents or host logs.
QVariantList diskDiagnostics(const QVariantList &disks) {
    QVariantList checks;
    for (const auto &value : disks) {
        const auto disk = value.toMap();
        const auto source = disk["source"].toString();
        QVariantMap check{{"target", disk["target"]}, {"device", disk["device"]}};
        if (source.isEmpty() || disk["type"] != "file") {
            check["checked"] = false;
            check["reason"] = source.isEmpty() ? "No media attached" : "Not a local file-backed disk";
        } else {
            QFileInfo file(source);
            check["checked"] = true;
            check["exists"] = file.exists();
            check["readable"] = file.isReadable();
            if (file.exists()) check["file_bytes"] = file.size();
            QStorageInfo storage(file.absolutePath());
            if (storage.isValid() && storage.isReady()) {
                check["filesystem_available_bytes"] = storage.bytesAvailable();
                check["filesystem_read_only"] = storage.isReadOnly();
            }
        }
        checks.append(check);
    }
    return checks;
}

// Whether agents may use this network choice: OmaWare's own networks and private user networking.
bool agentMayUse(const QVariantMap &choice) {
    return choice["id"] == "user" || choice["managed"].toBool();
}
}

// "vm.readiness": whether the VM runs, and (with probeAgent) whether its guest agent answers.
void VmWorker::vmReadiness(const Request &request, const Target &vm) {
    int state = 0, reason = 0;
    const bool running = virDomainGetState(vm.domain, &state, &reason, 0) == 0 && state == VIR_DOMAIN_RUNNING;
    bool agentReady = false;
    if (running && request.in["probeAgent"].toBool()) {
        char *raw = virDomainQemuAgentCommand(vm.domain, "{\"execute\":\"guest-ping\"}", 2, 0);
        if (raw) {
            agentReady = QJsonDocument::fromJson(raw).object().contains("return");
            free(raw);
        }
    }
    request.done(true, "Readiness observed.",
            {{"running", running}, {"active", vm.active}, {"paused", state == VIR_DOMAIN_PAUSED},
                    {"guest_agent", agentReady}});
}

// "vm.details": the VM's configuration as agents see it.
void VmWorker::vmDetails(const Request &request, const Target &vm) {
    QString failure;
    auto details = DomainConfig::describe(vm.xml, failure);
    if (!failure.isEmpty()) return request.fail(failure);
    const auto liveXml = vm.active ? Virt::definition(vm.domain, true) : QString{};
    const auto live = vm.active ? DomainConfig::describe(liveXml, failure) : details;
    PendingChanges pending(vm.uuid);
    // After a full restart the VM runs its saved settings; nothing is pending any more.
    if (vm.active && !pending.baseline().isEmpty() && Configuration::changes(liveXml, vm.xml).isEmpty())
        pending.clear();
    details["active"] = vm.active;
    details["liveInterfaces"] = live["interfaces"];
    details["agentConnected"] = vm.active && live["agentConnected"].toBool();
    details["changes"] = pending.items(vm.xml);
    details["pendingConflict"] = !pending.matches(vm.xml);
    QString status;
    QVariantList options;
    for (const auto &v : NetworkCatalog::discover(conn_, status))
        if (!request.fromAgent() || agentMayUse(v.toMap())) options.append(v);
    details["networkOptions"] = options;
    details["diskDiagnostics"] = diskDiagnostics(details["disks"].toList());
    request.done(true, "VM configuration loaded.", details);
}

// "adapter.save": adds, changes or removes a network adapter (see changeNetwork).
void VmWorker::saveAdapter(const Request &request, const Target &vm) {
    auto &in = request.in;
    if (request.fromAgent() && !in["remove"].toBool()) {
        QString status;
        bool allowed = false;
        for (const auto &v : NetworkCatalog::discover(conn_, status))
            if (v.toMap()["id"] == in["networkId"] && agentMayUse(v.toMap())) allowed = true;
        if (!allowed)
            return request.fail("Agents may attach only to OmaWare-managed networks or private user networking.");
    }
    QString message;
    const bool ok = changeNetwork(vm.uuid, in["mac"].toString(), in["networkId"].toString(), in["model"].toString(),
            in["linkUp"].toBool(), in["remove"].toBool(), in["revision"].toString(), message);
    // The new revision and what still waits for a restart, so callers don't have to list again.
    const auto saved = Virt::definition(vm.domain);
    request.done(ok, message,
            {{"revision", DomainConfig::revision(saved)},
                    {"pending_change_count", PendingChanges(vm.uuid).items(saved).size()},
                    {"active", virDomainIsActive(vm.domain) == 1}});
}

// "vm.delete": removes the VM with its disks, snapshots, firmware variables and TPM state, keeping any file
// another VM still uses.
void VmWorker::deleteVm(const Request &request, const Target &vm) {
    // Deleting is never something an agent may do, and only OmaWare's own VMs can be deleted.
    if (request.fromAgent()) return request.fail("AI agents can't delete VMs.");
    if (!owned(vm.domain)) return request.fail("OmaWare only deletes VMs it created; this one is read-only.");
    const auto app = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto checkpointRoot = app + "/checkpoints/" + vm.uuid;
    if (QUuid(vm.uuid).isNull()) return request.fail("Invalid VM identity.");
    if (QFile::exists(checkpointRoot + "/restore.json") || QFile::exists(checkpointRoot + "/freeze.json"))
        return request.fail("A snapshot restore or guest freeze for this VM hasn't finished. Recover it on the "
                            "Snapshots page before deleting the VM.");
    if (vm.active) {
        if (!request.in.value("powerOff").toBool()) return request.fail("Turn the VM off before deleting it.");
        emit progress("Powering the VM off…");
        if (virDomainDestroy(vm.domain) < 0) return request.fail(Virt::lastError("Power off"));
    }
    const auto name = QString::fromUtf8(virDomainGetName(vm.domain));
    // Restores keep the disks and firmware variables they replace, so every definition the VM has had counts.
    QStringList definitions{Virt::definition(vm.domain)};
    for (const auto &id : QDir(checkpointRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile manifest(checkpointRoot + "/" + id + "/manifest.json");
        if (manifest.size() < 8 * 1024 * 1024 && manifest.open(QIODevice::ReadOnly))
            definitions << QJsonDocument::fromJson(manifest.readAll()).object()["xml"].toString();
    }
    const auto pendingFile = app + "/pending/" + vm.uuid + ".json";
    QFile pendingChanges(pendingFile);
    if (pendingChanges.size() < 8 * 1024 * 1024 && pendingChanges.open(QIODevice::ReadOnly))
        definitions << QJsonDocument::fromJson(pendingChanges.readAll()).object()["xml"].toString();
    const auto libvirtConfig = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/libvirt/qemu";
    auto plan = VmFiles::plan(definitions, vm.uuid,
            {checkpointRoot, Paths::vms() + "/" + vm.uuid, Paths::legacyVms() + "/" + vm.uuid},
            libvirtConfig + "/nvram");

    emit progress("Deleting the VM…");
    // Firmware variables (UEFI), TPM state, saved state and libvirt's snapshot records go with the definition.
    const unsigned flags = VIR_DOMAIN_UNDEFINE_MANAGED_SAVE | VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA |
                           VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA | VIR_DOMAIN_UNDEFINE_NVRAM |
                           VIR_DOMAIN_UNDEFINE_TPM;
    if (virDomainUndefineFlags(vm.domain, flags) < 0) return request.fail(Virt::lastError("Delete VM"));
    QFile::remove(pendingFile);
    const auto tpm = libvirtConfig + "/swtpm/" + vm.uuid;
    if (QFileInfo(tpm).isDir() && !QFileInfo(tpm).isSymLink()) QDir(tpm).removeRecursively();
    // A disk is deleted only once no other VM uses it. If that can't be checked, everything stays. The VM's
    // own snapshots refer to each other; only what other VMs use keeps a file.
    QSet<QString> referenced;
    QString failure;
    if (!Checkpoints::references(conn_, referenced, failure, vm.uuid))
        return request.fail(name +
                                    " was removed, but its disks and snapshots were kept because OmaWare couldn't "
                                    "check whether another VM uses them (" +
                                    failure + ").",
                {{"uuid", vm.uuid}});
    VmFiles::exclude(plan, referenced);
    emit progress("Deleting its disks and snapshots…");
    QStringList failures;
    const auto freed = VmFiles::remove(plan, failures);
    auto message = QString("%1 was deleted with its disks and snapshots. %2 freed.")
                           .arg(name, QLocale().formattedDataSize(freed));
    if (!plan.kept.isEmpty())
        message += " Kept (not made for this VM, or used by another VM): " + plan.kept.join(", ") + ".";
    if (!failures.isEmpty()) message += " Couldn't delete: " + failures.join(", ") + ".";
    request.done(failures.isEmpty(), message, {{"uuid", vm.uuid}, {"freedBytes", double(freed)}, {"kept", plan.kept}});
}

// "vm.power": start, shutdown, force-off, pause, resume or remove (see power()).
void VmWorker::vmPower(const Request &request, const Target &vm) {
    static const QMap<QString, QString> results{{"start", "VM started."},
            {"shutdown", "Shutdown requested; the guest decides when it stops."}, {"force-off", "VM powered off."},
            {"pause", "VM paused."}, {"resume", "VM resumed."}, {"remove", "VM removed. Its disk files were kept."}};
    const auto action = request.in["action"].toString();
    if (!results.contains(action)) return request.fail("Unknown power action.");
    const auto failure = power(vm.domain, action);
    request.done(failure.isEmpty(), failure.isEmpty() ? results.value(action) : failure, {{"uuid", vm.uuid}});
}

// "vm.serial": sends text to the VM's first serial console and reads what comes back.
void VmWorker::serialConsole(const Request &request, const Target &vm) {
    auto &in = request.in;
    if (!vm.active) return request.fail("Start the VM first.");
    // SAFE opens the console only when libvirt can make sure nobody else (virsh console, another agent call)
    // has it at the same time.
    Stream stream(virStreamNew(conn_, VIR_STREAM_NONBLOCK), virStreamFree);
    if (!stream || virDomainOpenConsole(vm.domain, nullptr, stream.get(), VIR_DOMAIN_CONSOLE_SAFE) < 0)
        return request.fail(Virt::lastError("Open the serial console") +
                                    ". The VM needs a serial console, and nobody else may have it open (close virsh "
                                    "console).",
                {{"code", "console_unavailable"}});
    const auto closeStream = qScopeGuard([&] { virStreamAbort(stream.get()); });
    const auto send = in["send"].toString().replace("\r\n", "\r").replace('\n', '\r').toUtf8();
    const auto expected = in["waitFor"].toString().toUtf8();
    const qint64 limit = std::clamp(in.value("timeout", 10).toInt(), 1, 60) * 1000LL;
    const int maxBytes = std::clamp(in.value("maxBytes", 16384).toInt(), 1024, 65536);
    QElapsedTimer clock;
    clock.start();
    for (qsizetype sent = 0; sent < send.size();) {
        const int n = virStreamSend(
                stream.get(), send.constData() + sent, size_t(std::min<qsizetype>(send.size() - sent, 4096)));
        if (n == -2) {
            if (clock.elapsed() > limit) break;
            QThread::msleep(20);
            continue;
        }
        if (n < 0)
            return request.fail(Virt::lastError("Write to the serial console"), {{"code", "console_unavailable"}});
        sent += n;
    }
    // Read until the expected text shows up, or (without one) the guest has been quiet for a second.
    QByteArray output;
    bool matched = false, truncated = false, closed = false;
    qint64 lastData = clock.elapsed();
    char buffer[4096];
    while (clock.elapsed() < limit) {
        const int n = virStreamRecv(stream.get(), buffer, sizeof buffer);
        if (n > 0) {
            output.append(buffer, n);
            lastData = clock.elapsed();
            if (output.size() > maxBytes) {
                output = output.right(maxBytes);
                truncated = true;
            }
            if (!expected.isEmpty() && output.contains(expected)) {
                matched = true;
                break;
            }
            continue;
        }
        if (n == 0) {
            closed = true;
            break;
        }
        if (n == -1) return request.fail(Virt::lastError("Read the serial console"), {{"code", "console_unavailable"}});
        if (expected.isEmpty() && clock.elapsed() - lastData >= 1000) break;
        QThread::msleep(30);
    }
    // Terminal control sequences and carriage returns are noise in text an agent reads.
    auto text = QString::fromUtf8(output);
    text.remove(QRegularExpression("\\x1b(\\[[0-9;?]*[ -/]*[@-~]|\\][^\\x07]*\\x07|[()][0-9A-Za-z]|[=>78DEHMc])"))
            .remove('\r');
    request.done(true, "Serial console read.",
            {{"output", text}, {"bytes", output.size()}, {"matched", matched},
                    {"timedOut", !expected.isEmpty() && !matched && !closed}, {"truncated", truncated},
                    {"closed", closed}, {"elapsedMs", clock.elapsed()}});
}

// "vm.screenshot": the VM's screen as a PNG, scaled down to at most `maxWidth` pixels wide.
void VmWorker::screenshot(const Request &request, const Target &vm) {
    if (!vm.active) return request.fail("Start the VM to see its screen.");
    Stream stream(virStreamNew(conn_, 0), virStreamFree);
    char *mime = stream ? virDomainScreenshot(vm.domain, stream.get(), 0, 0) : nullptr;
    if (!mime) return request.fail(Virt::lastError("Capture the screen"));
    free(mime);
    QByteArray data;
    char chunk[65536];
    for (;;) {
        const int n = virStreamRecv(stream.get(), chunk, sizeof chunk);
        if (n > 0) data.append(chunk, n);
        if (n == 0) break;
        if (n < 0 || data.size() > 128 * 1024 * 1024) {
            virStreamAbort(stream.get());
            return request.fail(Virt::lastError("Read the screen"));
        }
    }
    virStreamFinish(stream.get());
    QImage image;
    if (!image.loadFromData(data)) return request.fail("The screen image couldn't be read.");
    const QSize screen = image.size();
    // Large screens are scaled down so they stay readable for an AI model; clicks use the scaled size.
    const int maxWidth = std::clamp(request.in.value("maxWidth", 1280).toInt(), 320, 3840);
    if (image.width() > maxWidth) image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    request.done(true, "Screen captured",
            {{"uuid", vm.uuid}, {"png", QString::fromLatin1(png.toBase64())}, {"width", image.width()},
                    {"height", image.height()}, {"screenWidth", screen.width()}, {"screenHeight", screen.height()}});
}

// "vm.input": up to fifty mouse and keyboard actions, in order, stopping at the first that fails.
void VmWorker::sendInput(const Request &request, const Target &vm) {
    auto &in = request.in;
    if (!vm.active) return request.fail("Start the VM before using its screen.");
    const auto actions = in["actions"].toList();
    if (actions.isEmpty() || actions.size() > 50) return request.fail("Send one to fifty actions.");
    InputSender input(vm.domain, in["width"].toDouble(), in["height"].toDouble());
    int completed = 0;
    QString why;
    for (const auto &action : actions) {
        if (!input.perform(action.toMap(), why)) break;
        ++completed;
    }
    if (!why.isEmpty())
        return request.fail(QString("Stopped at action %1 (%2): %3")
                                    .arg(completed + 1)
                                    .arg(actions.value(completed).toMap()["type"].toString(), why),
                {{"completed", completed}});
    request.done(true, QString("%1 action%2 sent.").arg(completed).arg(completed == 1 ? "" : "s"),
            {{"completed", completed}});
}

// "vm.agentExec": a command run through the QEMU guest agent (as root), which works without any network.
void VmWorker::guestExec(const Request &request, const Target &vm) {
    auto &in = request.in;
    if (!vm.active) return request.fail("Start the VM first.");
    QString ignored;
    const auto live = DomainConfig::describe(Virt::definition(vm.domain, true), ignored);
    if (!live["agentConnected"].toBool())
        return request.fail("The QEMU guest agent isn't running in this VM.", {{"noAgent", true}});
    QJsonObject reply;
    QString why;
    const bool windows = agentCommand(vm.domain, QJsonObject{{"execute", "guest-get-osinfo"}}, 5, reply, why) &&
                         reply["return"].toObject()["id"].toString() == "mswindows";
    const auto command = in["command"].toString();
    const auto program =
            windows ? QJsonObject{{"path", "powershell.exe"},
                              {"arg", QJsonArray{"-NoProfile", "-NonInteractive", "-Command", command}},
                              {"capture-output", true}}
                    : QJsonObject{{"path", "/bin/sh"}, {"arg", QJsonArray{"-c", command}}, {"capture-output", true}};
    if (!agentCommand(vm.domain, QJsonObject{{"execute", "guest-exec"}, {"arguments", program}}, 10, reply, why))
        return request.fail(why);
    const int pid = reply["return"].toObject()["pid"].toInt(-1);
    if (pid < 0) return request.fail("The guest agent didn't start the command.");
    QElapsedTimer clock;
    clock.start();
    const qint64 limit = std::clamp(in.value("timeout", 60).toInt(), 1, 900) * 1000LL;
    const QJsonObject statusQuery{{"execute", "guest-exec-status"}, {"arguments", QJsonObject{{"pid", pid}}}};
    while (true) {
        if (!agentCommand(vm.domain, statusQuery, 10, reply, why)) return request.fail(why);
        const auto status = reply["return"].toObject();
        if (status["exited"].toBool()) {
            auto decode = [&](const char *key) {
                const auto bytes = QByteArray::fromBase64(status[key].toString().toLatin1());
                return QString::fromUtf8(bytes.right(65536));
            };
            const int exitCode =
                    status["exitcode"].toInt(status.contains("signal") ? 128 + status["signal"].toInt() : -1);
            return request.done(true, "Command finished.",
                    {{"exitCode", exitCode}, {"stdout", decode("out-data")}, {"stderr", decode("err-data")},
                            {"via", "guest agent"},
                            {"truncated", status["out-truncated"].toBool() || status["err-truncated"].toBool()}});
        }
        if (clock.elapsed() > limit)
            return request.fail(QString("The command is still running after %1 seconds (process %2 in the guest).")
                                        .arg(limit / 1000)
                                        .arg(pid),
                    {{"running", true}});
        QThread::msleep(250);
    }
}

// "vm.addresses": IPv4 addresses for each adapter, from DHCP leases of host networks, the host's neighbour
// table and the guest agent.
void VmWorker::guestAddresses(const Request &request, const Target &vm) {
    QString ignored;
    const auto live = DomainConfig::describe(Virt::definition(vm.domain, true), ignored);
    QHash<QString, QStringList> found;
    QHash<QString, QString> sources; // per MAC, the first source that knew an address
    QString source = "dhcp_lease";
    auto add = [&](const QString &mac, const QString &ip) {
        auto &list = found[mac.toLower()];
        if (!list.contains(ip)) list << ip;
        if (!sources.contains(mac.toLower())) sources[mac.toLower()] = source;
    };
    if (vm.active) {
        Connection system(virConnectOpenReadOnly("qemu:///system"), virConnectClose);
        const auto leases = Management::dhcpLeases(system.get());
        for (auto it = leases.cbegin(); it != leases.cend(); ++it)
            for (const auto &ip : it.value())
                add(it.key(), ip);
        for (const unsigned from : {unsigned(VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP),
                     unsigned(VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT)}) {
            if (from == VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT && !live["agentConnected"].toBool()) continue;
            source = from == VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT ? "guest_agent" : "arp";
            virDomainInterfacePtr *ifaces = nullptr;
            const int n = virDomainInterfaceAddresses(vm.domain, &ifaces, from, 0);
            for (int j = 0; j < n; ++j) {
                for (unsigned k = 0; k < ifaces[j]->naddrs; ++k) {
                    const auto ip = QString::fromUtf8(ifaces[j]->addrs[k].addr);
                    if (ifaces[j]->hwaddr && ifaces[j]->addrs[k].type == VIR_IP_ADDR_TYPE_IPV4 &&
                            !ip.startsWith("127."))
                        add(QString::fromUtf8(ifaces[j]->hwaddr), ip);
                }
                virDomainInterfaceFree(ifaces[j]);
            }
            free(ifaces);
        }
    }
    QVariantList nics;
    for (const auto &v : live["interfaces"].toList()) {
        const auto nic = v.toMap();
        const auto mac = nic["mac"].toString().toLower();
        nics.append(QVariantMap{{"mac", nic["mac"]}, {"type", nic["type"]}, {"source", nic["source"]},
                {"linkUp", nic["linkUp"]}, {"ips", found.value(mac)},
                {"ipSource", found.value(mac).isEmpty() ? QString("unknown") : sources.value(mac)}});
    }
    request.done(true, "Addresses read.",
            {{"uuid", vm.uuid}, {"active", vm.active}, {"agent", live["agentConnected"]}, {"interfaces", nics}});
}

// "vm.restart": asks the guest to shut down, then starts the VM again with its saved settings. Never forces it
// off: after two minutes without a shutdown it gives up.
void VmWorker::restartVm(const Request &request, const Target &vm) {
    if (const auto blocked = Containment::blocker(vm.domain, false); !blocked.isEmpty()) return request.fail(blocked);
    if (!vm.active) {
        const int result = start(vm.domain);
        return request.done(
                result == 0, result == 0 ? "VM started with its saved settings." : Virt::lastError("Start VM"));
    }
    if (virDomainShutdown(vm.domain) < 0) return request.fail(Virt::lastError("Request guest shutdown"));
    restartUuid_ = vm.uuid;
    restartTicks_ = 0;
    if (!restartTimer_) {
        restartTimer_ = new QTimer(this);
        restartTimer_->setInterval(1000);
        connect(restartTimer_, &QTimer::timeout, this, [this] {
            Domain d(virDomainLookupByUUIDString(conn_, restartUuid_.toUtf8().constData()), virDomainFree);
            if (!d || ++restartTicks_ >= 120) {
                restartTimer_->stop();
                restartUuid_.clear();
                emit finished("The guest did not shut down in time. Check its console; no forced power-off was "
                              "performed.",
                        false);
                return;
            }
            if (virDomainIsActive(d.get()) != 0) return;
            restartTimer_->stop();
            restartUuid_.clear();
            const auto blocked = Containment::blocker(d.get(), false);
            const int result = owned(d.get()) && blocked.isEmpty() ? start(d.get()) : -1;
            refresh();
            emit finished(result == 0          ? "VM started with its saved settings."
                          : !blocked.isEmpty() ? blocked
                                               : Virt::lastError("Start VM after shutdown"),
                    result == 0);
        });
    }
    restartTimer_->start();
    emit progress("Waiting for the guest to shut down before starting it with saved settings…");
    emit managed(request.op, true, {{"waiting", true}, {"requestTag", request.in.value("requestTag")}});
}

// "containment.set": turns containment (see containment.h) on or off.
void VmWorker::setContainment(const Request &request, const Target &vm) {
    const bool on = request.in["enabled"].toBool();
    if (on) {
        auto verdict = Containment::check(vm.xml);
        if (vm.active) {
            const auto live = Containment::check(Virt::definition(vm.domain, true));
            auto list = verdict["violations"].toStringList() + live["violations"].toStringList();
            list.removeDuplicates();
            verdict["violations"] = list;
        }
        if (!verdict["violations"].toStringList().isEmpty())
            return request.fail("Fix these before containing this VM: " + Containment::summary(verdict));
        // Containment is enforced by OmaWare; libvirt autostart would start the VM without that check.
        virDomainSetAutostart(vm.domain, 0);
    }
    const unsigned flags = VIR_DOMAIN_AFFECT_CONFIG | (vm.active ? VIR_DOMAIN_AFFECT_LIVE : 0);
    if (virDomainSetMetadata(vm.domain, VIR_DOMAIN_METADATA_ELEMENT, on ? "<containment enabled='yes'/>" : nullptr,
                on ? "containment" : nullptr, Containment::ns, flags) < 0)
        return request.fail(Virt::lastError("Update containment"));
    PendingChanges changes(vm.uuid);
    QString ignored;
    if (!changes.baseline().isEmpty()) changes.saved(Virt::definition(vm.domain), ignored);
    request.done(true,
            on ? "VM contained: only verified isolated switches, no shared folders, clipboard or passthrough."
               : "Containment removed. This VM can now be given internet or host access.",
            {{"uuid", vm.uuid}});
}

// "hardware.save", "disk.add", "disk.detach", "pending.discard" and "pending.accept": changes to the saved
// definition. A running VM keeps its current settings until its next full start; PendingChanges lists what
// is waiting and can undo it.
void VmWorker::editConfiguration(const Request &request, const Target &vm) {
    auto &in = request.in;
    const auto &op = request.op;
    if (in["revision"].toString() != DomainConfig::revision(vm.xml))
        return request.fail("The VM configuration changed. Refresh Details and review your changes again.");
    PendingChanges pending(vm.uuid);
    QString failure, updated, newDisk;
    if (op == "hardware.save") {
        virNodeInfo host{};
        virNodeGetInfo(conn_, &host);
        if (in.value("cpus", 1).toUInt() > std::max(1u, host.cpus) ||
                in.value("memoryMiB", 256).toULongLong() > host.memory / 1024)
            return request.fail("The requested CPU or RAM exceeds this host's available capacity.");
        if (request.fromAgent() && in.contains("iso") && !in["iso"].toString().isEmpty()) {
            const QFileInfo iso(in["iso"].toString());
            if (iso.isSymLink() || !iso.isFile() ||
                    iso.canonicalPath() != QFileInfo(Paths::isos()).canonicalFilePath() ||
                    !iso.fileName().endsWith(".iso", Qt::CaseInsensitive))
                return request.fail("Agents can only attach regular ISO files from OmaWare's local library.");
        }
        updated = Configuration::hardware(vm.xml, in, failure);
    } else if (op == "pending.discard") {
        if (pending.baseline().isEmpty() || !pending.matches(vm.xml))
            return request.fail("The pending-change record no longer matches this VM. Refresh and reconcile its "
                                "configuration first.");
        if (in["key"] == "all")
            updated = pending.baseline();
        else
            updated = Configuration::revert(pending.baseline(), vm.xml, in["key"].toString(), failure);
    } else if (op == "pending.accept") {
        pending.clear();
        return request.done(true, "Current saved settings accepted. They will be used on the next full start.");
    } else if (op == "disk.add") {
        const int size = in["diskGiB"].toInt();
        if (size < 1 || size > 2048) return request.fail("Choose a disk size of 1–2048 GiB.");
        const auto directory = Paths::vmDir(vm.uuid);
        if (!QDir().mkpath(directory) || QStorageInfo(directory).bytesAvailable() < qint64(size) * 1073741824)
            return request.fail("There is not enough free space for the new disk.");
        const auto path = directory + "/disk-" + QUuid::createUuid().toString(QUuid::Id128) + ".qcow2";
        QByteArray output;
        if (!Management::run("qemu-img", {"create", "-f", "qcow2", path, QString::number(size) + "G"}, output, failure))
            return request.fail(failure);
        newDisk = path;
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
        updated = withNewDisk(vm.xml, path, failure);
    } else if (op == "disk.detach") {
        updated = withoutDisk(vm.xml, in["target"].toString());
        if (updated.isEmpty()) failure = "The selected disk is no longer attached.";
    } else {
        return request.fail("Unknown VM operation.");
    }

    // From here on, a failure also removes a disk created for this change.
    const auto refuse = [&](const QString &why) {
        if (!newDisk.isEmpty()) QFile::remove(newDisk);
        request.fail(why);
    };
    if (failure.isEmpty() && Containment::enabled(vm.xml)) {
        // Edits keep the policy marker and must stay within it.
        updated = Containment::withMarker(updated, true);
        const auto verdict = Containment::check(updated);
        if (!verdict["violations"].toStringList().isEmpty())
            failure = "This VM is contained: " + Containment::summary(verdict);
    }
    if (!failure.isEmpty()) return refuse(failure);
    if (op == "hardware.save" && in.value("dryRun").toBool())
        return request.done(true, "Validated without changing the VM.",
                {{"dry_run", true}, {"changes", Configuration::changes(vm.xml, updated)},
                        {"revision", DomainConfig::revision(vm.xml)}, {"requires_restart", vm.active}});
    if ((vm.active || !pending.baseline().isEmpty()) && !pending.prepare(vm.xml, failure)) return refuse(failure);
    Domain saved(
            virDomainDefineXMLFlags(conn_, updated.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE), virDomainFree);
    if (!saved) return refuse(Virt::lastError("Save VM configuration"));
    if (!pending.baseline().isEmpty()) {
        const auto canonical = Virt::definition(saved.get());
        if (!pending.saved(canonical, failure)) return request.fail("Settings saved, but " + failure);
        if (pending.items(canonical).isEmpty()) pending.clear();
    }
    request.done(true, vm.active ? "Settings saved for the next full shutdown and start." : "VM settings saved.",
            {{"revision", DomainConfig::revision(Virt::definition(saved.get()))}, {"requires_restart", vm.active}});
}
