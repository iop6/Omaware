// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include "console.h"
#include "domainconfig.h"
#include "networkcatalog.h"
#include "configuration.h"
#include "workspace.h"
#include "diagnostics.h"
#include "snapshothistory.h"
#include "containment.h"
#include "instance.h"
#include "isolibrary.h"
#include "updater.h"
#include <memory>
#include <QDomDocument>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QtTest>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QLockFile>
#include <sys/socket.h>
#include <sys/stat.h>

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void isoReleaseLists() {
        // Ubuntu: the newest supported LTS from meta-release-lts, then the ISO from its SHA256SUMS.
        const QByteArray meta = "Dist: jammy\nName: Jammy Jellyfish\nVersion: 22.04.5 LTS\nSupported: 1\n\nDist: noble\nName: Noble Numbat\nVersion: 24.04.3 LTS\nSupported: 1\n\nDist: zesty\nSupported: 0\n";
        QCOMPARE(IsoLibrary::ubuntuLtsCodename(meta), QString("noble"));
        const QByteArray sums = QByteArray(64, 'a') + " *ubuntu-24.04.2-desktop-amd64.iso\n" + QByteArray(64, 'b') + " *ubuntu-24.04.10-desktop-amd64.iso\n"
            + QByteArray(64, 'c') + " *ubuntu-24.04.10-live-server-amd64.iso\n";
        auto desktop = IsoLibrary::fromChecksums(sums, "^ubuntu-([0-9][0-9.]*)-desktop-amd64\\.iso$", "https://releases.ubuntu.com/noble/");
        QCOMPARE(desktop.version, QString("24.04.10")); QCOMPARE(desktop.sha256, QString(64, 'b'));
        QCOMPARE(desktop.url, QString("https://releases.ubuntu.com/noble/ubuntu-24.04.10-desktop-amd64.iso"));
        // Fedora: the highest stable Workstation x86_64 ISO; betas and other variants are ignored.
        auto entry = [](QString version, QString arch, QString link, char hash, QString size) {
            return QJsonObject{{"version", version}, {"arch", arch}, {"variant", "Workstation"}, {"subvariant", "Workstation"}, {"link", link}, {"sha256", QString(64, hash)}, {"size", size}};
        };
        const auto fedora = QJsonDocument(QJsonArray{entry("42", "x86_64", "https://download.fedoraproject.org/a/Fedora-Workstation-Live-42-1.1.x86_64.iso", 'd', "2400000000"),
            entry("43 Beta", "x86_64", "https://x/beta.iso", 'e', "1"), entry("43", "aarch64", "https://x/arm.iso", 'f', "1")}).toJson();
        auto f = IsoLibrary::fedora(fedora, "Workstation");
        QCOMPARE(f.version, QString("42")); QCOMPARE(f.file, QString("Fedora-Workstation-Live-42-1.1.x86_64.iso")); QCOMPARE(f.size, qint64(2400000000));
        QVERIFY(IsoLibrary::newer("24.04.10", "24.04.9")); QVERIFY(!IsoLibrary::newer("24.04", "24.04.0"));
        // BSD-style checksum lists (Rocky, AlmaLinux, FreeBSD, OPNsense).
        const QByteArray bsd = "# Rocky-10.2-x86_64-boot.iso: 1049538560 bytes\nSHA256 (Rocky-10.2-x86_64-boot.iso) = " + QByteArray(64, 'a') + "\nSHA256 (Rocky-10.2-x86_64-dvd1.iso) = " + QByteArray(64, 'b') + "\n";
        auto rocky = IsoLibrary::fromChecksums(bsd, "^Rocky-([0-9][0-9.]*)-x86_64-boot\\.iso$", "https://download.rockylinux.org/pub/rocky/10/isos/x86_64/");
        QCOMPARE(rocky.version, QString("10.2")); QCOMPARE(rocky.sha256, QString(64, 'a'));
        // Directory listings: the newest numeric folder, not the last one alphabetically.
        const QByteArray listing = R"(<a href="9.8/">9.8/</a> <a href="10.2/">10.2/</a> <a href="10.10/">10.10/</a> <a href="Readme/">x</a> <a href="?C=N;O=D">)";
        QCOMPARE(IsoLibrary::newestFolder(listing), QString("10.10"));
        QCOMPARE(IsoLibrary::newestFolder("<a href=\"26.1.6/\"> <a href=\"26.7/\">", "^[0-9]+\\.[0-9]+$"), QString("26.7"));
        // Alpine's machine-readable release list.
        const QByteArray yaml = "---\n-\n  title: \"Virtual\"\n  flavor: alpine-virt\n  version: 3.24.2\n  iso: alpine-virt-3.24.2-x86_64.iso\n  sha256: " + QByteArray(64, 'c') + "\n  size: 72351744\n-\n  flavor: alpine-standard\n  iso: alpine-standard-3.24.2-x86_64.iso\n  sha256: " + QByteArray(64, 'd') + "\n";
        auto a = IsoLibrary::alpine(yaml, "alpine-virt", "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/");
        QCOMPARE(a.version, QString("3.24.2")); QCOMPARE(a.sha256, QString(64, 'c')); QCOMPARE(a.size, qint64(72351744));
        // Downloaded files are recognized and mapped to the closest OS preset.
        IsoLibrary library;
        QCOMPARE(library.identify("ubuntu-26.04.1-desktop-amd64.iso")["preset"].toString(), QString("ubuntu26.04"));
        QCOMPARE(library.identify("Rocky-10.2-x86_64-boot.iso")["preset"].toString(), QString("rocky10"));
        QCOMPARE(library.identify("alpine-virt-3.24.2-x86_64.iso")["preset"].toString(), QString("alpinelinux3.24"));
        QCOMPARE(library.identify("OPNsense-26.7-dvd-amd64.iso")["source"].toString(), QString("opnsense"));
        QVERIFY(library.identify("random.iso").isEmpty());
    }
    void windowsDownloads() {
        // Microsoft's page: the current version, its product edition and a checksum per language.
        const QByteArray page = "<p>(Current release: Windows 11 2026 Update l Version 26H2)</p>\n"
            "            <select><option value=\"\" selected>Select Download</option><option value=\"3813\">Windows 11 (multi-edition ISO for x64 devices)</option></select>\n"
            "            <table><tr><td>Arabic 64-bit</td><td>A75AE3F36CB9FFFFEDCB2D7DED9CEF83FFB710EB1376610D2220684D987CB9A5</td></tr>\n"
            "            <tr><td>English International 64-bit</td><td>1111111111111111111111111111111111111111111111111111111111111111</td></tr>\n"
            "            <tr><td>English 64-bit</td> <td>2222222222222222222222222222222222222222222222222222222222222222</td></tr>\n"
            "            <tr><td>Chinese Simplified 64-bit</td><td>3333333333333333333333333333333333333333333333333333333333333333</td></tr></table>";
        const auto windows = IsoLibrary::windowsPage(page);
        QCOMPARE(windows["version"].toString(), QString("26H2")); QCOMPARE(windows["edition"].toString(), QString("3813"));
        QCOMPARE(windows["languages"].toStringList(), QStringList({"Arabic", "English International", "English", "Chinese Simplified"}));
        QCOMPARE(windows["hashes"].toMap()["English"].toString(), QString(64, '2'));
        QVERIFY(IsoLibrary::windowsPage("<html>no download here</html>").isEmpty());
        // SKUs match by language name or display name.
        const QByteArray skus = "{\"Skus\":[{\"Id\":\"27160\",\"Language\":\"English\",\"LocalizedLanguage\":\"English (United States)\"},\n"
            "            {\"Id\":\"27161\",\"Language\":\"English (United Kingdom)\",\"LocalizedLanguage\":\"English International\"},\n"
            "            {\"Id\":\"27170\",\"Language\":\"Chinese (Simplified)\",\"LocalizedLanguage\":\"Chinese Simplified\"}]}";
        QCOMPARE(IsoLibrary::windowsSku(skus, "English"), QString("27160"));
        QCOMPARE(IsoLibrary::windowsSku(skus, "English International"), QString("27161"));
        QCOMPARE(IsoLibrary::windowsSku(skus, "Chinese Simplified"), QString("27170"));
        QVERIFY(IsoLibrary::windowsSku(skus, "Klingon").isEmpty());
        // The 64-bit link, or a plain explanation when Microsoft refuses.
        QString error;
        QCOMPARE(IsoLibrary::windowsLink("{\"ProductDownloadOptions\":[{\"Uri\":\"https://software.download.prss.microsoft.com/dbazure/Win11_26H2_English_x64.iso?t=abc\",\"DownloadType\":1}]}", error),
                 QString("https://software.download.prss.microsoft.com/dbazure/Win11_26H2_English_x64.iso?t=abc"));
        QVERIFY(IsoLibrary::windowsLink("{\"Errors\":[{\"Key\":\"ErrorSettings.SentinelReject\",\"Value\":\"Sentinel marked this request as rejected.\",\"Type\":8}]}", error).isEmpty());
        QVERIFY(error.contains("refused"));
        // The language that fits the computer's locale.
        const QStringList languages{"English", "English International", "German", "Brazilian Portuguese", "Portuguese", "Chinese Traditional", "Chinese Simplified"};
        QCOMPARE(IsoLibrary::windowsLanguage(QLocale("en_US"), languages), QString("English"));
        QCOMPARE(IsoLibrary::windowsLanguage(QLocale("en_GB"), languages), QString("English International"));
        QCOMPARE(IsoLibrary::windowsLanguage(QLocale("de_DE"), languages), QString("German"));
        QCOMPARE(IsoLibrary::windowsLanguage(QLocale("pt_BR"), languages), QString("Brazilian Portuguese"));
        QCOMPARE(IsoLibrary::windowsLanguage(QLocale("zh_TW"), languages), QString("Chinese Traditional"));
        QCOMPARE(IsoLibrary::windowsLanguage(QLocale("fi_FI"), languages), QString("English"));
        // Windows ISOs from OmaWare or Microsoft's website are recognized.
        IsoLibrary library;
        QCOMPARE(library.identify("Win11_26H2_English_x64.iso")["version"].toString(), QString("26H2"));
        QCOMPARE(library.identify("Win11_26H2_EnglishInternational_x64.iso")["preset"].toString(), QString("win11"));
        QCOMPARE(library.identify("Win11_24H2_English_x64v2.iso")["version"].toString(), QString("24H2"));
        QCOMPARE(library.identify("Windows11_Client_x64_en-us_26300_9457.iso")["version"].toString(), QString("26300.9457"));
    }
    void isoLatestOnline() {
        // Contacts the real publishers; opt in with OMAWARE_ONLINE_TEST=1. Nothing large is downloaded.
        if (qEnvironmentVariable("OMAWARE_ONLINE_TEST") != "1") QSKIP("Online release check: set OMAWARE_ONLINE_TEST=1 to contact the publishers.");
        QTemporaryDir dir; IsoLibrary library; library.setFolder(dir.path());
        library.check();
        QTRY_VERIFY_WITH_TIMEOUT(!library.checking(), 60000);
        QNetworkAccessManager network;
        for (const auto &value : library.sources()) {
            const auto source = value.toMap();
            if (source["kind"] != "download") continue;
            QVERIFY2(source["status"] == "ready", qPrintable(source["id"].toString() + ": " + source["error"].toString()));
            qInfo().noquote() << source["id"].toString() << source["version"].toString() << source["url"].toString();
            if (source["id"] == "windows-11") {
                // The full exchange with Microsoft, up to the start of the (7 GB) download, which is then cancelled.
                QVERIFY(!source["languages"].toStringList().isEmpty());
                QSignalSpy failed(&library, &IsoLibrary::finished);
                QVERIFY(library.download("windows-11"));
                QTRY_VERIFY_WITH_TIMEOUT(library.downloading() || !failed.isEmpty(), 60000);
                // Microsoft refuses some networks (and repeated requests) outright; that's its decision, not a bug here.
                if (!failed.isEmpty() && failed.last()[2].toString().contains("refused")) { qWarning().noquote() << "windows-11:" << failed.last()[2].toString(); continue; }
                QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.last()[2].toString()));
                QTRY_VERIFY_WITH_TIMEOUT([&] { for (const auto &v : library.sources()) if (v.toMap()["id"] == "windows-11") return v.toMap()["received"].toDouble() > 1048576; return false; }(), 60000);
                library.cancel("windows-11");
                QTRY_VERIFY(!library.downloading());
                continue;
            }
            // The published ISO really exists: ask for its headers only.
            QNetworkRequest request(QUrl(source["url"].toString()));
            request.setHeader(QNetworkRequest::UserAgentHeader, "OmaWare/test");   // as OmaWare's downloads do (some servers challenge browsers)
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            auto reply = network.head(request);
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 30000);
            QVERIFY2(reply->error() == QNetworkReply::NoError, qPrintable(source["url"].toString() + ": " + reply->errorString()));
            QVERIFY(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > 30 * 1024 * 1024);
            reply->deleteLater();
        }
        // Optionally download some for real and verify them: OMAWARE_ONLINE_DOWNLOAD=1 means Debian
        // (about 750 MB), or list source ids, e.g. "alpine,opnsense".
        const auto wanted = qEnvironmentVariable("OMAWARE_ONLINE_DOWNLOAD");
        const auto ids = wanted == "1" ? QStringList{"debian"} : wanted.split(',', Qt::SkipEmptyParts);
        for (const auto &id : ids) {
            QSignalSpy finished(&library, &IsoLibrary::finished);
            QVERIFY2(library.download(id), qPrintable(id));
            QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 7200000);
            QVERIFY2(finished.last()[1].toBool(), qPrintable(id + ": " + finished.last()[2].toString()));
            for (const auto &value : library.sources()) if (value.toMap()["id"] == id) QVERIFY2(value.toMap()["upToDate"].toBool(), qPrintable(id));
            qInfo().noquote() << id << "downloaded and verified:" << finished.last()[2].toString();
        }
    }
    void isoImports() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QDir().mkpath(dir.filePath("elsewhere"));
        const auto make = [&](const QString &name, const QByteArray &body) { QFile f(dir.filePath("elsewhere/" + name)); f.open(QIODevice::WriteOnly); f.write(body); return f.fileName(); };
        const auto ubuntu = make("ubuntu-24.04.3-desktop-amd64.iso", QByteArray(1024, 'u'));
        const auto other = make("Other.ISO", QByteArray(2048, 'o'));
        const auto text = make("notes.txt", "not an ISO");
        IsoLibrary library; library.setFolder(dir.filePath("isos"));
        QSignalSpy imported(&library, &IsoLibrary::imported);
        // Files on the same disk are linked: instant, the original stays where it was.
        QVERIFY(library.importFiles({QUrl::fromLocalFile(ubuntu).toString(), other, text}));
        QTRY_COMPARE(imported.size(), 1);
        auto result = imported.takeFirst();
        QVERIFY(result[1].toBool());
        QCOMPARE(result[0].toStringList(), QStringList({dir.filePath("isos/ubuntu-24.04.3-desktop-amd64.iso"), dir.filePath("isos/Other.ISO")}));
        QVERIFY(result[2].toString().contains("Added 2 ISOs.")); QVERIFY(result[2].toString().contains("notes.txt was skipped"));
        QVERIFY(QFile::exists(ubuntu)); QCOMPARE(library.files().size(), 2); QVERIFY(!library.importing());
        // Adding the same file again doesn't make a second copy; a different file with the same name gets a new name.
        QVERIFY(library.importFiles({ubuntu}));
        QTRY_COMPARE(imported.size(), 1); result = imported.takeFirst();
        QVERIFY(result[1].toBool()); QVERIFY(result[2].toString().contains("already in your ISOs")); QCOMPARE(library.files().size(), 2);
        QDir().mkpath(dir.filePath("second"));
        { QFile f(dir.filePath("second/Other.ISO")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(3000, 'x')); }
        QVERIFY(library.importFiles({dir.filePath("second/Other.ISO")}));
        QTRY_COMPARE(imported.size(), 1); result = imported.takeFirst();
        QCOMPARE(result[0].toStringList(), QStringList({dir.filePath("isos/Other (2).ISO")}));
        // Nothing usable: refused with a message.
        QVERIFY(!library.importFiles({text}));
        QTRY_COMPARE(imported.size(), 1); result = imported.takeFirst();
        QVERIFY(!result[1].toBool()); QVERIFY(result[2].toString().contains("Only .iso files"));
        // Another disk (tmpfs) can't be linked, so the file is copied in the background.
        QTemporaryDir shm("/dev/shm/omaware-test-XXXXXX");
        struct stat a {}, b {};
        if (!shm.isValid() || ::stat(qPrintable(shm.path()), &a) != 0 || ::stat(qPrintable(dir.path()), &b) != 0 || a.st_dev == b.st_dev) QSKIP("No second filesystem to copy from.");
        QFile big(shm.filePath("rocky-copy.iso")); QVERIFY(big.open(QIODevice::WriteOnly));
        QByteArray chunk(1 << 20, 'r'); for (int i = 0; i < 12; ++i) big.write(chunk); big.close();
        QVERIFY(library.importFiles({big.fileName()}));
        QVERIFY(library.importing()); QCOMPARE(library.importName(), QString("rocky-copy.iso"));
        QTRY_COMPARE(imported.size(), 1); result = imported.takeFirst();
        QVERIFY2(result[1].toBool(), qPrintable(result[2].toString()));
        QFile copy(dir.filePath("isos/rocky-copy.iso")); QVERIFY(copy.open(QIODevice::ReadOnly)); QCOMPARE(copy.size(), qint64(12 << 20));
        QVERIFY(!QFile::exists(dir.filePath("isos/rocky-copy.iso.part"))); QVERIFY(QFile::exists(big.fileName()));
        // A cancelled copy leaves nothing behind.
        QFile huge(shm.filePath("huge.iso")); QVERIFY(huge.open(QIODevice::WriteOnly));
        for (int i = 0; i < 160; ++i) huge.write(chunk);
        huge.close();
        QVERIFY(library.importFiles({huge.fileName()})); library.cancelImport();
        QTRY_COMPARE(imported.size(), 1); result = imported.takeFirst();
        QVERIFY(!result[1].toBool()); QVERIFY(result[2].toString().contains("cancelled"));
        QVERIFY(!QFile::exists(dir.filePath("isos/huge.iso"))); QVERIFY(!QFile::exists(dir.filePath("isos/huge.iso.part")));
    }
    void isoDownloadResumes() {
        qputenv("OMAWARE_STALL_SECONDS", "1");
        // A server that drops the connection halfway; the download continues from there and is still verified.
        const QByteArray body = QByteArray(3 * 1024 * 1024, 'r') + "end";
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> ranges; bool stall = false; QList<QTcpSocket *> quiet;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const auto head = socket->readAll();
                const auto range = QRegularExpression("Range: bytes=([0-9]+)-").match(QString::fromLatin1(head));
                ranges << (range.hasMatch() ? range.captured(1).toLatin1() : QByteArray("full"));
                if (!range.hasMatch()) {
                    // First request: promise the whole file, send half of it, then hang up.
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n" + body.left(body.size() / 2));
                    socket->flush(); socket->waitForBytesWritten(2000);
                    if (stall) quiet << socket; else socket->abort();
                    return;
                }
                const qint64 from = range.captured(1).toLongLong();
                socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: " + QByteArray::number(body.size() - from) + "\r\nContent-Range: bytes " + QByteArray::number(from) + "-" + QByteArray::number(body.size() - 1) + "/" + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body.mid(from));
                socket->disconnectFromHost();
            });
        });
        QTemporaryDir dir; IsoLibrary library; library.setFolder(dir.path());
        QSignalSpy finished(&library, &IsoLibrary::finished);
        QVERIFY(library.fetch("debian", QUrl(QString("http://127.0.0.1:%1/debian-13.1.0-amd64-netinst.iso").arg(server.serverPort())), sha, "debian-13.1.0-amd64-netinst.iso", body.size()));
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 20000);
        QVERIFY2(finished.last()[1].toBool(), qPrintable(finished.last()[2].toString()));
        QCOMPARE(ranges.size(), 2); QCOMPARE(ranges.first(), QByteArray("full")); QVERIFY(ranges.last().toLongLong() > 0);
        QFile result(dir.filePath("debian-13.1.0-amd64-netinst.iso")); QVERIFY(result.open(QIODevice::ReadOnly)); QCOMPARE(result.readAll(), body);
        // A server that goes quiet without hanging up is noticed (here after a second) and resumed the same way.
        stall = true; ranges.clear(); finished.clear(); QFile::remove(result.fileName());
        QVERIFY(library.fetch("debian", QUrl(QString("http://127.0.0.1:%1/debian-13.1.0-amd64-netinst.iso").arg(server.serverPort())), sha, "debian-13.1.0-amd64-netinst.iso", body.size()));
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 30000);
        QVERIFY2(finished.last()[1].toBool(), qPrintable(finished.last()[2].toString()));
        QCOMPARE(ranges.size(), 2); QVERIFY(ranges.last().toLongLong() > 0);
        QFile again(result.fileName()); QVERIFY(again.open(QIODevice::ReadOnly)); QCOMPARE(again.readAll(), body);
    }
    void isoDownloads() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QFile source(dir.filePath("source.bin")); QVERIFY(source.open(QIODevice::WriteOnly)); const QByteArray body(3 * 1024 * 1024 + 7, 'x'); source.write(body); source.close();
        const auto good = QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
        IsoLibrary library; library.setFolder(dir.filePath("isos"));
        QSignalSpy finished(&library, &IsoLibrary::finished);
        // A verified download lands in the folder and is recognized.
        QVERIFY(library.fetch("debian", QUrl::fromLocalFile(source.fileName()), good, "debian-13.1.0-amd64-netinst.iso"));
        QTRY_COMPARE(finished.count(), 1); QVERIFY2(finished.last()[1].toBool(), qPrintable(finished.last()[2].toString()));
        QVERIFY(QFile::exists(dir.filePath("isos/debian-13.1.0-amd64-netinst.iso")));
        QCOMPARE(library.files().size(), 1); QCOMPARE(library.files().first().toMap()["version"].toString(), QString("13.1.0"));
        // A checksum mismatch keeps nothing.
        QVERIFY(library.fetch("debian", QUrl::fromLocalFile(source.fileName()), QString(64, '0'), "debian-13.2.0-amd64-netinst.iso"));
        QTRY_COMPARE(finished.count(), 2); QVERIFY(!finished.last()[1].toBool());
        QVERIFY(!QFile::exists(dir.filePath("isos/debian-13.2.0-amd64-netinst.iso"))); QVERIFY(!QFile::exists(dir.filePath("isos/debian-13.2.0-amd64-netinst.iso.part")));
        // Names must be plain .iso files inside the folder.
        QVERIFY(!library.fetch("debian", QUrl::fromLocalFile(source.fileName()), good, "../escape.iso"));
        QVERIFY(!library.remove("../source.bin")); QVERIFY(library.remove("debian-13.1.0-amd64-netinst.iso")); QVERIFY(library.files().isEmpty());
        // Several at once; anything that isn't a plain ISO in the folder is skipped.
        for (auto name : {"a.iso", "b.iso"}) { QFile f(dir.filePath(QString("isos/") + name)); QVERIFY(f.open(QIODevice::WriteOnly)); }
        library.rescan(); QCOMPARE(library.removeAll({"a.iso", "b.iso", "../source.bin", "missing.iso"}), 2); QVERIFY(library.files().isEmpty());
        // A compressed image is verified first, then unpacked into a plain ISO.
        QProcess bzip; bzip.start("bzip2", {"-kf", source.fileName()}); QVERIFY(bzip.waitForFinished()); QCOMPARE(bzip.exitCode(), 0);
        QFile packed(source.fileName() + ".bz2"); QVERIFY(packed.open(QIODevice::ReadOnly));
        const auto packedHash = QString::fromLatin1(QCryptographicHash::hash(packed.readAll(), QCryptographicHash::Sha256).toHex()); packed.close();
        QVERIFY(library.fetch("opnsense", QUrl::fromLocalFile(packed.fileName()), packedHash, "OPNsense-26.7-dvd-amd64.iso.bz2"));
        // (The refused "../escape.iso" above also reported a failure, so this is the fourth result.)
        QTRY_COMPARE(finished.count(), 4); QVERIFY2(finished.last()[1].toBool(), qPrintable(finished.last()[2].toString()));
        QFile unpacked(dir.filePath("isos/OPNsense-26.7-dvd-amd64.iso")); QVERIFY(unpacked.open(QIODevice::ReadOnly)); QCOMPARE(unpacked.readAll(), body);
        QVERIFY(!QFile::exists(dir.filePath("isos/OPNsense-26.7-dvd-amd64.iso.bz2")));
        QCOMPARE(library.files().first().toMap()["source"].toString(), QString("opnsense"));
    }
    void updaterVersions() {
        QVERIFY(Updater::newer("1.0.1", "1.0.0")); QVERIFY(Updater::newer("1.10.0", "1.9.3")); QVERIFY(!Updater::newer("1.0.0", "1.0.0"));
        QVERIFY(Updater::newer("1.1.0", "1.1.0-dev")); QVERIFY(Updater::newer("1.1.0-dev", "1.0.0")); QVERIFY(!Updater::newer("1.0.0", "1.1.0-dev"));
        const auto release = Updater::parseRelease(QJsonDocument(QJsonObject{{"tag_name", "v1.2.0"}, {"html_url", "https://github.com/iop6/Omaware/releases/tag/v1.2.0"}, {"body", "Notes"},
            {"assets", QJsonArray{QJsonObject{{"name", "omaware-1.2.0-linux-x86_64.tar.gz"}, {"browser_download_url", "https://example/pkg"}, {"size", 1000}},
                                  QJsonObject{{"name", "SHA256SUMS"}, {"browser_download_url", "https://example/sums"}}}}}).toJson());
        QCOMPARE(release["version"].toString(), QString("1.2.0")); QCOMPARE(release["packageUrl"].toString(), QString("https://example/pkg")); QCOMPARE(release["sumsUrl"].toString(), QString("https://example/sums"));
        QVERIFY(Updater::parseRelease(QJsonDocument(QJsonObject{{"tag_name", "nightly"}}).toJson()).isEmpty());
    }
    void updaterInstallsVerifiedPackage() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        auto write = [](const QString &path, const QByteArray &data, bool executable = false) {
            QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); f.open(QIODevice::WriteOnly); f.write(data); f.close();
            if (executable) f.setPermissions(f.permissions() | QFile::ExeOwner);
        };
        auto sums = [](const QString &root, const QStringList &files) {
            QByteArray out;
            for (const auto &name : files) { QFile f(root + "/" + name); f.open(QIODevice::ReadOnly); out += QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex() + "  " + name.toUtf8() + "\n"; }
            QFile list(root + "/SHA256SUMS"); list.open(QIODevice::WriteOnly); list.write(out);
        };
        // The installed copy: a self-contained app folder.
        const auto app = dir.filePath("app");
        write(app + "/omaware", "old", true); sums(app, {"omaware"});
        // The new release, packaged like a GitHub Release.
        const auto stage = dir.filePath("build/omaware-1.0.1-linux-x86_64");
        write(stage + "/omaware", "new", true); write(stage + "/lib/libvncclient.so.0.9.15", "lib"); write(stage + "/omaware.sh", "#!/bin/sh\n", true);
        sums(stage, {"omaware", "lib/libvncclient.so.0.9.15", "omaware.sh"});
        QProcess tar; tar.start("tar", {"-czf", dir.filePath("omaware-1.0.1-linux-x86_64.tar.gz"), "-C", dir.filePath("build"), "omaware-1.0.1-linux-x86_64"}); QVERIFY(tar.waitForFinished()); QCOMPARE(tar.exitCode(), 0);
        auto publish = [&](const QByteArray &packageHash) {
            write(dir.filePath("SHA256SUMS"), packageHash + "  omaware-1.0.1-linux-x86_64.tar.gz\n");
            write(dir.filePath("release.json"), QJsonDocument(QJsonObject{{"tag_name", "v1.0.1"}, {"html_url", "https://example/release"},
                {"assets", QJsonArray{QJsonObject{{"name", "omaware-1.0.1-linux-x86_64.tar.gz"}, {"browser_download_url", QUrl::fromLocalFile(dir.filePath("omaware-1.0.1-linux-x86_64.tar.gz")).toString()}},
                                      QJsonObject{{"name", "SHA256SUMS"}, {"browser_download_url", QUrl::fromLocalFile(dir.filePath("SHA256SUMS")).toString()}}}}}).toJson());
        };
        Updater updater; updater.setAppDir(app); updater.setCurrent("1.0.0"); updater.setFeed(QUrl::fromLocalFile(dir.filePath("release.json")));
        QVERIFY(updater.canInstall());
        // A package that doesn't match its published checksum is refused and nothing changes.
        publish(QByteArray(64, '0'));
        updater.check(); QTRY_COMPARE(updater.status(), QString("available")); QCOMPARE(updater.latest(), QString("1.0.1"));
        updater.download(); QTRY_COMPARE(updater.status(), QString("error")); QVERIFY(!QDir(app + ".update").exists());
        QFile old(app + "/omaware"); QVERIFY(old.open(QIODevice::ReadOnly)); QCOMPARE(old.readAll(), QByteArray("old")); old.close();
        // The real one is downloaded, checked, unpacked and swapped in; the old copy is kept.
        QFile package(dir.filePath("omaware-1.0.1-linux-x86_64.tar.gz")); QVERIFY(package.open(QIODevice::ReadOnly));
        publish(QCryptographicHash::hash(package.readAll(), QCryptographicHash::Sha256).toHex());
        updater.check(); QTRY_COMPARE(updater.status(), QString("available"));
        updater.download(); QTRY_COMPARE_WITH_TIMEOUT(updater.status(), QString("ready"), 10000);
        QVERIFY(updater.install()); QCOMPARE(updater.status(), QString("installed"));
        QFile now(app + "/omaware"); QVERIFY(now.open(QIODevice::ReadOnly)); QCOMPARE(now.readAll(), QByteArray("new"));
        QFile version(app + "/VERSION"); QVERIFY(version.open(QIODevice::ReadOnly)); QCOMPARE(version.readAll().trimmed(), QByteArray("1.0.1"));
        QVERIFY(QFile::exists(app + ".previous/omaware")); QVERIFY(!QDir(app + ".update").exists());
        QVERIFY(Updater::mismatches(app).isEmpty());
        // Development builds and copies that aren't a self-contained folder never update themselves.
        Updater dev; dev.setAppDir(app); dev.setCurrent("1.1.0-dev"); QVERIFY(!dev.canInstall());
        Updater loose; loose.setAppDir(dir.filePath("build")); loose.setCurrent("1.0.0"); QVERIFY(!loose.canInstall());
    }
    void singleInstance() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("omaware.lock");
        QVERIFY(InstanceGuard::otherProcesses("omaware-no-such-program").isEmpty());
        auto first = std::make_unique<InstanceGuard>(path);
        QVERIFY(first->acquire());
        InstanceGuard second(path);
        QVERIFY(!second.acquire());
        QVERIFY(second.blocker().contains("already open"));
        first.reset();
        InstanceGuard third(path);
        QVERIFY(third.acquire());
    }
    void themeDefaultsWithoutOmarchy() {
        // Other distributions have no Omarchy palette; the UI then defaults to the built-in dark theme.
        Theme missing("/nonexistent/omarchy/colors.toml"); QVERIFY(!missing.omarchyAvailable());
        QTemporaryDir dir; QVERIFY(dir.isValid());
        { QFile f(dir.filePath("colors.toml")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("background='#000000'\nforeground='#ffffff'\naccent='#00ff00'"); }
        Theme present(dir.filePath("colors.toml")); QVERIFY(present.omarchyAvailable());
    }
    void containmentPolicy() {
        const QHash<QString, QVariantMap> bridges{{"omaiso", QVariantMap{{"isolated", true}, {"name", "omaware-lab"}}}, {"omaleak", QVariantMap{{"isolated", false}, {"name", "omaware-leaky"}}}};
        auto domain = [](QString devices) { return "<domain type='kvm'><name>omaware-x</name><devices>" + devices + "</devices></domain>"; };
        auto violations = [&](QString devices) { return Containment::check(domain(devices), bridges)["violations"].toStringList(); };
        QVERIFY(violations("").isEmpty());
        QVERIFY(violations("<interface type='bridge'><mac address='52:54:00:00:00:01'/><source bridge='omaiso'/></interface>").isEmpty());
        QCOMPARE(violations("<interface type='user'><mac address='52:54:00:00:00:02'/></interface>").size(), 1);
        QCOMPARE(violations("<interface type='bridge'><source bridge='omaleak'/></interface>").size(), 1);
        QCOMPARE(violations("<interface type='bridge'><source bridge='virbr0'/></interface>").size(), 1);
        QCOMPARE(violations("<interface type='direct'><source dev='eth0' mode='bridge'/></interface>").size(), 1);
        QCOMPARE(violations("<filesystem type='mount'><source dir='/home'/><target dir='home'/></filesystem>").size(), 1);
        QCOMPARE(violations("<hostdev mode='subsystem' type='usb'/>").size(), 1);
        QCOMPARE(violations("<redirdev bus='usb' type='spicevmc'/>").size(), 1);
        QCOMPARE(violations("<channel type='qemu-vdagent'><source><clipboard copypaste='yes'/></source></channel>").size(), 1);
        QVERIFY(violations("<channel type='qemu-vdagent'><source><clipboard copypaste='no'/></source></channel>").isEmpty());
        QVERIFY(violations("<graphics type='vnc'><listen type='none'/></graphics>").isEmpty());
        QCOMPARE(violations("<graphics type='vnc' port='5900'><listen type='address' address='0.0.0.0'/></graphics>").size(), 1);
        QCOMPARE(violations("<serial type='tcp'><source mode='bind' host='0.0.0.0' service='4444'/></serial>").size(), 1);
        const auto agent = Containment::check(domain("<channel type='unix'><target type='virtio' name='org.qemu.guest_agent.0'/></channel>"), bridges);
        QVERIFY(agent["violations"].toStringList().isEmpty()); QCOMPARE(agent["warnings"].toStringList().size(), 1);
        // The marker round-trips through a full definition and can be removed again.
        const auto marked = Containment::withMarker(domain(""), true);
        QVERIFY(Containment::enabled(marked)); QVERIFY(!Containment::enabled(domain("")));
        QVERIFY(Containment::enabled(Containment::withMarker(marked, true)));
        QCOMPARE(QDomDocument().setContent(marked) ? 1 : 0, 1);
        QVERIFY(!Containment::enabled(Containment::withMarker(marked, false)));
    }
    void isolatedSwitchVerification() {
        // Generated isolated switches never give the host IPv6; NAT/host-only keep guest IPv6.
        QString error;
        const auto isolated = Configuration::networkXml({{"name", "lab"}, {"mode", "isolated"}}, QUuid::createUuid().toString(QUuid::WithoutBraces), "omatest1", error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QDomDocument doc; QVERIFY(doc.setContent(isolated)); QVERIFY(!doc.documentElement().hasAttribute("ipv6"));
        QVERIFY(doc.documentElement().firstChildElement("forward").isNull()); QVERIFY(doc.documentElement().firstChildElement("ip").isNull());
        const auto nat = Configuration::networkXml({{"name", "web"}, {"mode", "nat"}, {"subnet", "192.168.241.0/24"}, {"dhcp", false}}, QUuid::createUuid().toString(QUuid::WithoutBraces), "omatest2", error);
        QDomDocument natDoc; QVERIFY(natDoc.setContent(nat)); QCOMPARE(natDoc.documentElement().attribute("ipv6"), "yes");
        // Live checks against a fake /sys and /proc tree.
        QTemporaryDir root; QVERIFY(root.isValid());
        auto touch = [&](QString path, QByteArray data = {}) { QVERIFY(QDir().mkpath(QFileInfo(root.filePath(path)).absolutePath())); QFile f(root.filePath(path)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); };
        QVERIFY(QDir().mkpath(root.filePath("sys/class/net/omatest1/bridge")));
        QVERIFY(QDir().mkpath(root.filePath("sys/class/net/omatest1/brif/vnet7")));
        touch("sys/class/net/vnet7/tun_flags", "0x1002\n");
        touch("proc/sys/net/ipv6/conf/omatest1/disable_ipv6", "1\n");
        auto verdict = Containment::verifyNetwork(isolated, true, root.path());
        QVERIFY2(verdict["isolated"].toBool(), qPrintable(QJsonDocument::fromVariant(verdict).toJson()));
        QVERIFY(!Containment::verifyNetwork(isolated, false, root.path())["isolated"].toBool());
        touch("proc/sys/net/ipv6/conf/omatest1/disable_ipv6", "0\n");
        QVERIFY(!Containment::verifyNetwork(isolated, true, root.path())["isolated"].toBool());
        touch("proc/sys/net/ipv6/conf/omatest1/disable_ipv6", "1\n");
        QVERIFY(QDir().mkpath(root.filePath("sys/class/net/omatest1/brif/eth0"))); // a physical NIC joined to the switch
        QVERIFY(!Containment::verifyNetwork(isolated, true, root.path())["isolated"].toBool());
        QVERIFY(QDir(root.filePath("sys/class/net/omatest1/brif/eth0")).removeRecursively());
        QVERIFY(Containment::verifyNetwork(isolated, true, root.path())["isolated"].toBool());
        auto leaky = isolated; leaky.replace("<network", "<network ipv6=\"yes\"");
        QVERIFY(!Containment::verifyNetwork(leaky, true, root.path())["isolated"].toBool());
        auto routed = isolated; routed.replace("</network>", "<forward mode=\"nat\"/></network>");
        QVERIFY(!Containment::verifyNetwork(routed, true, root.path())["isolated"].toBool());
    }
    void isoLibraryAndPersistentHistory() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        for (const auto &name : {"Ubuntu.iso", "Recovery.ISO", "notes.txt"}) {
            QFile file(directory.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture");
        }
        QVERIFY(QDir().mkdir(directory.filePath("folder.iso")));
        auto catalog = Workspace::mediaFiles(directory.path());
        QCOMPARE(catalog["items"].toList().size(), 2);
        for (const auto &value : catalog["items"].toList()) { QVERIFY(QFileInfo(value.toMap()["path"].toString()).isFile()); QCOMPARE(value.toMap()["bytes"].toLongLong(), 7); }
        QVERIFY(Workspace::mediaFiles(directory.filePath("missing"))["error"].isValid());
        QVERIFY(Workspace::mediaFiles("relative")["error"].isValid());
        const auto logPath = directory.filePath("activity.json"); ActivityLog first(logPath); QVariantList entries; QString error;
        QVERIFY(first.append({{"message", "Saved snapshot"}, {"ok", true}}, entries, error));
        ActivityLog reopened(logPath); QCOMPARE(reopened.load().first().toMap()["message"].toString(), "Saved snapshot");
        QVERIFY(!(QFileInfo(logPath).permissions() & (QFile::ReadGroup | QFile::ReadOther)));
        QVERIFY(reopened.append({{"message", "Missing disk file"}, {"ok", false}}, entries, error));
        QCOMPARE(first.load().size(), 2);
        QLockFile competing(logPath + ".lock"); QVERIFY(competing.tryLock());
        QVERIFY(!first.append({{"message", "Must not overwrite"}}, entries, error)); QCOMPARE(reopened.load().size(), 2); competing.unlock();
        for (int i = 0; i < 205; ++i) QVERIFY(first.append({{"message", QString("Operation %1").arg(i)}}, entries, error));
        QCOMPARE(reopened.load().size(), 200); QCOMPARE(reopened.load().first().toMap()["message"].toString(), "Operation 204");
        QVERIFY(reopened.clear(error)); QVERIFY(first.load().isEmpty());
        QCOMPARE(Diagnostics::advice("Not enough free space")["action"].toString(), "storage");
        QCOMPARE(Diagnostics::advice("Missing source file")["action"].toString(), "source");
        Theme theme("/missing/theme"); theme.setTextScale(2); QCOMPARE(theme.textScale(), 1.3);
        theme.setTextScale(0); QCOMPARE(theme.textScale(), 1.0); theme.setReducedMotion(true); QVERIFY(theme.reducedMotion());
    }
    void snapshotTreeHistory() {
        const QVariantMap oldLabels{{"branches", QVariantList{QVariantMap{{"id", "old"}, {"name", "Experiment"}, {"baseId", "B"}}}}, {"branchId", "old"}};
        QVariantList items; QVariantMap state = oldLabels; QString error;
        auto capture = [&](QString id) {
            QVariantMap item{{"id", id}, {"name", id}, {"parentId", state["id"]}, {"parent", state["name"]}};
            state = SnapshotHistory::captured(state, item); items.append(item);
        };
        capture("A"); capture("B");
        state = SnapshotHistory::restored(items, state, "A", error); QVERIFY(!state.isEmpty());
        capture("C");
        QCOMPARE(items[1].toMap()["parentId"].toString(), "A");
        QCOMPARE(items[2].toMap()["parentId"].toString(), "A");
        QCOMPARE(SnapshotHistory::view(items, state)["currentId"].toString(), "C");
        QCOMPARE(SnapshotHistory::view(items, state)["items"].toList(), items);
        QVERIFY(!SnapshotHistory::view(items, state).contains("branches"));
        QCOMPARE(state["branches"], oldLabels["branches"]); // old labels preserved but inert
        QVERIFY(SnapshotHistory::restored(items, state, "missing", error).isEmpty());
        state = SnapshotHistory::restored(items, state, "B", error);
        capture("D"); QCOMPARE(items.last().toMap()["parentId"].toString(), "B");
        SnapshotHistory::removed(state, items.last().toMap());
        QCOMPARE(state["id"].toString(), "B");
        SnapshotHistory::removed(state, items[2].toMap());
        QCOMPARE(state["id"].toString(), "B"); // removing another leaf does not move current state
        SnapshotHistory::removed(state, items[1].toMap());
        QCOMPARE(SnapshotHistory::survivingParent(state,"D"),QString("A"));
        QCOMPARE(SnapshotHistory::survivingParent(state,"C"),QString("A"));
        SnapshotHistory::removed(state, items[0].toMap());
        QVERIFY(SnapshotHistory::survivingParent(state,"D").isEmpty());
        QVERIFY(SnapshotHistory::survivingParent({{"deletedSnapshots",QVariantMap{{"cycle",QVariantMap{{"parentId","cycle"}}}}}},"cycle").isEmpty());
        QVERIFY(SnapshotHistory::view(items, {{"id", "missing"}})["currentId"].toString().isEmpty());
    }
    void hardwareAndPendingChanges() {
        const QString xml = R"(<domain type='kvm'><name>fixture</name><uuid>87ba849e-e633-4ab2-b13c-25ba3c071912</uuid><memory unit='MiB'>512</memory><currentMemory unit='MiB'>512</currentMemory><vcpu>1</vcpu><os><type arch='x86_64'>hvm</type><boot dev='hd'/></os><devices><disk type='file' device='disk'><driver name='qemu' type='qcow2'/><source file='/untouched/system.qcow2'/><target dev='vda' bus='virtio'/><boot order='1'/></disk><interface type='user'><mac address='52:54:00:11:22:33'/><model type='virtio'/></interface></devices></domain>)";
        QString error;
        auto changed = Configuration::hardware(xml, {{"cpus", 2}, {"memoryMiB", 1024}, {"cpuMode", "host-model"}, {"boot", "cdrom,hd"}, {"clipboard", true}}, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(changed.contains("/untouched/system.qcow2"));
        auto info = DomainConfig::describe(changed, error);
        QCOMPARE(info["vcpus"].toInt(), 2); QCOMPARE(info["memoryMiB"].toInt(), 1024);
        QVERIFY(info["clipboardConfigured"].toBool());
        QCOMPARE(Configuration::changes(xml, changed).size(), 4);
        auto discarded = Configuration::revert(xml, changed, "memory", error);
        QCOMPARE(DomainConfig::describe(discarded, error)["memoryMiB"].toInt(), 512);
        QCOMPARE(DomainConfig::describe(discarded, error)["vcpus"].toInt(), 2);
        auto bootRestored = Configuration::revert(xml, discarded, "boot", error);
        QVERIFY(bootRestored.contains("order=\"1\""));
        auto invalid = Configuration::hardware(xml, {{"cpus", 0}}, error);
        QVERIFY(invalid.isEmpty());
        error.clear();
        auto advanced = xml; advanced.replace("<vcpu>", "<cputune/><vcpu>");
        QVERIFY(Configuration::hardware(advanced, {{"cpus", 4}}, error).isEmpty());
        QVERIFY(error.contains("advanced"));
    }
    void managedNetworkValidation() {
        QString error;
        QVariantMap values{{"name", "Development"}, {"mode", "nat"}, {"subnet", "192.168.80.0/24"}, {"dhcp", true}, {"dhcpStart", "192.168.80.100"}, {"dhcpEnd", "192.168.80.200"}};
        auto xml = Configuration::networkXml(values, "test-uuid", "omatest", error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(NetworkCatalog::describe(xml)["category"].toString(), "Shared NAT");
        QVERIFY(xml.contains("192.168.80.1"));
        values["dhcpStart"] = "192.168.80.1";
        QVERIFY(Configuration::networkXml(values, "test-uuid", "omatest", error).isEmpty());
        error.clear(); values["mode"] = "isolated";
        xml = Configuration::networkXml(values, "test-uuid", "omatest", error);
        QVERIFY(!xml.contains("<ip")); QVERIFY(!xml.contains("<forward"));
        QCOMPARE(NetworkCatalog::describe(xml)["category"].toString(), "Isolated");
        values["name"] = "../unsafe";
        QVERIFY(Configuration::networkXml(values, "test-uuid", "omatest", error).isEmpty());
        QVERIFY(Diagnostics::nextStep("bridge helper denied access").contains("Networks"));
    }
    void consolePeerClosesDuringHandshake() {
        int sockets[2];
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        auto client = std::make_shared<GraphicsSocket>(sockets[0]);
        // The peer supplies a version then closes before our version reply.
        // LibVNCClient must report EPIPE instead of terminating the process.
        const auto written = ::write(sockets[1], "RFB 003.008\n", 12);
        ::close(sockets[1]);
        QCOMPARE(written, 12);
        bool disconnected = false;
        std::thread worker([&] {
            VncWorker vnc;
            QObject::connect(&vnc, &VncWorker::status, &vnc,
                [&](QString message, bool connected, quint64) {
                    disconnected = !connected && message.contains("handshake failed");
                }, Qt::DirectConnection);
            vnc.start(client, 1);
            vnc.stop();
        });
        worker.join();
        QVERIFY(disconnected);
    }
    void domainDetailsAndAdapterPreservation() {
        const QString xml = R"(<domain type='kvm'><name>fixture</name><uuid>fixture-uuid</uuid>
          <metadata><libosinfo:libosinfo xmlns:libosinfo='http://libosinfo.org/xmlns/libvirt/domain/1.0'><libosinfo:os id='http://ubuntu.com/ubuntu/24.04'/></libosinfo:libosinfo></metadata>
          <memory unit='GiB'>8</memory><currentMemory unit='MiB'>4096</currentMemory><vcpu>4</vcpu>
          <cpu mode='host-passthrough'><topology sockets='1' cores='2' threads='2'/></cpu>
          <os><type arch='x86_64' machine='q35'>hvm</type><loader type='pflash' secure='yes'>/firmware</loader><boot dev='hd'/></os>
          <devices><disk type='file' device='disk'><driver type='qcow2'/><source file='/vm/disk.qcow2'/><target dev='vda' bus='virtio'/></disk>
          <interface type='user'><mac address='52:54:00:12:34:56'/><model type='virtio'/><boot order='2'/><address type='pci' slot='0x03'/></interface>
          <interface type='bridge'><mac address='52:54:00:12:34:57'/><source bridge='br0'/><filterref filter='clean-traffic'/></interface>
          <channel type='unix'><target type='virtio' name='org.qemu.guest_agent.0' state='connected'/></channel>
          <video><model type='virtio' heads='1' vram='16384'/></video></devices></domain>)";
        QString error;
        auto details = DomainConfig::describe(xml, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(details["memoryMiB"].toDouble(), 8192.0);
        QCOMPARE(details["currentMemoryMiB"].toDouble(), 4096.0);
        QCOMPARE(details["firmware"].toString(), "UEFI");
        QCOMPARE(details["osProfile"].toString(), "http://ubuntu.com/ubuntu/24.04");
        QCOMPARE(details["disks"].toList().first().toMap()["source"].toString(), "/vm/disk.qcow2");
        QVERIFY(details["agentConnected"].toBool());
        QVERIFY(!details["interfaces"].toList().last().toMap()["editable"].toBool());
        QString device;
        QVERIFY(DomainConfig::networkDevice(xml, "52:54:00:12:34:56", "bridge", "virbr0", "e1000e", false, false, device, error));
        QDomDocument doc; QVERIFY(doc.setContent(device));
        auto nic = doc.documentElement();
        QCOMPARE(nic.attribute("type"), "bridge");
        QCOMPARE(nic.firstChildElement("source").attribute("bridge"), "virbr0");
        QCOMPARE(nic.firstChildElement("mac").attribute("address"), "52:54:00:12:34:56");
        QCOMPARE(nic.firstChildElement("address").attribute("slot"), "0x03");
        QCOMPARE(nic.firstChildElement("boot").attribute("order"), "2");
        QCOMPARE(nic.firstChildElement("link").attribute("state"), "down");
        QVERIFY(!device.contains("disk.qcow2"));
        QVERIFY(!DomainConfig::networkDevice(xml, "52:54:00:12:34:57", "user", "", "virtio", true, false, device, error));
        QVERIFY(error.contains("advanced"));
        error.clear();
        QVERIFY(!DomainConfig::networkDevice(xml, "52:54:00:00:00:00", "user", "", "virtio", true, false, device, error));
        QVERIFY(error.contains("no longer exists"));
        error.clear();
        QVERIFY(DomainConfig::networkDevice(xml, "", "user", "", "virtio", true, false, device, error));
        QVERIFY(!device.contains("52:54:00:12:34:56"));
        QVERIFY(DomainConfig::revision(xml) != DomainConfig::revision(xml + "\n"));
    }
    void networkClassificationAndPermissions() {
        QCOMPARE(NetworkCatalog::describe("<network><name>nat</name><forward mode='nat'/><bridge name='virbr0'/><ip address='192.168.1.1' prefix='24'/></network>")["category"].toString(), "Shared NAT");
        QCOMPARE(NetworkCatalog::describe("<network><name>host</name><ip address='192.168.2.1'/></network>")["category"].toString(), "Host-only");
        QCOMPARE(NetworkCatalog::describe("<network><name>isolated</name><bridge name='virbr1'/></network>")["category"].toString(), "Isolated");
        QTemporaryDir dir;
        auto path = dir.filePath("bridge.conf"), child = dir.filePath("extra.conf");
        auto write = [](QString path, QByteArray data) { QFile f(path); if (!f.open(QIODevice::WriteOnly)) return false; return f.write(data) == data.size(); };
        QVERIFY(write(path, "allow virbr0\ninclude " + child.toUtf8() + "\n"));
        QVERIFY(write(child, "allow virbr1\ndeny virbr2\n"));
        QVERIFY(DomainConfig::bridgeAllowed("virbr0", path));
        QVERIFY(DomainConfig::bridgeAllowed("virbr1", path));
        QVERIFY(!DomainConfig::bridgeAllowed("unknown", path));
        QVERIFY(write(child, "allow all\ndeny virbr0\n"));
        QVERIFY(!DomainConfig::bridgeAllowed("virbr0", path));
        QVERIFY(write(child, "include " + path.toUtf8() + "\n"));
        QVERIFY(!DomainConfig::bridgeAllowed("virbr0", path));
    }
    void missingTreeRecovery() {
        QTemporaryDir dir;
        auto path = dir.path() + "/current/theme/colors.toml";
        Theme theme(path);
        QVERIFY(QDir().mkpath(dir.path() + "/current/theme"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("background='#111111'\nforeground='#eeeeee'\naccent='#ff3344'");
        file.close();
        QTRY_COMPARE(theme.colors()["accent"].toString(), "#ff3344");
    }
    void paletteValidation() {
        QVariantMap colors; QString error;
        QVERIFY(Theme::parse("background='#111111'\nforeground='#eeeeee'\naccent='#88aaff'", colors, error));
        QCOMPARE(colors["background"].toString(), "#111111");
        auto before = colors;
        QVERIFY(!Theme::parse("background='broken'", colors, error));
        QCOMPARE(colors, before);
        QVERIFY(!Theme::parse("background='#ffffff'\nforeground='#ffffff'\naccent='#ffffff'", colors, error));
        QCOMPARE(colors, before);
        QVERIFY(!Theme::parse("background = [", colors, error));
        // Pure-black palettes still need visible surfaces; bright accent buttons
        // use dark ink even when the rest of the window uses light text.
        QVERIFY(Theme::parse("background='#000000'\nforeground='#ffffff'\naccent='#ffff00'", colors, error));
        QVERIFY(colors["surface"] != colors["background"]);
        QCOMPARE(colors["accentText"].toString(), "#000000");
        QVERIFY(Theme::parse("background='#ffffff'\nforeground='#000000'\naccent='#111144'", colors, error));
        QCOMPARE(colors["accentText"].toString(), "#ffffff");
    }
    void replacementAndRecovery() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto current = dir.path() + "/current";
        QDir().mkpath(current + "/theme");
        auto path = current + "/theme/colors.toml";
        auto write = [&](QByteArray data) { QSaveFile f(path); if (!f.open(QIODevice::WriteOnly)) return false; f.write(data); return f.commit(); };
        QVERIFY(write("background='#111111'\nforeground='#eeeeee'\naccent='#88aaff'"));
        Theme theme(path);
        QCOMPARE(theme.colors()["accent"].toString(), "#88aaff");
        QVERIFY(QDir().rename(current + "/theme", current + "/old"));
        QVERIFY(QDir().mkpath(current + "/theme"));
        QVERIFY(write("background='#ffffff'\nforeground='#111111'\naccent='#2233aa'"));
        QTRY_COMPARE(theme.colors()["accent"].toString(), "#2233aa");
        QVERIFY(write("not valid toml"));
        QTRY_VERIFY(!theme.status().startsWith("Following"));
        QCOMPARE(theme.colors()["accent"].toString(), "#2233aa");
        QVERIFY(write("background='#111111'\nforeground='#eeeeee'\naccent='#ffbb88'"));
        QTRY_COMPARE(theme.colors()["accent"].toString(), "#ffbb88");
        theme.setMode("light");
        QCOMPARE(theme.colors()["background"].toString(), "#f4f6fa");
        theme.setMode("hacker");
        QCOMPARE(theme.mode(), QString("hacker")); QCOMPARE(theme.colors()["accent"].toString(), "#00ff5f");
        QCOMPARE(theme.colors()["accentText"].toString(), "#000000");
        theme.setMode("omarchy");
        QCOMPARE(theme.colors()["accent"].toString(), "#ffbb88");
    }
};
QTEST_GUILESS_MAIN(CoreTest)
#include "test_core.moc"
