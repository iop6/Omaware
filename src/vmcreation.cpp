// SPDX-License-Identifier: GPL-3.0-or-later
// Creating VMs for VmWorker::manage() ("capabilities", "vm.create"), and the first boots of a VM that
// installs itself unattended (see unattended.h).
#include "management.h"
#include "agentprovision.h"
#include "applianceimport.h"
#include "cloudimages.h"
#include "cloudseed.h"
#include "containment.h"
#include "domainconfig.h"
#include "labs.h"
#include "networkcatalog.h"
#include "paths.h"
#include "unattended.h"
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>
#include <archive.h>
#include <archive_entry.h>
#include <libvirt/virterror.h>
#include <unistd.h>

using Management::child;
using Management::run;

namespace {
// The files in a new VM's own folder.
const QString systemDisk = "/system.qcow2";
const QString cloudSeedDisc = "/seed.iso"; // cloud-init's first-boot setup, for cloud images
const QString answersDisc = "/setup.iso";  // the installer's answers, for unattended installs
const QString installerKernel = "/installer-vmlinuz", installerInitrd = "/installer-initrd"; // Ubuntu's

// A libosinfo OS id, as virt-install accepts it.
const QRegularExpression osinfoId("^[a-z][a-z0-9.+-]{1,40}$");

// OS presets offered when creating a VM, if this host's libosinfo knows them.
const QList<QPair<QString, QString>> &supportedPresets() {
    static const QList<QPair<QString, QString>> list{{"ubuntu26.04", "Ubuntu 26.04"}, {"ubuntu24.04", "Ubuntu 24.04"},
            {"ubuntu22.04", "Ubuntu 22.04"}, {"fedora44", "Fedora 44"}, {"fedora43", "Fedora 43"},
            {"fedora42", "Fedora 42"}, {"debian13", "Debian 13"}, {"debian12", "Debian 12"}, {"win11", "Windows 11"},
            {"win10", "Windows 10"}, {"generic", "Generic OS"}};
    return list;
}

// libvirt emulates a TPM with swtpm, which has to be installed on this computer.
bool tpmAvailable() {
    return !QStandardPaths::findExecutable("swtpm").isEmpty();
}

// Whether this host's libosinfo knows `preset`. A cloud image newer than the host's OS list still boots fine
// as a generic Linux, so for cloud images an unknown preset becomes "generic".
bool resolvePreset(QString &preset, bool cloud) {
    const auto &presets = supportedPresets();
    bool known = std::any_of(presets.begin(), presets.end(), [&](const auto &p) { return p.first == preset; });
    if (!known && osinfoId.match(preset).hasMatch()) {
        // Any other libosinfo id this host knows is fine too (checked, never passed through unvalidated).
        QByteArray list;
        QString ignored;
        if (run("virt-install", {"--osinfo", "list"}, list, ignored, 15000))
            known = QString::fromUtf8(list).split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts).contains(preset);
    }
    if (!known && cloud) {
        preset = "generic";
        known = true;
    }
    return known;
}

// Copies one file (e.g. "casper/vmlinuz") out of an ISO image.
bool extractFromIso(const QString &iso, const QString &entryName, const QString &target, QString &error) {
    auto archive = archive_read_new();
    archive_read_support_format_iso9660(archive);
    const auto freeArchive = qScopeGuard([&] { archive_read_free(archive); });
    if (archive_read_open_filename(archive, QFile::encodeName(iso).constData(), 1 << 20) != ARCHIVE_OK) {
        error = "The installation ISO couldn't be read.";
        return false;
    }
    archive_entry *entry = nullptr;
    while (archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
        auto name = QString::fromUtf8(archive_entry_pathname(entry));
        if (name.startsWith("./")) name = name.mid(2);
        if (name != entryName || archive_entry_filetype(entry) != AE_IFREG) continue;
        if (archive_entry_size(entry) > (qint64(512) << 20)) {
            error = "The installer's " + entryName + " is unexpectedly large.";
            return false;
        }
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            error = "Couldn't write " + target + ".";
            return false;
        }
        out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        char buffer[1 << 16];
        for (la_ssize_t n; (n = archive_read_data(archive, buffer, sizeof buffer)) > 0;) {
            if (out.write(buffer, n) != n) {
                error = "Couldn't write " + target + ".";
                return false;
            }
        }
        return true;
    }
    error = "The installation ISO has no " + entryName + ", so it can't be installed automatically.";
    return false;
}

// Removes a VM folder that createVm() started but couldn't finish, with everything it wrote there.
void removeUnfinishedVm(const QString &directory) {
    for (const auto &file : {systemDisk, cloudSeedDisc, answersDisc, installerKernel, installerInitrd})
        QFile::remove(directory + file);
    QDir().rmdir(directory);
}

// A cloud image's own size is just big enough for its system: grows it to the size asked for, and writes
// the cloud-init disc it sets itself up from on its first boot.
bool prepareCloudDisk(const QString &directory, int sizeGiB, const QVariantMap &seed, QString &error) {
    QByteArray output;
    if (!run("qemu-img", {"resize", "-f", "qcow2", directory + systemDisk, QString::number(sizeGiB) + "G"}, output,
                error))
        return false;
    return CloudSeed::writeIso(directory + cloudSeedDisc, "cidata",
            {{"user-data", seed["userData"].toByteArray()}, {"meta-data", seed["metaData"].toByteArray()},
                    {"network-config", seed["networkConfig"].toByteArray()}},
            error);
}

// What an unattended installer sets up: the user's account from `answers`, and this computer's own time
// zone, locale and keyboard when they're plain enough to pass on.
Unattended::Settings setupSettings(const QVariantMap &answers, const QString &vmName, bool uefi) {
    Unattended::Settings settings;
    settings.user = answers["user"].toString();
    settings.password = answers["password"].toString();
    settings.passwordHash = CloudSeed::hashPassword(settings.password);
    settings.hostname = CloudSeed::hostname(vmName);
    const auto zone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    settings.timezone =
            QRegularExpression("^[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+)*$").match(zone).hasMatch() ? zone : QString("UTC");
    const auto locale = QLocale::system().name();
    settings.locale = QRegularExpression("^[a-z]{2,3}_[A-Z]{2}$").match(locale).hasMatch() ? locale : QString("en_US");
    settings.keyboard = Unattended::keyboardFor(settings.locale);
    settings.uefi = uefi;
    return settings;
}

// Writes the installer's answers disc and, for Ubuntu, copies its installer's kernel out of the ISO, since
// the first boot starts that kernel directly.
bool writeSetupMedia(const QString &kind, const Unattended::Settings &settings, const QString &iso,
        const QString &identity, const QString &directory, QString &error) {
    const auto answers = directory + answersDisc;
    bool written;
    if (kind == "windows") {
        written = CloudSeed::writeIso(answers, "OMAWARE",
                {{"autounattend.xml", Unattended::autounattend(settings, QFileInfo(iso).fileName())}}, error);
    } else {
        written = CloudSeed::writeIso(answers, "cidata",
                          {{"user-data", Unattended::subiquityUserData(settings)},
                                  {"meta-data", Unattended::subiquityMetaData("omaware-setup-" + identity)}},
                          error) &&
                  extractFromIso(iso, "casper/vmlinuz", directory + installerKernel, error) &&
                  extractFromIso(iso, "casper/initrd", directory + installerInitrd, error);
    }
    if (written) QFile::setPermissions(answers, QFile::ReadOwner | QFile::WriteOwner);
    return written;
}

// Records on a new VM what start() and finishSetup() need for its unattended installation, and sets its
// boot order: Windows boots its installer from the first CD drive, which must be the installation media;
// Ubuntu boots from its disk, since start() boots its installer's kernel directly the first time.
void markForSetup(QDomDocument &doc, const QString &kind, bool uefi, const QString &directory, const QString &media) {
    auto root = doc.documentElement(), devices = root.firstChildElement("devices");
    const auto answers = directory + answersDisc;
    auto setup = doc.createElementNS(Unattended::ns, "omasetup:setup");
    setup.setAttribute("kind", kind);
    setup.setAttribute("state", "new");
    setup.setAttribute("uefi", uefi ? "1" : "0");
    setup.setAttribute("answers", answers);
    setup.setAttribute("media", media);
    if (kind == "subiquity") {
        setup.setAttribute("kernel", directory + installerKernel);
        setup.setAttribute("initrd", directory + installerInitrd);
    }
    root.firstChildElement("metadata").appendChild(setup);
    auto os = root.firstChildElement("os");
    for (auto b = os.firstChildElement("boot"); !b.isNull(); b = os.firstChildElement("boot"))
        os.removeChild(b);
    for (const auto &dev : kind == "subiquity" ? QStringList{"hd", "cdrom"} : QStringList{"cdrom", "hd"})
        child(doc, os, "boot").setAttribute("dev", dev);
    // The answers disc goes last, behind the installation media.
    const auto disks = devices.elementsByTagName("disk");
    for (int i = 0; i < disks.size(); ++i) {
        auto e = disks.at(i).toElement();
        if (e.attribute("device") == "cdrom" && e.firstChildElement("source").attribute("file") == answers) {
            devices.appendChild(devices.removeChild(e));
            break;
        }
    }
}

// Appends the network adapter XML for one of NetworkCatalog's choices, with this MAC address if given.
void appendAdapter(QDomDocument &doc, const QVariantMap &choice, bool windows, const QString &mac = {}) {
    QString nic, ignored;
    DomainConfig::networkDevice(doc.toString(-1), {}, choice["kind"].toString(), choice["source"].toString(),
            windows ? "e1000e" : "virtio", true, false, nic, ignored);
    QDomDocument device;
    device.setContent(nic);
    if (!mac.isEmpty()) {
        auto macElement = device.documentElement().firstChildElement("mac");
        if (macElement.isNull()) {
            macElement = device.createElement("mac");
            device.documentElement().insertBefore(macElement, device.documentElement().firstChild());
        }
        macElement.setAttribute("address", mac);
    }
    doc.documentElement().firstChildElement("devices").appendChild(doc.importNode(device.documentElement(), true));
}

// Adds the new VM's network adapters: one on in["networkId"] (none for "none"), or, with in["networks"],
// several, each with the MAC address its first-boot network settings expect.
bool addAdapters(QDomDocument &doc, QVariantMap &in, const QVariantList &choices, bool windows, QString &error) {
    QVariantMap choice;
    for (const auto &v : choices)
        if (v.toMap()["id"] == in.value("networkId", "user")) choice = v.toMap();
    if (in.contains("networks")) {
        QSet<QString> macs;
        for (const auto &entry : in["networks"].toList()) {
            const auto want = entry.toMap();
            QVariantMap found;
            for (const auto &v : choices)
                if (v.toMap()["id"] == want["id"]) found = v.toMap();
            if (found.isEmpty() || !found["available"].toBool()) {
                error = "A network for this VM is unavailable: " +
                        (found.isEmpty() ? want["id"].toString() : found["reason"].toString());
                return false;
            }
            const auto mac = want["mac"].toString().toLower();
            if (!QRegularExpression("^52:54:00(:[0-9a-f]{2}){3}$").match(mac).hasMatch() || macs.contains(mac)) {
                error = "Each adapter needs its own MAC address.";
                return false;
            }
            macs.insert(mac);
            appendAdapter(doc, found, windows, mac);
        }
    } else if (in["networkId"] != "none") {
        if (choice.isEmpty() || !choice["available"].toBool()) {
            error = "The selected network is unavailable. Refresh its configuration before creating the VM.";
            return false;
        }
        appendAdapter(doc, choice, windows);
    }
    return true;
}

// The unattended-setup marker of a VM still being set up, or a null element.
QDomElement setupMarker(virDomainPtr d, QDomDocument &doc) {
    char *raw = virDomainGetMetadata(d, VIR_DOMAIN_METADATA_ELEMENT, Unattended::ns, VIR_DOMAIN_AFFECT_CONFIG);
    if (!raw) {
        virResetLastError();
        return {};
    }
    doc.setContent(QString::fromUtf8(raw));
    free(raw);
    return doc.documentElement();
}

void saveSetupMarker(virDomainPtr d, const QDomDocument &doc) {
    virDomainSetMetadata(d, VIR_DOMAIN_METADATA_ELEMENT, doc.toString(-1).toUtf8().constData(), "omasetup",
            Unattended::ns, VIR_DOMAIN_AFFECT_CONFIG);
}
}

// "capabilities": what the Create VM dialog offers on this host.
void VmWorker::capabilities(const Request &request) {
    virNodeInfo host{};
    virNodeGetInfo(conn_, &host);
    QByteArray installed;
    QString failure;
    const bool creatorReady = run("virt-install", {"--osinfo", "list"}, installed, failure, 15000);
    const auto osList = QString::fromUtf8(installed);
    QVariantList presets;
    for (const auto &[id, label] : supportedPresets())
        if (osList.contains(QRegularExpression("(^|[,\\n])\\s*" + QRegularExpression::escape(id) + "(,|\\n|$)")))
            presets.append(QVariantMap{{"id", id}, {"label", label}});
    // Every OS id this host's libosinfo knows, so an ISO from the shop can select its exact OS.
    QStringList osinfo;
    for (const auto &token : osList.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts))
        if (osinfoId.match(token).hasMatch()) osinfo << token;
    osinfo.removeDuplicates();
    QString status;
    request.done(true, "Host capabilities loaded",
            {{"cpus", host.cpus}, {"memoryMiB", qulonglong(host.memory / 1024)}, {"storage", Paths::vms()},
                    {"isos", Paths::isos()}, {"virtInstall", creatorReady}, {"presets", presets}, {"osinfo", osinfo},
                    {"tpm", tpmAvailable()}, {"networks", NetworkCatalog::discover(conn_, status)}});
}

// "vm.create": a new VM from an installation ISO, an existing disk image or appliance, or a cloud image (for
// labs). virt-install with libosinfo proposes the definition; OmaWare adjusts it and defines it, stopped.
void VmWorker::createVm(const Request &request) {
    auto &in = request.in;
    auto name = in["name"].toString().trimmed();
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9 ._-]{0,47}$").match(name).hasMatch())
        return request.fail("Use a VM name of 1–48 letters, numbers, spaces, dots, dashes or underscores, starting "
                            "with a letter or number.");
    name = "omaware-" + name.toLower().replace(' ', '-');
    if (Domain(virDomainLookupByName(conn_, name.toUtf8().constData()), virDomainFree))
        return request.fail("A VM with this name already exists.");
    const int cpus = in.value("cpus", 2).toInt(), memory = in.value("memoryMiB", 4096).toInt(),
              size = in.value("diskGiB", 32).toInt();
    virNodeInfo host{};
    virNodeGetInfo(conn_, &host);
    if (cpus < 1 || unsigned(cpus) > std::min(256u, std::max(1u, host.cpus)) || memory < 256 ||
            qulonglong(memory) > host.memory / 1024 || size < 1 || size > 2048)
        return request.fail("Choose CPU and memory within the host limits and a 1–2048 GiB disk.");

    const bool cloud = in["sourceMode"] == "cloud";
    auto preset = in.value("preset", "generic").toString();
    const auto firmware = in.value("firmware", "bios").toString();
    const bool uefi = firmware == "uefi";
    if (!resolvePreset(preset, cloud) || !QStringList{"bios", "uefi"}.contains(firmware))
        return request.fail("Choose a supported OS preset and firmware.");

    const bool importing = in["sourceMode"] == "disk" || cloud ||
                           (!request.provisioning && ApplianceImport::mediaType(in["source"].toString()) == "disk");
    auto source = in["source"].toString();
    // An agent's approved media is read through the descriptor manage() pinned, not a replaceable file name.
    // CLOEXEC is fine: it remains open in this process, where qemu-img's parent runs.
    if (request.provisioning && importing) source = QString("/proc/%1/fd/%2").arg(::getpid()).arg(request.mediaFd);
    if (!QFileInfo(source).isAbsolute() || !QFileInfo(source).isFile() || !QFileInfo(source).isReadable())
        return request.fail("Choose a readable local ISO or disk image.");
    // Cloud images only come from OmaWare's own image folder, where they were checked against their
    // publisher's checksum.
    if (cloud && QFileInfo(source).canonicalPath() != QFileInfo(CloudImages::folder()).canonicalFilePath())
        return request.fail("Cloud images must be in OmaWare's image folder.");

    // "Set it up for me": answers for the installer, only for ISOs that support it.
    const auto answers = in.value("unattended").toMap();
    const auto setupKind = answers.isEmpty() ? QString() : Unattended::kindForFile(QFileInfo(source).fileName());
    if (!answers.isEmpty()) {
        const auto password = answers["password"].toString();
        if (importing || setupKind.isEmpty()) return request.fail("This ISO can't be installed automatically.");
        if (!CloudSeed::validUser(answers["user"].toString()))
            return request.fail("Choose a user name of lower-case letters, digits, dashes or underscores.");
        if (password.isEmpty() || password.size() > 127 || password.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
            return request.fail("Choose a password of 1–127 characters.");
    }
    const auto seed = in.value("seed").toMap();
    if (cloud && (seed["userData"].toByteArray().size() > 262144 || seed["metaData"].toByteArray().isEmpty() ||
                         seed["networkConfig"].toByteArray().size() > 65536))
        return request.fail("The first-boot setup is missing or too large.");

    const auto parent = in.value("location").toString().isEmpty() ? Paths::vms() : in.value("location").toString();
    if (!request.stillAuthorized()) return;
    QString failure;
    if (request.provisioning && !AgentProvision::storageRoot(failure, true)) return request.fail(failure);
    if (!QFileInfo(parent).isAbsolute() || !QDir().mkpath(parent) || !QFileInfo(parent).isWritable())
        return request.fail("The storage directory is not writable.");

    const auto identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto directory = parent + "/" + name + "-" + identity.left(8), disk = directory + systemDisk;
    // Staging sits next to the new VM: an appliance can unpack to tens of GiB, too much for a RAM-backed /tmp.
    ApplianceImport importedDisk(parent);
    quint64 required = qulonglong(size) * 1073741824;
    if (importing) {
        emit progress("Unpacking and checking the appliance or disk image…");
        if (!importedDisk.prepare(source, in["source"].toString(), failure)) return request.fail(failure);
        required = std::max<quint64>(importedDisk.capacity(), cloud ? qulonglong(size) * 1073741824 : 0);
    }
    if (QStorageInfo(parent).bytesAvailable() < qint64(required))
        return request.fail("There is not enough free space for the disk's full capacity at this location.");
    if (!request.stillAuthorized()) return;
    if (!QDir().mkdir(directory)) return request.fail("Could not create a new unique VM storage directory.");
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // From here on, a failure removes the new folder with everything written into it.
    const auto abandon = [&](const QString &why) {
        removeUnfinishedVm(directory);
        request.fail(why);
    };

    emit progress(importing ? "Copying the source disk into a new independent image…" : "Creating the VM's new disk…");
    QByteArray output;
    bool diskOk =
            importing ? importedDisk.convert(disk, failure)
                      : run("qemu-img", {"create", "-f", "qcow2", disk, QString::number(size) + "G"}, output, failure);
    if (diskOk && cloud) diskOk = prepareCloudDisk(directory, size, seed, failure);
    if (diskOk && !setupKind.isEmpty()) {
        emit progress("Writing the answers for the installer…");
        diskOk = writeSetupMedia(
                setupKind, setupSettings(answers, in["name"].toString(), uefi), source, identity, directory, failure);
    }
    if (!diskOk) return abandon(failure);
    QFile::setPermissions(disk, QFile::ReadOwner | QFile::WriteOwner);

    emit progress("Preparing and validating the VM definition…");
    // Windows installs without extra drivers on SATA disks, e1000e network cards and a standard VGA display.
    // A TPM (which Windows 11 requires) and Secure Boot make it look like a current PC.
    const bool windows = preset.startsWith("win");
    const bool tpm = in.value("tpm", false).toBool();
    if (tpm && !tpmAvailable())
        return abandon("This computer can't give VMs a TPM yet: install the swtpm package, then try again.");
    if (disk.contains(',') || source.contains(','))
        return abandon("Choose paths without commas for the virt-install workflow.");
    QStringList args{"--connect", uri_, "--name", name, "--uuid", identity, "--memory", QString::number(memory),
            "--vcpus", QString::number(cpus), "--osinfo", preset, "--disk",
            "path=" + disk + ",format=qcow2,bus=" + (windows ? "sata" : "virtio"), "--graphics", "vnc,listen=none",
            "--video", windows ? "vga" : "virtio", "--network", "none", "--tpm",
            tpm ? "emulator,model=tpm-crb,version=2.0" : "none", "--channel",
            "unix,target.type=virtio,target.name=org.qemu.guest_agent.0", "--noautoconsole", "--dry-run", "--print-xml",
            "1"};
    if (importing)
        args << "--import";
    else
        args << "--cdrom" << source;
    if (cloud) args << "--disk" << "path=" + directory + cloudSeedDisc + ",device=cdrom";
    if (!setupKind.isEmpty()) args << "--disk" << "path=" + directory + answersDisc + ",device=cdrom";
    // Secure Boot when the host has firmware for it, preferably with Microsoft's keys preloaded; otherwise
    // plain UEFI. virt-install's dry run doesn't resolve firmware, so the choice follows the host's firmware
    // descriptors instead of waiting for the define to fail.
    const auto secureFirmware = windows && uefi ? DomainConfig::secureBootFirmware() : QString();
    const QString secureBoot = "uefi,firmware.feature0.name=secure-boot,firmware.feature0.enabled=yes,"
                               "firmware.feature1.name=enrolled-keys,firmware.feature1.enabled=" +
                               QString(secureFirmware == "enrolled" ? "yes" : "no");
    const auto boot = uefi ? QString("uefi") : importing ? QString("hd") : QString("cdrom,hd");
    const bool proposed =
            !secureFirmware.isEmpty() && run("virt-install", args + QStringList{"--boot", secureBoot}, output, failure);
    if (!proposed && !run("virt-install", args + QStringList{"--boot", boot}, output, failure)) return abandon(failure);
    QDomDocument doc;
    if (!doc.setContent(output)) return abandon("virt-install did not return a valid domain definition.");

    auto root = doc.documentElement(), devices = root.firstChildElement("devices");
    if (!importing) {
        // virt-install's first stage normally stops at reboot so its own installer can swap the definition.
        // OmaWare keeps the media attached instead and lets the user eject it.
        auto reboot = root.firstChildElement("on_reboot");
        if (!reboot.isNull()) root.removeChild(reboot);
        child(doc, root, "on_reboot", "restart");
    }
    auto metadata = root.firstChildElement("metadata");
    if (metadata.isNull()) metadata = child(doc, root, "metadata");
    metadata.appendChild(doc.createElementNS(Management::ownershipNs, "omaware:managed"));
    if (!setupKind.isEmpty()) markForSetup(doc, setupKind, uefi, directory, source);
    if (const auto lab = in["lab"].toMap(); !lab.isEmpty()) {
        // Which lab built this VM and the login it was set up with (the name of a saved login, never a password).
        auto tag = doc.createElementNS(Labs::ns, "omalab:lab");
        for (const QString key : {"name", "slug", "login", "user"})
            tag.setAttribute(key, lab[key].toString().left(64));
        metadata.appendChild(tag);
    }
    QString status;
    if (!addAdapters(doc, in, NetworkCatalog::discover(conn_, status), windows, failure)) return abandon(failure);
    auto balloon = devices.firstChildElement("memballoon");
    if (!balloon.isNull() && balloon.attribute("model") == "virtio")
        child(doc, balloon, "stats").setAttribute("period", "5");
    if (request.provisioning && !request.stillAuthorized()) return removeUnfinishedVm(directory);

    Domain defined(virDomainDefineXMLFlags(conn_, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE),
            virDomainFree);
    if (!defined) return abandon(Virt::lastError("Define VM"));
    emit created(identity);
    const QString summary = !setupKind.isEmpty() ? "VM created. Start it and the installer sets it up by itself, "
                                                   "then OmaWare removes the installation media."
                                                 : "VM created. Start it to boot the selected installation media or "
                                                   "imported disk.";
    request.done(true, (QStringList{summary} + importedDisk.notes()).join(' '),
            {{"uuid", identity}, {"storage", directory}, {"importNotes", importedDisk.notes()},
                    {"unattended", setupKind}});
}

int VmWorker::start(virDomainPtr d) {
    QDomDocument marker;
    auto setup = setupMarker(d, marker);
    if (setup.isNull()) return virDomainCreate(d);
    const auto kind = setup.attribute("kind");
    const bool first = setup.attribute("state") == "new";
    if (first) {
        setup.setAttribute("state", "installing");
        saveSetupMarker(d, marker);
    }
    if (kind == "subiquity") {
        // Ubuntu's installer only runs without its "Continue with autoinstall?" question when "autoinstall"
        // is on the kernel command line: boot its kernel directly, for this run only. The saved definition
        // stays as it is, and the installer powers the VM off when it's done.
        QDomDocument doc;
        doc.setContent(Virt::definition(d));
        auto os = doc.documentElement().firstChildElement("os");
        child(doc, os, "kernel", setup.attribute("kernel"));
        child(doc, os, "initrd", setup.attribute("initrd"));
        child(doc, os, "cmdline", "autoinstall ---");
        auto live = virDomainCreateXML(conn_, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_NONE);
        if (!live) return -1;
        virDomainFree(live);
        return 0;
    }
    const int result = virDomainCreate(d);
    // Windows on UEFI asks to "Press any key to boot from CD or DVD" the first time. Enter answers that, and
    // also picks "Windows Setup" if a later key lands on the Windows Boot Manager menu (a key stops its countdown).
    if (result == 0 && first && kind == "windows" && setup.attribute("uefi") == "1") {
        const auto uuid = Virt::uuidOf(d);
        if (!uuid.isEmpty()) pressKeys(uuid, 12);
    }
    return result;
}

void VmWorker::pressKeys(const QString &uuid, int times) {
    if (times <= 0 || !conn_) return;
    QTimer::singleShot(700, this, [this, uuid, times] {
        if (!conn_) return;
        Domain d(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
        if (!d || virDomainIsActive(d.get()) != 1) return;
        unsigned int enter = 28; // KEY_ENTER
        virDomainSendKey(d.get(), VIR_KEYCODE_SET_LINUX, 60, &enter, 1, 0);
        pressKeys(uuid, times - 1);
    });
}

void VmWorker::finishSetup(const QString &uuid, int detail) {
    if (storageOnly_ || !conn_) return;
    Domain d(virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData()), virDomainFree);
    if (!d || virDomainIsActive(d.get()) == 1 || !owned(d.get())) {
        virResetLastError();
        return;
    }
    QDomDocument marker;
    const auto setup = setupMarker(d.get(), marker);
    // Only when the guest itself switched off: the installer when it's done (Linux), or you shutting down a
    // Windows VM that finished setting up. A forced stop leaves everything in place for the next start.
    if (setup.isNull() || setup.attribute("state") != "installing" || detail != VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN)
        return;
    const auto kind = setup.attribute("kind"), answers = setup.attribute("answers"), media = setup.attribute("media");
    QDomDocument doc;
    doc.setContent(Virt::definition(d.get()));
    auto root = doc.documentElement(), devices = root.firstChildElement("devices");
    // Take out the answers disc and the installation media, so the VM boots its new system.
    const auto disks = devices.elementsByTagName("disk");
    for (int i = disks.size() - 1; i >= 0; --i) {
        auto e = disks.at(i).toElement();
        if (e.attribute("device") != "cdrom") continue;
        const auto file = e.firstChildElement("source").attribute("file");
        if (file == answers)
            devices.removeChild(e);
        else if (file == media)
            e.removeChild(e.firstChildElement("source"));
    }
    auto metadata = root.firstChildElement("metadata");
    for (auto e = metadata.firstChildElement(); !e.isNull();) {
        auto next = e.nextSiblingElement();
        if (e.namespaceURI() == Unattended::ns || e.tagName().endsWith(":setup")) metadata.removeChild(e);
        e = next;
    }
    const auto name = Virt::displayName(d.get());
    Domain saved(virDomainDefineXMLFlags(conn_, doc.toString(-1).toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE),
            virDomainFree);
    if (!saved) {
        emit finished(Virt::lastError("Finish the unattended setup of " + name), false);
        return;
    }
    for (const auto &attribute : {"answers", "kernel", "initrd"})
        if (!setup.attribute(attribute).isEmpty()) QFile::remove(setup.attribute(attribute));
    if (kind == "windows") {
        emit finished(
                name + " finished setting up: OmaWare removed its answer disc and the Windows installation media.",
                true);
        return;
    }
    // Starting it is like any start: a VM contained meanwhile has to pass its checks.
    if (const auto blocked = startBlocker(saved.get()); !blocked.isEmpty()) {
        emit finished(name + " is installed, but wasn't started. " + blocked, false);
        return;
    }
    const int started = virDomainCreate(saved.get());
    emit finished(started == 0 ? name + " is installed and starting its new system."
                               : Virt::lastError(name + " is installed, but starting it failed"),
            started == 0);
}
