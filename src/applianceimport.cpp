// SPDX-License-Identifier: GPL-3.0-or-later
#include "applianceimport.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStorageInfo>
#include <QXmlStreamReader>
#include <QtEndian>
#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <optional>

namespace {
constexpr qint64 maxBytes = 64LL << 30;              // a source file, OVA member or decompressed disk
constexpr qint64 maxSmall = 4LL << 20;               // OVF, manifest and certificate
constexpr quint64 maxCapacity = 2ULL << 40;
constexpr qint64 stagingTime = 2LL * 60 * 60 * 1000; // appliances are tens of GiB
constexpr int chunk = 4 << 20;
bool run(const QStringList &args, QByteArray &output, QString &error, int timeout) {
    QProcess p; p.setProcessChannelMode(QProcess::MergedChannels); p.start("qemu-img", args);
    if (!p.waitForStarted(5000)) { error = "Appliance import requires qemu-img."; return false; }
    if (!p.waitForFinished(timeout)) { p.kill(); p.waitForFinished(); error = "Disk processing took too long and was stopped."; return false; }
    output = p.readAll();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) { error = "qemu-img: " + QString::fromUtf8(output.left(4096)); return false; }
    return true;
}
bool plainName(const QString &name) {
    return !name.isEmpty() && name.size() <= 255 && name != "." && name != ".."
        && !name.contains('/') && !name.contains('\\') && !name.contains(':')
        && !name.contains(QRegularExpression("[\\x00-\\x1f]"));
}
// Accept only a sparse VMDK with an embedded descriptor and one extent: qemu then reads that
// file alone, whatever the extent is called. Descriptor-only VMDKs and parent links could
// otherwise make qemu-img read host files, so they are refused before qemu-img sees the disk.
bool standaloneVmdk(const QString &path, QString &error) {
    auto reject = [&] { error = "Unsupported VMDK: use a standalone monolithicSparse or streamOptimized disk, without parent or external extents."; return false; };
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return reject();
    const auto header = f.read(512);
    if (header.size() != 512 || header.left(4) != "KDMV") return reject();
    const auto offset = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(header.constData() + 28));
    const auto sectors = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(header.constData() + 36));
    if (!offset || !sectors || sectors > 2048 || offset > quint64(f.size()) / 512 || sectors > quint64(f.size()) / 512 - offset) return reject();
    if (!f.seek(qint64(offset * 512))) return reject();
    const auto descriptor = f.read(qint64(sectors * 512));
    auto text = QString::fromLatin1(descriptor.constData(), descriptor.indexOf('\0') >= 0 ? descriptor.indexOf('\0') : descriptor.size());
    bool type = false, parent = false; int extents = 0;
    for (auto line : text.split('\n')) {
        line = line.trimmed(); if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith("createType")) {
            if (!QRegularExpression(R"rx(^createType\s*=\s*"(monolithicSparse|streamOptimized)"$)rx").match(line).hasMatch()) return reject();
            type = true;
        } else if (line.startsWith("parentCID")) {
            if (!QRegularExpression("^parentCID\\s*=\\s*ffffffff$", QRegularExpression::CaseInsensitiveOption).match(line).hasMatch()) return reject();
            parent = true;
        } else if (line.startsWith("parentFileNameHint")) return reject();
        else if (line.startsWith("RW") || line.startsWith("RDONLY") || line.startsWith("NOACCESS")) {
            if (!QRegularExpression(R"rx(^(RW|RDONLY)\s+[0-9]+\s+SPARSE\s+"[^"]*"$)rx").match(line).hasMatch()) return reject();
            ++extents;
        } else if (!QRegularExpression(R"(^[A-Za-z][A-Za-z0-9.]*\s*=)").match(line).hasMatch()) return reject();
    }
    return type && parent && extents == 1 ? true : reject();
}
// What OmaWare takes from an OVF: the one disk file, and hardware hints it reports but does not apply.
struct Envelope { QString disk, compression; QStringList notes; };
bool readOvf(const QByteArray &data, Envelope &envelope, QString &error) {
    const QString ns = "http://schemas.dmtf.org/ovf/envelope/1";
    QXmlStreamReader xml(data);
    int disks = 0, systems = 0, collections = 0, cpus = 0; qint64 memoryMiB = 0;
    QString diskRef, diskFormat, firmware, itemType, itemQuantity, itemUnits;
    QHash<QString, QPair<QString, QString>> files; // id -> href, compression
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.tokenType() == QXmlStreamReader::DTD || xml.tokenType() == QXmlStreamReader::EntityReference) { error = "OVF documents with a DTD or entities are not supported."; return false; }
        if (xml.isEndElement() && xml.name() == u"Item") {
            if (itemType == "3") cpus = itemQuantity.toInt();
            if (itemType == "4" && itemUnits.remove(' ') == "byte*2^20") memoryMiB = itemQuantity.toLongLong();
            if (itemType == "4" && itemUnits == "byte*2^30") memoryMiB = itemQuantity.toLongLong() * 1024;
        }
        if (!xml.isStartElement()) continue;
        const auto tag = xml.name(); const auto attributes = xml.attributes();
        if (tag == u"VirtualSystem") ++systems;
        else if (tag == u"VirtualSystemCollection") ++collections;
        else if (tag == u"Disk") { ++disks; diskRef = attributes.value(ns, "fileRef").toString(); diskFormat = attributes.value(ns, "format").toString(); }
        else if (tag == u"File") {
            const auto id = attributes.value(ns, "id").toString(), href = attributes.value(ns, "href").toString(), compression = attributes.value(ns, "compression").toString();
            if (id.isEmpty() || files.contains(id) || !plainName(href)) { error = "The OVF has an unsafe or invalid file reference."; return false; }
            if (attributes.hasAttribute(ns, "chunkSize")) { error = "OVAs with disks split into chunks are not supported."; return false; }
            if (!QStringList{"", "identity", "gzip"}.contains(compression)) { error = "The OVF uses an unsupported disk compression: " + compression; return false; }
            files.insert(id, {href, compression});
        } else if (tag == u"Item") { itemType.clear(); itemQuantity.clear(); itemUnits.clear(); }
        else if (tag == u"ResourceType") itemType = xml.readElementText().trimmed();
        else if (tag == u"VirtualQuantity") itemQuantity = xml.readElementText().trimmed();
        else if (tag == u"AllocationUnits") itemUnits = xml.readElementText().trimmed();
        else if (tag == u"Config") {
            QString key, value;
            for (const auto &a : attributes) { if (a.name() == u"key") key = a.value().toString(); if (a.name() == u"value") value = a.value().toString(); }
            if (key == "firmware") firmware = value;
        }
    }
    if (xml.hasError()) { error = "The OVF descriptor is not valid XML."; return false; }
    if (systems != 1 || collections) { error = "Only an OVA with exactly one virtual machine is supported."; return false; }
    if (disks != 1) { error = disks ? "OVAs with more than one disk are not supported." : "The OVF does not describe a disk."; return false; }
    if (files.size() != 1 || !files.contains(diskRef)) { error = "Only an OVA whose one referenced file is its disk is supported."; return false; }
    if (!diskFormat.contains("vmdk", Qt::CaseInsensitive)) { error = "The OVF disk must be a VMDK."; return false; }
    envelope.disk = files[diskRef].first; envelope.compression = files[diskRef].second;
    if (cpus > 0 || memoryMiB > 0)
        envelope.notes << QString("The OVF suggests %1 and %2; the new VM uses the CPU and memory chosen in Create VM.")
            .arg(cpus > 0 ? QString("%1 vCPU(s)").arg(cpus) : "an unknown CPU count", memoryMiB > 0 ? QString("%1 MiB of memory").arg(memoryMiB) : "an unknown amount of memory");
    if (!firmware.isEmpty()) envelope.notes << "The OVF asks for " + firmware + " firmware; choose matching firmware in Create VM.";
    envelope.notes << "Only the disk was imported. The OVF's network, controller and other device settings are not applied.";
    return true;
}
// Reads a small archive member fully, refusing anything over the limit.
bool readSmall(archive *ar, qint64 size, QByteArray &out) {
    if (size < 0 || size > maxSmall) return false;
    out.resize(size); qint64 done = 0;
    while (done < size) {
        const auto n = archive_read_data(ar, out.data() + done, size - done);
        if (n <= 0) return false;
        done += n;
    }
    char extra; return archive_read_data(ar, &extra, 1) == 0;
}
std::optional<QCryptographicHash::Algorithm> algorithm(const QString &name) {
    if (name == "SHA1") return QCryptographicHash::Sha1;
    if (name == "SHA256") return QCryptographicHash::Sha256;
    if (name == "SHA512") return QCryptographicHash::Sha512;
    return std::nullopt;
}
}
ApplianceImport::ApplianceImport(const QString &stagingParent) : staging_(stagingParent + "/.omaware-import-XXXXXX") {}
QString ApplianceImport::mediaType(const QString &path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "iso") return "iso";
    if (suffix == "ova" || suffix == "qcow2") return "disk";
    return {};
}
// Streams the OVA once from the opened file into private staging; the original is never
// read again, so later changes to it cannot affect what is checked and converted.
bool ApplianceImport::stageOva(int fd, QString &error) {
    std::unique_ptr<archive, decltype(&archive_read_free)> ar(archive_read_new(), archive_read_free);
    archive_read_support_format_tar(ar.get()); // an OVA is a plain tar; no other formats or filters
    if (archive_read_open_fd(ar.get(), fd, 1 << 20) != ARCHIVE_OK) { error = "The OVA is not a plain tar archive."; return false; }
    QElapsedTimer timer; timer.start();
    QSet<QString> names; Envelope envelope; QString ovfName; QByteArray ovf, manifest;
    QHash<QString, QPair<QCryptographicHash::Algorithm, QByteArray>> expected; // member -> algorithm, hex digest
    QByteArray diskDigest; bool haveDisk = false, certificate = false;
    QByteArray buffer(chunk, Qt::Uninitialized);
    archive_entry *entry = nullptr; int status;
    while ((status = archive_read_next_header(ar.get(), &entry)) == ARCHIVE_OK) {
        const auto name = QString::fromUtf8(archive_entry_pathname(entry));
        const auto suffix = QFileInfo(name).suffix().toLower(); const qint64 bytes = archive_entry_size(entry);
        if (names.size() >= 16 || !plainName(name) || names.contains(name)
            || archive_entry_filetype(entry) != AE_IFREG || archive_entry_symlink(entry) || archive_entry_hardlink(entry)
            || archive_entry_sparse_count(entry) || bytes < 0 || bytes > maxBytes) {
            error = "Unsafe or unsupported OVA member (only flat regular files; no links, folders, traversal, duplicates or sparse entries)."; return false;
        }
        names.insert(name);
        if (ovfName.isEmpty()) {
            // The OVF standard puts the descriptor first, so it is checked before any disk data is written.
            if (suffix != "ovf" || !readSmall(ar.get(), bytes, ovf)) { error = "The OVA must start with its OVF descriptor."; return false; }
            if (!readOvf(ovf, envelope, error)) return false;
            ovfName = name; continue;
        }
        if (name == envelope.disk) {
            const auto check = expected.contains(name) ? std::optional(expected[name].first) : std::nullopt;
            QCryptographicHash hash(check.value_or(QCryptographicHash::Sha256));
            const bool gzip = envelope.compression == "gzip";
            QFile out(staging_.filePath("disk.vmdk"));
            if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { error = out.errorString(); return false; }
            out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            z_stream z{}; if (gzip && inflateInit2(&z, 15 + 16) != Z_OK) { error = "Could not start decompressing the disk."; return false; }
            const auto end = qScopeGuard([&] { if (gzip) inflateEnd(&z); });
            QByteArray inflated(gzip ? chunk : 0, Qt::Uninitialized);
            qint64 consumed = 0, written = 0; bool ended = false;
            auto store = [&](const char *data, qint64 n) {
                if (written + n > maxBytes) { error = "The appliance disk is larger than 64 GiB."; return false; }
                if (out.write(data, n) != n) { error = "Could not stage the disk: " + out.errorString(); return false; }
                written += n; return true;
            };
            for (;;) {
                const auto n = archive_read_data(ar.get(), buffer.data(), buffer.size());
                if (n < 0 || timer.elapsed() > stagingTime) { error = "Could not read the OVA disk (damaged archive or time limit)."; return false; }
                if (!n) break;
                consumed += n; if (consumed > bytes) { error = "The OVA disk is longer than its archive entry."; return false; }
                if (check) hash.addData(QByteArrayView(buffer.constData(), n));
                if (!gzip) { if (!store(buffer.constData(), n)) return false; continue; }
                z.next_in = reinterpret_cast<Bytef *>(buffer.data()); z.avail_in = uInt(n);
                do {
                    if (ended && z.avail_in) { if (inflateReset(&z) != Z_OK) { error = "The compressed disk is corrupt."; return false; } ended = false; }
                    z.next_out = reinterpret_cast<Bytef *>(inflated.data()); z.avail_out = uInt(chunk);
                    const int result = inflate(&z, Z_NO_FLUSH);
                    if (result == Z_STREAM_END) ended = true;
                    else if (result != Z_OK && result != Z_BUF_ERROR) { error = "The compressed disk is corrupt."; return false; }
                    if (!store(inflated.constData(), chunk - z.avail_out)) return false;
                } while (z.avail_out == 0 || z.avail_in > 0);
            }
            if (consumed != bytes || (gzip && !ended)) { error = "The OVA disk is truncated."; return false; }
            if (!out.flush()) { error = "Could not stage the disk: " + out.errorString(); return false; }
            out.close();
            if (check) diskDigest = hash.result().toHex();
            haveDisk = true;
        } else if (suffix == "mf") {
            if (haveDisk || !manifest.isEmpty()) { error = "The OVA manifest must come once, before the disk."; return false; }
            if (!readSmall(ar.get(), bytes, manifest) || manifest.isEmpty()) { error = "The OVA manifest is unreadable."; return false; }
            for (auto line : QString::fromUtf8(manifest).split('\n')) {
                line = line.trimmed(); if (line.isEmpty()) continue;
                const auto m = QRegularExpression(R"(^(SHA1|SHA256|SHA512)\(([^)]+)\)\s*=\s*([0-9A-Fa-f]+)$)").match(line);
                const auto kind = m.hasMatch() ? algorithm(m.captured(1)) : std::nullopt;
                if (!kind || expected.contains(m.captured(2)) || m.captured(3).size() != QCryptographicHash::hashLength(*kind) * 2) { error = "The OVA manifest is malformed."; return false; }
                expected.insert(m.captured(2), {*kind, m.captured(3).toLower().toLatin1()});
            }
        } else if (suffix == "cert") {
            QByteArray ignored;
            if (certificate || !readSmall(ar.get(), bytes, ignored)) { error = "The OVA certificate is unreadable."; return false; }
            certificate = true;
        } else { error = "Unexpected file in the OVA: " + name + ". Only its OVF, manifest, certificate and one disk are supported."; return false; }
    }
    if (status != ARCHIVE_EOF) { error = "The OVA archive is damaged."; return false; }
    if (ovfName.isEmpty()) { error = "The OVA has no OVF descriptor."; return false; }
    if (!haveDisk) { error = "The OVA does not contain the disk its OVF references."; return false; }
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const auto actual = it.key() == ovfName ? QCryptographicHash::hash(ovf, it->first).toHex() : it.key() == envelope.disk ? diskDigest : QByteArray();
        if (actual.isEmpty()) { error = "The OVA manifest lists a file that is not in the OVA: " + it.key(); return false; }
        if (actual != it->second) { error = "The OVA file " + it.key() + " does not match its manifest checksum. The download may be damaged."; return false; }
    }
    notes_ << (expected.contains(envelope.disk)
        ? "The disk matched the OVA's manifest checksum. That detects damage, not who published it; verify downloads with the publisher."
        : "The OVA has no manifest checksum for its disk, so damage could not be detected.");
    if (certificate) notes_ << "The OVA's signing certificate was not checked.";
    notes_ << envelope.notes;
    disk_ = staging_.filePath("disk.vmdk"); format_ = "vmdk";
    return standaloneVmdk(disk_, error);
}
// QCOW2 and raw images are copied once into staging; only that private copy is inspected and converted.
bool ApplianceImport::stageCopy(int fd, QString &error) {
    QFile input;
    if (!input.open(fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) { error = "Could not open the source disk."; return false; }
    const auto staged = staging_.filePath("source"); QFile copy(staged);
    if (!copy.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { error = copy.errorString(); return false; }
    copy.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    QByteArray buffer(chunk, Qt::Uninitialized); qint64 total = 0; QElapsedTimer timer; timer.start();
    for (;;) {
        const auto n = input.read(buffer.data(), buffer.size());
        if (n < 0 || total + n > maxBytes || timer.elapsed() > stagingTime) { error = "Could not stage the disk within the size and time limits."; return false; }
        if (!n) break;
        if (copy.write(buffer.constData(), n) != n) { error = "Could not stage the disk: " + copy.errorString(); return false; }
        total += n;
    }
    if (!copy.flush()) { error = "Could not stage the disk: " + copy.errorString(); return false; }
    copy.close();
    QFile f(staged);
    if (!f.open(QIODevice::ReadOnly)) { error = "Could not read the staged disk."; return false; }
    disk_ = staged; format_ = f.read(4) == QByteArray("QFI\xfb", 4) ? "qcow2" : "raw";
    return true;
}
bool ApplianceImport::prepare(const QString &source, const QString &originalName, QString &error) {
    if (!disk_.isEmpty() || !staging_.isValid()) { error = "Could not create private staging for the import."; return false; }
    QFile::setPermissions(staging_.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // Provisioning passes an already pinned descriptor as /proc/<pid>/fd/<n>, which is a link by design.
    const int fd = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_CLOEXEC | (source.startsWith("/proc/") ? 0 : O_NOFOLLOW));
    const auto closeSource = qScopeGuard([fd] { if (fd >= 0) ::close(fd); });
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > maxBytes) { error = "Choose a regular disk or appliance file between 1 byte and 64 GiB."; return false; }
    if (QStorageInfo(staging_.path()).bytesAvailable() < st.st_size) { error = "There is not enough free space next to the VM storage to unpack this appliance."; return false; }
    const auto suffix = QFileInfo(originalName).suffix().toLower();
    if (!(suffix == "ova" ? stageOva(fd, error) : stageCopy(fd, error))) { disk_.clear(); return false; }
    if (suffix == "qcow2" && format_ != "qcow2") { error = "This .qcow2 file is not a QCOW2 image."; disk_.clear(); return false; }
    QByteArray output;
    if (!run({"info", "--output=json", "-f", format_, disk_}, output, error, 60000)) { disk_.clear(); return false; }
    const auto info = QJsonDocument::fromJson(output).toVariant().toMap();
    const auto specific = info["format-specific"].toMap()["data"].toMap();
    const auto capacity = info["virtual-size"].toULongLong();
    if (!capacity || capacity > maxCapacity || info.contains("backing-filename") || info.contains("full-backing-filename") || specific.contains("data-file") || info.value("encrypted", false).toBool()) {
        error = "Import requires a standalone, unencrypted disk of at most 2 TiB, without backing or external data files."; disk_.clear(); return false;
    }
    capacity_ = capacity;
    return true;
}
bool ApplianceImport::convert(const QString &destination, QString &error) const {
    if (!capacity_ || disk_.isEmpty() || QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) { error = "Conversion requires a prepared disk and a new output path."; return false; }
    QByteArray output; const bool ok = run({"convert", "-f", format_, "-O", "qcow2", disk_, destination}, output, error, int(stagingTime));
    if (!ok) QFile::remove(destination);
    return ok;
}
