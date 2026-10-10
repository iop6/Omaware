// SPDX-License-Identifier: GPL-3.0-or-later
// Real qemu-img and libarchive fixtures; nothing here touches libvirt or existing VMs.
#include "applianceimport.h"
#include "isolibrary.h"
#include "agentprovision.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtEndian>
#include <QJsonDocument>
#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#include <sys/stat.h>

class ApplianceTests : public QObject {
    Q_OBJECT
    static QByteArray read(const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {};
        return f.readAll();
    }

    static bool write(const QString &path, const QByteArray &data) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    }

    static bool qemu(const QStringList &args) {
        QProcess p;
        p.start("qemu-img", args);
        return p.waitForFinished(30000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    }

    struct Member {
        QByteArray name, data;
        int type = AE_IFREG;
        QByteArray link;
    };

    static bool tar(const QString &path, const QList<Member> &members) {
        archive *a = archive_write_new();
        archive_write_set_format_ustar(a);
        bool ok = archive_write_open_filename(a, QFile::encodeName(path).constData()) == ARCHIVE_OK;
        for (const auto &m : members) {
            archive_entry *e = archive_entry_new();
            archive_entry_set_pathname(e, m.name.constData());
            archive_entry_set_filetype(e, m.type);
            archive_entry_set_perm(e, 0600);
            if (!m.link.isEmpty()) {
                if (m.type == AE_IFLNK)
                    archive_entry_set_symlink(e, m.link.constData());
                else
                    archive_entry_set_hardlink(e, m.link.constData());
            }
            archive_entry_set_size(e, m.type == AE_IFREG && m.link.isEmpty() ? m.data.size() : 0);
            ok = ok && archive_write_header(a, e) == ARCHIVE_OK;
            if (!m.data.isEmpty()) ok = ok && archive_write_data(a, m.data.constData(), m.data.size()) == m.data.size();
            archive_entry_free(e);
        }
        ok = archive_write_close(a) == ARCHIVE_OK && ok;
        archive_write_free(a);
        return ok;
    }

    static QByteArray gzip(const QByteArray &data) {
        z_stream z{};
        if (deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
        QByteArray out(int(deflateBound(&z, uLong(data.size()))), '\0');
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
        z.avail_in = uInt(data.size());
        z.next_out = reinterpret_cast<Bytef *>(out.data());
        z.avail_out = uInt(out.size());
        const bool ok = deflate(&z, Z_FINISH) == Z_STREAM_END;
        out.resize(int(z.total_out));
        deflateEnd(&z);
        return ok ? out : QByteArray();
    }

    // The shape of a real ovftool export (REMnux): disk format, optional compression, hardware hints.
    static QByteArray ovf(const QByteArray &href = "disk.vmdk", const QByteArray &compression = {}) {
        return "<Envelope xmlns=\"http://schemas.dmtf.org/ovf/envelope/1\" "
               "xmlns:ovf=\"http://schemas.dmtf.org/ovf/envelope/1\" "
               "xmlns:rasd=\"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData\">"
               "<References><File ovf:id=\"f1\" ovf:href=\"" +
               href + "\"" + (compression.isEmpty() ? QByteArray() : " ovf:compression=\"" + compression + "\"") +
               "/></References>"
               "<DiskSection><Disk ovf:diskId=\"disk1\" ovf:fileRef=\"f1\" "
               "ovf:format=\"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized\"/></"
               "DiskSection>"
               "<VirtualSystem ovf:id=\"fixture\"><VirtualHardwareSection>"
               "<Item><rasd:ResourceType>3</rasd:ResourceType><rasd:VirtualQuantity>2</rasd:VirtualQuantity></Item>"
               "<Item><rasd:AllocationUnits>byte * "
               "2^20</rasd:AllocationUnits><rasd:ResourceType>4</rasd:ResourceType><rasd:VirtualQuantity>4096</"
               "rasd:VirtualQuantity></Item>"
               "</VirtualHardwareSection></VirtualSystem></Envelope>";
    }

    static QByteArray sha256(const QByteArray &data) {
        return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
    }

    // Rewrites a sparse VMDK's embedded descriptor in place, keeping the file layout.
    static QByteArray descriptor(QByteArray disk, const std::function<QString(QString)> &edit) {
        const auto offset = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(disk.constData() + 28)) * 512;
        const auto length = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(disk.constData() + 36)) * 512;
        const auto region = disk.mid(qsizetype(offset), qsizetype(length));
        auto text = edit(QString::fromLatin1(region.left(region.indexOf('\0')))).toLatin1();
        if (quint64(text.size()) > length) return {};
        text.append(QByteArray(qsizetype(length) - text.size(), '\0'));
        disk.replace(qsizetype(offset), qsizetype(length), text);
        return disk;
    }

    QTemporaryDir dir;
    QByteArray payload, vmdk;
private slots:

    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY2(!QStandardPaths::findExecutable("qemu-img").isEmpty(),
                "Real appliance fixtures require qemu-img (no mock/skip).");
        payload = QByteArray(1024 * 1024, '\0');
        const QByteArray marker("OmaWare appliance fixture");
        payload.replace(0, marker.size(), marker);
        QVERIFY(write(dir.filePath("payload.raw"), payload));
        QVERIFY(qemu({"convert", "-f", "raw", "-O", "vmdk", "-o", "subformat=streamOptimized",
                dir.filePath("payload.raw"), dir.filePath("payload.vmdk")}));
        vmdk = read(dir.filePath("payload.vmdk"));
        QVERIFY(vmdk.startsWith("KDMV"));
    }

    void validOva() {
        QTemporaryDir work;
        const auto ova = work.filePath("fixture.ova"), output = work.filePath("independent.qcow2");
        QVERIFY(tar(ova, {{"fixture.ovf", ovf()}, {"disk.vmdk", vmdk}}));
        const auto original = read(ova);
        ApplianceImport importer(work.path());
        QString error;
        QVERIFY2(importer.prepare(ova, ova, error), qPrintable(error));
        QCOMPARE(importer.capacity(), quint64(payload.size()));
        QVERIFY(importer.notes().join(' ').contains("2 vCPU(s) and 4096 MiB"));
        QVERIFY(importer.notes().join(' ').contains("Only the disk was imported"));
        QVERIFY(importer.notes().join(' ').contains("no manifest checksum"));
        // Conversion uses the staged content even if the original is rewritten after validation.
        QVERIFY(write(ova, "mutated original after preparation"));
        QVERIFY2(importer.convert(output, error), qPrintable(error));
        QVERIFY(qemu({"convert", "-f", "qcow2", "-O", "raw", output, work.filePath("roundtrip.raw")}));
        QCOMPARE(read(work.filePath("roundtrip.raw")), payload);
        QVERIFY(!importer.convert(output, error));
        QVERIFY(read(ova) != original);
        QVERIFY(qemu({"info", "--backing-chain", output}));
    }

    // REMnux ships "<name>-disk1.vmdk.gz" with ovf:compression="gzip", a SHA256 manifest, and an
    // "RDONLY ... SPARSE \"generated-stream.vmdk\"" extent that does not match the member name.
    void compressedOvaWithManifest() {
        QTemporaryDir work;
        const auto ova = work.filePath("remnux-fixture.ova"), output = work.filePath("out.qcow2");
        const auto disk = descriptor(vmdk, [](QString t) {
            return t.replace(
                    QRegularExpression("RW (\\d+) SPARSE \"[^\"]+\""), "RDONLY \\1 SPARSE \"generated-stream.vmdk\"");
        });
        QVERIFY(disk.contains("generated-stream.vmdk"));
        // Two concatenated gzip members are valid gzip and must both be unpacked.
        const auto half = disk.size() / 2;
        const auto compressed = gzip(disk.left(half)) + gzip(disk.mid(half));
        const auto description = ovf("fixture-disk1.vmdk.gz", "gzip");
        const QByteArray manifest = "SHA256(fixture.ovf)= " + sha256(description) +
                                    "\nSHA256(fixture-disk1.vmdk.gz)= " + sha256(compressed) + "\n";
        QVERIFY(tar(
                ova, {{"fixture.ovf", description}, {"fixture.mf", manifest}, {"fixture-disk1.vmdk.gz", compressed}}));
        ApplianceImport importer(work.path());
        QString error;
        QVERIFY2(importer.prepare(ova, ova, error), qPrintable(error));
        QVERIFY(importer.notes().join(' ').contains("matched the OVA's manifest"));
        QVERIFY2(importer.convert(output, error), qPrintable(error));
        QVERIFY(qemu({"compare", "-f", "raw", "-F", "qcow2", dir.filePath("payload.raw"), output}));
    }

    // VirtualBox writes the manifest after the disk, with SHA1 or SHA256; both orders and kinds must check.
    void manifestAfterDisk_data() {
        QTest::addColumn<QString>("algorithm");
        QTest::newRow("sha1") << "SHA1";
        QTest::newRow("sha256") << "SHA256";
        QTest::newRow("sha512") << "SHA512";
    }

    void manifestAfterDisk() {
        QFETCH(QString, algorithm);
        const auto kind = algorithm == "SHA1"     ? QCryptographicHash::Sha1
                          : algorithm == "SHA256" ? QCryptographicHash::Sha256
                                                  : QCryptographicHash::Sha512;
        QTemporaryDir work;
        const auto ova = work.filePath("vbox-fixture.ova"), output = work.filePath("out.qcow2");
        const auto description = ovf();
        const QByteArray manifest =
                algorithm.toUtf8() + "(x.ovf)= " + QCryptographicHash::hash(description, kind).toHex() + "\n" +
                algorithm.toUtf8() + "(disk.vmdk)= " + QCryptographicHash::hash(vmdk, kind).toHex() + "\n";
        QVERIFY(tar(ova, {{"x.ovf", description}, {"disk.vmdk", vmdk}, {"x.mf", manifest}}));
        ApplianceImport importer(work.path());
        QString error;
        QVERIFY2(importer.prepare(ova, ova, error), qPrintable(error));
        QVERIFY(importer.notes().join(' ').contains("matched the OVA's manifest"));
        QVERIFY2(importer.convert(output, error), qPrintable(error));
        // A wrong checksum after the disk is still caught.
        QByteArray wrong = manifest;
        wrong.replace(QCryptographicHash::hash(vmdk, kind).toHex(),
                QByteArray(int(QCryptographicHash::hashLength(kind)) * 2, '0'));
        const auto bad = work.filePath("bad.ova");
        QVERIFY(tar(bad, {{"x.ovf", description}, {"disk.vmdk", vmdk}, {"x.mf", wrong}}));
        ApplianceImport second(work.path());
        QVERIFY(!second.prepare(bad, bad, error));
        QVERIFY2(error.contains("does not match its manifest"), qPrintable(error));
    }

    // qemu reads an embedded-descriptor sparse VMDK from the file itself, so a hostile extent
    // name pointing at a host file must not leak it into the new disk.
    void hostileExtentNameReadsNothingElse() {
        QTemporaryDir work;
        const auto secret = work.filePath("host-secret.raw"), ova = work.filePath("x.ova"),
                   output = work.filePath("out.qcow2");
        QVERIFY(write(secret, QByteArray("HOST SECRET") + QByteArray(1024 * 1024 - 11, 's')));
        const auto disk = descriptor(vmdk, [&](QString t) {
            return t.replace(QRegularExpression("RW (\\d+) SPARSE \"[^\"]+\""), "RDONLY \\1 SPARSE \"" + secret + "\"");
        });
        QVERIFY(!disk.isEmpty());
        QVERIFY(tar(ova, {{"x.ovf", ovf()}, {"disk.vmdk", disk}}));
        ApplianceImport importer(work.path());
        QString error;
        QVERIFY2(importer.prepare(ova, ova, error), qPrintable(error));
        QVERIFY2(importer.convert(output, error), qPrintable(error));
        QVERIFY(qemu({"convert", "-f", "qcow2", "-O", "raw", output, work.filePath("roundtrip.raw")}));
        const auto copied = read(work.filePath("roundtrip.raw"));
        QCOMPARE(copied, payload);
        QVERIFY(!copied.contains("HOST SECRET"));
    }

    void unsafeOva_data() {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("reason");
        const QList<QPair<const char *, const char *>> rows{{"traversal", "Unsafe or unsupported OVA member"},
                {"absolute", "Unsafe or unsupported OVA member"}, {"symlink", "Unsafe or unsupported OVA member"},
                {"hardlink", "Unsafe or unsupported OVA member"}, {"special", "Unsafe or unsupported OVA member"},
                {"unexpected-member", "Unexpected file"}, {"duplicate", "Unsafe or unsupported OVA member"},
                {"external-reference", "unsafe or invalid file reference"},
                {"multi-system", "exactly one virtual machine"}, {"multi-disk", "more than one disk"},
                {"chunked", "split into chunks"}, {"unknown-compression", "unsupported disk compression"},
                {"ovf-not-first", "must start with its OVF"}, {"manifest-twice", "manifest must come once"},
                {"manifest-mismatch", "does not match its manifest"},
                {"manifest-unknown-file", "lists a file that is not in the OVA"},
                {"corrupt-gzip", "compressed disk is corrupt"}, {"truncated-gzip", "truncated"},
                {"descriptor-only-vmdk", "Unsupported VMDK"}, {"parent-hint", "Unsupported VMDK"},
                {"not-tar", "not a plain tar"}, {"missing-disk", "does not contain the disk"}};
        for (const auto &[name, reason] : rows)
            QTest::newRow(name) << QString(name) << QString(reason);
    }

    void unsafeOva() {
        QFETCH(QString, scenario);
        QFETCH(QString, reason);
        QTemporaryDir work;
        const auto ova = work.filePath("bad.ova");
        QByteArray description = ovf(), disk = vmdk;
        QList<Member> members;
        if (scenario == "external-reference") description.replace("\"disk.vmdk\"", "\"../disk.vmdk\"");
        if (scenario == "multi-system") description.replace("</Envelope>", "<VirtualSystem/></Envelope>");
        if (scenario == "multi-disk")
            description.replace("</DiskSection>",
                    "<Disk ovf:diskId=\"disk2\" ovf:fileRef=\"f1\" ovf:format=\"vmdk\"/></DiskSection>");
        if (scenario == "chunked")
            description.replace("ovf:href=\"disk.vmdk\"", "ovf:href=\"disk.vmdk\" ovf:chunkSize=\"1024\"");
        if (scenario == "unknown-compression")
            description.replace("ovf:href=\"disk.vmdk\"", "ovf:href=\"disk.vmdk\" ovf:compression=\"bzip2\"");
        if (scenario == "descriptor-only-vmdk")
            disk = "# Disk DescriptorFile\nversion=1\nCID=1\nparentCID=ffffffff\ncreateType=\"monolithicFlat\"\nRW "
                   "2048 FLAT \"/etc/passwd\" 0\n";
        if (scenario == "parent-hint")
            disk = descriptor(vmdk, [](QString t) { return t + "parentFileNameHint=\"/etc/passwd\"\n"; });
        if (scenario == "corrupt-gzip" || scenario == "truncated-gzip") {
            description = ovf("disk.vmdk.gz", "gzip");
            auto packed = gzip(vmdk);
            if (scenario == "corrupt-gzip")
                packed[packed.size() / 2] = char(packed[packed.size() / 2] ^ 0x55);
            else
                packed.chop(64);
            members << Member{"bad.ovf", description} << Member{"disk.vmdk.gz", packed};
        } else if (scenario == "ovf-not-first")
            members << Member{"disk.vmdk", disk} << Member{"bad.ovf", description};
        else if (scenario == "manifest-twice")
            members << Member{"bad.ovf", description} << Member{"bad.mf", "SHA256(disk.vmdk)= " + sha256(disk)}
                    << Member{"disk.vmdk", disk} << Member{"again.mf", "SHA256(disk.vmdk)= " + sha256(disk)};
        else if (scenario == "manifest-mismatch")
            members << Member{"bad.ovf", description} << Member{"bad.mf", "SHA256(disk.vmdk)= " + QByteArray(64, '0')}
                    << Member{"disk.vmdk", disk};
        else if (scenario == "manifest-unknown-file")
            members << Member{"bad.ovf", description} << Member{"bad.mf", "SHA256(other.vmdk)= " + sha256(disk)}
                    << Member{"disk.vmdk", disk};
        else if (scenario == "missing-disk")
            members << Member{"bad.ovf", description};
        else if (scenario != "not-tar")
            members << Member{"bad.ovf", description} << Member{"disk.vmdk", disk};
        if (scenario == "traversal") members << Member{"../escape.mf", "x"};
        if (scenario == "absolute") members << Member{QFile::encodeName(work.filePath("escape.mf")), "x"};
        if (scenario == "symlink") members << Member{"link.mf", {}, AE_IFLNK, "../escape"};
        if (scenario == "hardlink") members << Member{"link.mf", {}, AE_IFREG, "disk.vmdk"};
        if (scenario == "special") members << Member{"pipe.mf", {}, AE_IFIFO, {}};
        if (scenario == "unexpected-member") members << Member{"second.vmdk", disk};
        if (scenario == "duplicate") members << Member{"disk.vmdk", disk};
        if (scenario == "not-tar")
            QVERIFY(write(ova, gzip(vmdk)));
        else
            QVERIFY(tar(ova, members));
        ApplianceImport importer(work.path());
        QString error;
        QVERIFY2(!importer.prepare(ova, ova, error), qPrintable(scenario));
        QVERIFY2(error.contains(reason, Qt::CaseInsensitive), qPrintable(error));
        QVERIFY(!importer.convert(work.filePath("out.qcow2"), error));
        QVERIFY(!QFileInfo::exists(work.filePath("out.qcow2")));
        QVERIFY(!QFileInfo::exists(work.filePath("escape.mf")));
        QVERIFY(!QFileInfo::exists(work.filePath("escape")));
    }

    void stagingStaysUnderParentAndIsRemoved() {
        QTemporaryDir work;
        const auto ova = work.filePath("fixture.ova");
        QVERIFY(tar(ova, {{"f.ovf", ovf()}, {"disk.vmdk", vmdk}}));
        {
            ApplianceImport importer(work.path());
            QString error;
            QVERIFY2(importer.prepare(ova, ova, error), qPrintable(error));
            const auto staged = QDir(work.path()).entryInfoList({".omaware-import-*"}, QDir::Dirs | QDir::Hidden);
            QCOMPARE(staged.size(), 1);
            QCOMPARE(staged[0].permissions() &
                             (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther),
                    QFile::Permissions());
        }
        QVERIFY(QDir(work.path()).entryList({".omaware-import-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        ApplianceImport missing(work.filePath("absent"));
        QString error;
        QVERIFY(!missing.prepare(ova, ova, error));
    }

    void qcow2ImportAndBackingRejection() {
        QTemporaryDir work;
        const auto image = work.filePath("fixture.qcow2"), output = work.filePath("copy.qcow2");
        QVERIFY(qemu({"create", "-f", "qcow2", image, "1M"}));
        const auto original = read(image);
        ApplianceImport importer(work.path());
        QString error;
        QVERIFY2(importer.prepare(image, image, error), qPrintable(error));
        QVERIFY2(importer.convert(output, error), qPrintable(error));
        QCOMPARE(read(image), original);
        QVERIFY(qemu({"compare", "-f", "qcow2", "-F", "qcow2", image, output}));
        const auto overlay = work.filePath("overlay.qcow2");
        QVERIFY(qemu({"create", "-f", "qcow2", "-F", "qcow2", "-b", image, overlay}));
        ApplianceImport rejected(work.path());
        QVERIFY(!rejected.prepare(overlay, overlay, error));
        const auto fake = work.filePath("fake.qcow2");
        QVERIFY(write(fake, QByteArray(4096, 'r')));
        ApplianceImport notQcow(work.path());
        QVERIFY(!notQcow.prepare(fake, fake, error));
        QVERIFY(error.contains("not a QCOW2"));
        const auto link = work.filePath("link.qcow2");
        QVERIFY(QFile::link(image, link));
        ApplianceImport linked(work.path());
        QVERIFY(!linked.prepare(link, link, error));
    }

    void libraryDispatchAndIndependentCopy() {
        QTemporaryDir work;
        IsoLibrary library;
        library.setFolder(work.filePath("isos"));
        library.setApplianceFolder(work.filePath("appliances"));
        const auto image = work.filePath("remnux-7.qcow2"), iso = work.filePath("ubuntu.iso");
        QVERIFY(qemu({"create", "-f", "qcow2", image, "1M"}));
        QVERIFY(write(iso, "ISO fixture"));
        QSignalSpy imported(&library, &IsoLibrary::imported);
        QVERIFY(library.importFiles({image, iso}));
        QTRY_COMPARE(imported.size(), 1);
        QVERIFY(imported[0][1].toBool());
        QCOMPARE(library.files().size(), 2);
        bool diskFound = false, isoFound = false;
        for (const auto &v : library.files()) {
            const auto f = v.toMap();
            if (f["name"] == "remnux-7.qcow2") {
                diskFound = true;
                QCOMPARE(f["type"].toString(), QString("disk"));
                QCOMPARE(f["source"].toString(), QString("remnux"));
                QCOMPARE(f["path"].toString(), work.filePath("appliances/remnux-7.qcow2"));
                struct stat a{}, b{};
                QVERIFY(!::stat(QFile::encodeName(image).constData(), &a));
                QVERIFY(!::stat(QFile::encodeName(f["path"].toString()).constData(), &b));
                QVERIFY(a.st_ino != b.st_ino || a.st_dev != b.st_dev);
            } else {
                isoFound = true;
                QCOMPARE(f["type"].toString(), QString("iso"));
            }
        }
        QVERIFY(diskFound && isoFound);
        QCOMPARE(ApplianceImport::mediaType("appliance.OVA"), QString("disk"));
        QCOMPARE(ApplianceImport::mediaType(image), QString("disk"));
        QCOMPARE(ApplianceImport::mediaType(iso), QString("iso"));
        QVariantMap args{{"request_id", "11111111-1111-4111-8111-111111111111"}, {"name", "fixture"},
                {"media_kind", "disk"}, {"media", "fixture.ova"}};
        QString error;
        QVERIFY2(AgentProvision::validate("create_vm", args, error), qPrintable(error));
        args["media_kind"] = "iso";
        QVERIFY(!AgentProvision::validate("create_vm", args, error));
        QVERIFY(library.remove("remnux-7.qcow2"));
        QVERIFY(QFileInfo::exists(image));
        QVERIFY(!library.remove("../source.qcow2"));
    }

    // Opt-in: a real downloaded appliance (for example REMnux) is opened read-only and converted
    // into OMAWARE_REAL_APPLIANCE_WORK, which needs room for the unpacked disk and its copy.
    void realAppliance() {
        const auto source = qEnvironmentVariable("OMAWARE_REAL_APPLIANCE"),
                   work = qEnvironmentVariable("OMAWARE_REAL_APPLIANCE_WORK");
        if (source.isEmpty() || work.isEmpty())
            QSKIP("Set OMAWARE_REAL_APPLIANCE and OMAWARE_REAL_APPLIANCE_WORK to convert a real appliance.");
        const QFileInfo before(source);
        const auto output = work + "/real-appliance.qcow2";
        QVERIFY(!QFileInfo::exists(output));
        ApplianceImport importer(work);
        QString error;
        QVERIFY2(importer.prepare(source, source, error), qPrintable(error));
        qInfo().noquote() << "capacity" << importer.capacity() << "notes:" << importer.notes().join(" | ");
        QVERIFY2(importer.convert(output, error), qPrintable(error));
        const QFileInfo after(source);
        QCOMPARE(after.size(), before.size());
        QCOMPARE(after.lastModified(), before.lastModified());
        QProcess info;
        info.start("qemu-img", {"info", "--output=json", output});
        QVERIFY(info.waitForFinished(60000));
        const auto map = QJsonDocument::fromJson(info.readAllStandardOutput()).toVariant().toMap();
        QCOMPARE(map["format"].toString(), QString("qcow2"));
        QVERIFY(!map.contains("backing-filename"));
        QCOMPARE(map["virtual-size"].toULongLong(), importer.capacity());
    }

    // The REMnux catalog entry links to the publisher's page; it never invents a download or checksum.
    void remnuxCatalogEntry() {
        QTemporaryDir work;
        IsoLibrary library;
        library.setFolder(work.filePath("isos"));
        QVERIFY(library.sourceIds().contains("remnux"));
        QVariantMap info;
        for (const auto &v : library.sources())
            if (v.toMap()["id"] == "remnux") info = v.toMap();
        QCOMPARE(info["kind"].toString(), QString("page"));
        QCOMPARE(info["page"].toString(), QString("https://docs.remnux.org/install-distro/get-virtual-appliance"));
        QCOMPARE(library.identify("remnux-noble-amd64.ova")["source"].toString(), QString("remnux"));
        QCOMPARE(library.identify("remnux-noble-amd64.qcow2")["source"].toString(), QString("remnux"));
        QVERIFY(library.identify("remnux-noble-amd64.iso")["source"].toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ApplianceTests)
#include "test_applianceimport.moc"
