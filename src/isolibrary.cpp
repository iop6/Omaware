// SPDX-License-Identifier: GPL-3.0-or-later
#include "isolibrary.h"
#include "applianceimport.h"
#include "unattended.h"
#include <QSet>
#include "paths.h"
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Fills {codename}, {version}, {major} and similar placeholders in a URL or preset template.
QString fill(QString text, const QHash<QString, QString> &values) {
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        text.replace("{" + it.key() + "}", it.value());
    return text;
}

QString majorOf(const QString &version) {
    return version.section('.', 0, 0);
}

const QString offline = "Couldn't reach the publisher. Check your internet connection and try again.";

// How long a download may receive nothing before it is treated as a dropped connection (tests shorten it).
qint64 stallLimit() {
    const int seconds = qEnvironmentVariableIntValue("OMAWARE_STALL_SECONDS");
    return seconds > 0 ? seconds * 1000LL : 60000;
}

// Microsoft's download service expects the requests its website makes in a browser.
const QByteArray browserAgent = "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";

// Hex lengths of the checksums publishers use; anything else isn't a checksum.
QString algorithmFor(const QString &hex) {
    static const QRegularExpression valid("^[0-9a-f]+$");
    if (!valid.match(hex).hasMatch()) return {};
    return hex.size() == 64 ? "sha256" : hex.size() == 128 ? "sha512" : hex.size() == 32 ? "md5" : QString{};
}

int strength(const QString &algorithm) {
    return algorithm == "sha512" ? 3 : algorithm == "sha256" ? 2 : algorithm == "md5" ? 1 : 0;
}

std::optional<QCryptographicHash::Algorithm> hashOf(const QString &algorithm) {
    if (algorithm == "sha256") return QCryptographicHash::Sha256;
    if (algorithm == "sha512") return QCryptographicHash::Sha512;
    if (algorithm == "md5") return QCryptographicHash::Md5;
    return std::nullopt;
}

// SHA-256 and SHA-512 from the publisher prove the file is the one they released; MD5 only catches damage.
bool verifying(const QString &algorithm) {
    return algorithm == "sha256" || algorithm == "sha512";
}

const QString metaFile = ".omaware-media.json";
}

IsoLibrary::IsoLibrary(QObject *parent) : QObject(parent) {
    // id, name, description, category, badge color, local file pattern (captures the version), preset template, lookup.
    auto add = [this](QString id, QString name, QString description, QString category, QString color, QString pattern,
                       QString preset, QVariantMap lookup) {
        Source source;
        source.id = id;
        source.name = name;
        source.description = description;
        source.category = category;
        source.color = color;
        source.pattern = pattern;
        source.preset = preset;
        source.lookup = lookup;
        if (lookup.value("type") == "page") source.kind = "page";
        source.page = lookup.value("page").toString();
        source.note = lookup.value("note").toString();
        source.media = lookup.value("media", "installer").toString();
        source.trustNote = lookup.value("trust").toString();
        sources_.append(source);
    };
    const QString ubuntu = "https://releases.ubuntu.com/{codename}/";
    // Desktop
    add("ubuntu-desktop", "Ubuntu Desktop", "The most popular Linux desktop. Long-term support (LTS) releases.",
            "desktop", "#E95420", R"(^ubuntu-([0-9][0-9.]*)-desktop-amd64\.iso$)", "ubuntu{mm}",
            {{"type", "ubuntu"}, {"base", ubuntu}, {"count", 3}});
    add("kubuntu", "Kubuntu", "Ubuntu with the KDE Plasma desktop. LTS releases.", "desktop", "#0079C1",
            R"(^kubuntu-([0-9][0-9.]*)-desktop-amd64\.iso$)", "ubuntu{mm}",
            {{"type", "ubuntu"}, {"base", "https://cdimage.ubuntu.com/kubuntu/releases/{codename}/release/"},
                    {"count", 2}});
    add("xubuntu", "Xubuntu", "Ubuntu with the light Xfce desktop, good for smaller VMs. LTS releases.", "desktop",
            "#2284F2", R"(^xubuntu-([0-9][0-9.]*)-desktop-amd64\.iso$)", "ubuntu{mm}",
            {{"type", "ubuntu"}, {"base", "https://cdimage.ubuntu.com/xubuntu/releases/{codename}/release/"},
                    {"count", 2}});
    add("linux-mint", "Linux Mint", "A friendly, Windows-like desktop based on Ubuntu (Cinnamon edition).", "desktop",
            "#87CF3E", R"(^linuxmint-([0-9][0-9.]*)-cinnamon-64bit\.iso$)", "linuxmint{m}",
            {{"type", "folder"}, {"index", "https://mirrors.edge.kernel.org/linuxmint/stable/"},
                    {"sums", "https://mirrors.edge.kernel.org/linuxmint/stable/{version}/sha256sum.txt"},
                    {"base", "https://mirrors.edge.kernel.org/linuxmint/stable/{version}/"}, {"count", 3}});
    add("fedora-workstation", "Fedora Workstation", "Up-to-date software with the GNOME desktop.", "desktop", "#51A2DA",
            R"(^Fedora-Workstation-Live-(?:x86_64-)?([0-9]+)-[0-9.]+(?:\.x86_64)?\.iso$)", "fedora{m}",
            {{"type", "fedora"}, {"variant", "Workstation"}});
    add("fedora-kde", "Fedora KDE Plasma", "Fedora with the KDE Plasma desktop.", "desktop", "#3C6EB4",
            R"(^Fedora-KDE-[A-Za-z-]*?(?:x86_64-)?([0-9]+)-[0-9.]+(?:\.x86_64)?\.iso$)", "fedora{m}",
            {{"type", "fedora"}, {"variant", "KDE"}});
    add("debian", "Debian", "The rock-solid base of Ubuntu and many others. Small installer that downloads the rest.",
            "desktop", "#D70A53", R"(^debian-([0-9][0-9.]*)-amd64-netinst\.iso$)", "debian{m}",
            {{"type", "sums"}, {"sums", "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/SHA256SUMS"},
                    {"base", "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/"},
                    // The previous stable release, still supported for a while.
                    {"more", QVariantList{QStringList{"https://cdimage.debian.org/cdimage/archive/latest-oldstable/"
                                                      "amd64/iso-cd/SHA256SUMS",
                                     "https://cdimage.debian.org/cdimage/archive/latest-oldstable/amd64/iso-cd/"}}}});
    add("arch", "Arch Linux", "A minimal, always-current system you build up yourself. New image every month.",
            "desktop", "#1793D1", R"(^archlinux-([0-9][0-9.]*)-x86_64\.iso$)", "archlinux",
            {{"type", "sums"}, {"sums", "https://geo.mirror.pkgbuild.com/iso/latest/sha256sums.txt"},
                    {"base", "https://geo.mirror.pkgbuild.com/iso/latest/"}});
    add("opensuse-tumbleweed", "openSUSE Tumbleweed",
            "A rolling release with the newest software, tested before each snapshot.", "desktop", "#73BA25",
            R"(^openSUSE-Tumbleweed-DVD-x86_64-Snapshot([0-9]+)-Media\.iso$)", "opensusetumbleweed",
            {{"type", "sums"},
                    {"sums", "https://download.opensuse.org/tumbleweed/iso/"
                             "openSUSE-Tumbleweed-DVD-x86_64-Current.iso.sha256"},
                    {"base", "https://download.opensuse.org/tumbleweed/iso/"}});
    add("pop-os", "Pop!_OS", "System76's polished desktop based on Ubuntu, with the COSMIC desktop.", "desktop",
            "#48B9C7", R"(^pop-os_([0-9][0-9.]*)_amd64_intel_[0-9]+\.iso$)", "popos{mm}", {{"type", "popos"}});
    add("nixos", "NixOS", "A Linux system configured entirely from one file; easy to reproduce and roll back.",
            "desktop", "#5277C3", R"(^nixos-graphical-([0-9][0-9.]*)\.[0-9a-f]+-x86_64-linux\.iso$)", "nixos-{mm}",
            {{"type", "nixos"}});
    // Server
    add("ubuntu-server", "Ubuntu Server", "Ubuntu without a desktop, for servers. LTS releases.", "server", "#E95420",
            R"(^ubuntu-([0-9][0-9.]*)-live-server-amd64\.iso$)", "ubuntu{mm}",
            {{"type", "ubuntu"}, {"base", ubuntu}, {"count", 3}});
    add("rocky", "Rocky Linux", "A free rebuild of Red Hat Enterprise Linux. Network installer.", "server", "#10B981",
            R"(^Rocky-([0-9][0-9.]*)-x86_64-boot\.iso$)", "rocky{m}",
            {{"type", "folder"}, {"index", "https://download.rockylinux.org/pub/rocky/"},
                    {"sums", "https://download.rockylinux.org/pub/rocky/{major}/isos/x86_64/CHECKSUM"},
                    {"base", "https://download.rockylinux.org/pub/rocky/{major}/isos/x86_64/"}, {"count", 3},
                    {"perMajor", true}});
    add("almalinux", "AlmaLinux", "Another free Red Hat Enterprise Linux rebuild. Network installer.", "server",
            "#0F4266", R"(^AlmaLinux-([0-9][0-9.]*)-x86_64-boot\.iso$)", "almalinux{m}",
            {{"type", "folder"}, {"index", "https://repo.almalinux.org/almalinux/"},
                    {"sums", "https://repo.almalinux.org/almalinux/{major}/isos/x86_64/CHECKSUM"},
                    {"base", "https://repo.almalinux.org/almalinux/{major}/isos/x86_64/"}, {"count", 3},
                    {"perMajor", true}});
    add("alpine", "Alpine Linux", "A tiny, security-minded system; this edition is made for VMs.", "server", "#0D597F",
            R"(^alpine-virt-([0-9][0-9.]*)-x86_64\.iso$)", "alpinelinux{mm}",
            {{"type", "alpine"}, {"flavor", "alpine-virt"}});
    add("proxmox", "Proxmox VE", "A virtualization platform for running VMs and containers on a server.", "server",
            "#E57000", R"(^proxmox-ve_([0-9][0-9.-]*)\.iso$)", "",
            {{"type", "sums"}, {"sums", "https://enterprise.proxmox.com/iso/SHA256SUMS"},
                    {"base", "https://enterprise.proxmox.com/iso/"}});
    add("freebsd", "FreeBSD", "A complete BSD operating system, known for networking and storage.", "server", "#AB2B28",
            R"(^FreeBSD-([0-9][0-9.]*)-RELEASE-amd64-disc1\.iso$)", "freebsd{mm}",
            {{"type", "folder"}, {"index", "https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/"},
                    {"sums", "https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/{version}/"
                             "CHECKSUM.SHA256-FreeBSD-{version}-RELEASE-amd64"},
                    {"base", "https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/{version}/"}, {"count", 3}});
    // Security & networking
    add("remnux", "REMnux",
            "Malware analysis toolkit. Download the official virtual appliance, then import its OVA as a disk (not an "
            "installer ISO).",
            "security", "#4B8795", R"(^remnux[-_]?([0-9][0-9.]*)?.*\.(?:ova|qcow2)$)", "",
            {{"type", "page"}, {"page", "https://docs.remnux.org/install-distro/get-virtual-appliance"},
                    {"note", "Official appliance page; download and verify using the publisher's instructions. OmaWare "
                             "copies the disk into an independent stopped VM. Choose matching firmware and keep "
                             "malware-analysis networking isolated."}});
    add("kali", "Kali Linux", "Security testing tools, ready to use.", "security", "#367BF0",
            R"(^kali-linux-([0-9][0-9.]*)-installer-amd64\.iso$)", "",
            {{"type", "sums"}, {"sums", "https://cdimage.kali.org/current/SHA256SUMS"},
                    {"base", "https://cdimage.kali.org/current/"}});
    add("opnsense", "OPNsense", "A firewall and router with a web interface; can route your VMs to the internet.",
            "security", "#D94F00", R"(^OPNsense-([0-9][0-9.]*)-dvd-amd64\.iso$)", "",
            {{"type", "folder"}, {"index", "https://pkg.opnsense.org/releases/"}, {"folder", R"(^[0-9]+\.[0-9]+$)"},
                    {"sums", "https://pkg.opnsense.org/releases/{version}/OPNsense-{version}-checksums-amd64.sha256"},
                    // The checksum comes from OPNsense's own server; the image from an official mirror, which is much
                    // faster.
                    {"base", "https://mirror.wdc1.us.leaseweb.net/opnsense/releases/{version}/"},
                    {"remote", R"(^OPNsense-([0-9][0-9.]*)-dvd-amd64\.iso\.bz2$)"}, {"count", 2}});
    add("kali-vm", "Kali Linux VM",
            "Kali ready to run: Offensive Security's prebuilt VM disk, imported as a new VM with no installer. Log in "
            "as kali / kali.",
            "security", "#2A5DB0", R"(^kali-linux-([0-9][0-9.]*)-qemu-amd64\.qcow2$)", "",
            {{"type", "sums"}, {"sums", "https://cdimage.kali.org/current/SHA256SUMS"},
                    {"base", "https://cdimage.kali.org/current/"}, {"media", "image"},
                    {"remote", R"(^kali-linux-([0-9][0-9.]*)-qemu-amd64\.7z$)"},
                    {"note", "Change the kali password after the first login."}});
    add("parrot", "Parrot Security",
            "Security testing, forensics and privacy tools on a Debian base; an alternative to Kali.", "security",
            "#0FB5C4", R"(^Parrot-security-([0-9][0-9.]*)_amd64\.iso$)", "",
            {{"type", "folder"}, {"index", "https://deb.parrot.sh/parrot/iso/"},
                    {"sums", "https://deb.parrot.sh/parrot/iso/{version}/signed-hashes.txt"},
                    {"base", "https://deb.parrot.sh/parrot/iso/{version}/"}, {"count", 2},
                    {"trust", "Parrot publishes only MD5 checksums. They catch a damaged download, but can't prove the "
                              "file is the one Parrot released."}});
    add("security-onion", "Security Onion",
            "Network security monitoring, threat hunting and log management in one platform. Needs a large VM.",
            "security", "#1F7A8C", R"(^securityonion-([0-9][0-9.-]*)\.iso$)", "", {{"type", "securityonion"}});
    add("caine", "CAINE",
            "A computer forensics live system: investigation tools that don't write to the disks you examine.",
            "security", "#6A4C93", R"(^caine([0-9][0-9.]*)\.iso$)", "", {{"type", "caine"}});
    add("tsurugi", "Tsurugi Linux", "Digital forensics and incident response (DFIR), with a very large toolset.",
            "security", "#B23A3A", R"(^tsurugi_linux_([0-9][0-9.]*)\.iso$)", "",
            {{"type", "sums"}, {"sums", "https://tsurugi-linux.org/signed_hashes.sha512"},
                    {"base", "https://ftp.nluug.nl/os/Linux/distr/tsurugi/01.Tsurugi_Linux_%5bLAB%5d/"}});
    add("pfsense", "pfSense CE",
            "A popular firewall and router. Netgate offers it through its store (free, with a Netgate account).",
            "security", "#2B5797", R"(^(?:pfSense-CE|netgate-installer)[-_]([0-9][0-9.]*).*\.iso$)", "",
            {{"type", "page"}, {"page", "https://www.pfsense.org/download/"},
                    {"note", "Netgate's installer downloads pfSense while installing, so the VM needs internet access "
                             "during setup."}});
    // Windows
    // Straight from Microsoft's own download service, the way its website (and tools like Fido and Mido) get
    // the 24-hour link, and checked against the SHA-256 Microsoft publishes for each language.
    add("windows-11", "Windows 11",
            "The official multi-edition ISO from Microsoft, in your language. You need a Windows license to activate "
            "it.",
            "windows", "#0078D4",
            R"(^Win(?:dows)?11_(?:Client_x64_[A-Za-z-]+_)?([0-9][0-9A-Za-z_]*?)(?:_[A-Z][A-Za-z]*_x64)?(?:v[0-9])?\.iso$)",
            "win11",
            {{"type", "microsoft"}, {"page", "https://www.microsoft.com/en-us/software-download/windows11"},
                    {"note", "Microsoft sometimes refuses automated downloads from some networks. If that happens, "
                             "download it on Microsoft's website and save it in your ISO folder."}});
    const QString evaluation = "Microsoft doesn't publish checksums for evaluation copies, so only the secure (HTTPS) "
                               "connection to Microsoft vouches for this download.";
    add("windows-11-enterprise", "Windows 11 Enterprise (evaluation)",
            "Free to evaluate for 90 days, no license key needed. Good for test machines and malware labs.", "windows",
            "#005A9E",
            R"(^([0-9]+\.[0-9]+)\.[0-9]+-[0-9]+\.[A-Za-z0-9_]+_CLIENTENTERPRISEEVAL_[A-Za-z]+_x64FRE_[A-Za-z-]+\.iso$)",
            "win11",
            {{"type", "evaluation"},
                    {"page", "https://www.microsoft.com/en-us/evalcenter/download-windows-11-enterprise"},
                    {"product", "Windows 11 Enterprise"}, {"trust", evaluation}});
    add("windows-server", "Windows Server 2025 (evaluation)",
            "Free to evaluate for 180 days. For Active Directory, file servers and lab targets.", "windows", "#004E8C",
            R"(^([0-9]+\.[0-9]+)\.[0-9]+-[0-9]+\.[A-Za-z0-9_]+_SERVER_EVAL_x64FRE_[A-Za-z-]+\.iso$)", "win2k25",
            {{"type", "evaluation"},
                    {"page", "https://www.microsoft.com/en-us/evalcenter/download-windows-server-2025"},
                    {"product", "Windows Server 2025"}, {"trust", evaluation}});
    applianceFolder_ = Paths::root() + "/appliances";
    setFolder(Paths::isos());
}

IsoLibrary::~IsoLibrary() {
    for (auto id : jobs_.keys())
        cancel(id);
    if (importThread_) {
        import_->cancel = true;
        importThread_->disconnect(this);
        importThread_->wait();
        delete importThread_;
    }
}

QString IsoLibrary::root() const {
    return Paths::root();
}

void IsoLibrary::setFolder(const QString &folder) {
    folder_ = folder;
    applianceFolder_ = QFileInfo(folder).absolutePath() + "/appliances";
    QDir().mkpath(folder_);
    loadMeta();
    rescan();
}

void IsoLibrary::loadMeta() {
    kept_.clear();
    checked_.clear();
    QFile file(folder_ + "/" + metaFile);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1 << 20) return;
    const auto o = QJsonDocument::fromJson(file.readAll()).object();
    for (const auto &name : o["kept"].toArray())
        if (!name.toString().isEmpty()) kept_ << name.toString();
    const auto checked = o["checked"].toObject();
    for (auto it = checked.begin(); it != checked.end(); ++it)
        checked_[it.key()] = it.value().toString();
}

void IsoLibrary::saveMeta() {
    QJsonObject checked;
    for (auto it = checked_.cbegin(); it != checked_.cend(); ++it)
        checked[it.key()] = it.value();
    QFile file(folder_ + "/" + metaFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(
                QJsonDocument(QJsonObject{{"kept", QJsonArray::fromStringList(kept_)}, {"checked", checked}}).toJson());
}

void IsoLibrary::setKept(const QString &name, bool kept) {
    bool known = false;
    for (const auto &entry : files_)
        if (entry.toMap()["name"] == name) known = true;
    if (!known || kept_.contains(name) == kept) return;
    if (kept)
        kept_ << name;
    else
        kept_.removeAll(name);
    saveMeta();
    rescan();
}

IsoLibrary::Source *IsoLibrary::find(const QString &id) {
    for (auto &source : sources_)
        if (source.id == id) return &source;
    return nullptr;
}

const IsoLibrary::Source *IsoLibrary::find(const QString &id) const {
    for (const auto &source : sources_)
        if (source.id == id) return &source;
    return nullptr;
}

QStringList IsoLibrary::sourceIds() const {
    QStringList ids;
    for (const auto &source : sources_)
        ids << source.id;
    return ids;
}

bool IsoLibrary::newer(const QString &a, const QString &b) {
    const auto x = a.split(QRegularExpression("[^0-9]+"), Qt::SkipEmptyParts),
               y = b.split(QRegularExpression("[^0-9]+"), Qt::SkipEmptyParts);
    for (int i = 0; i < std::max(x.size(), y.size()); ++i) {
        const auto p = i < x.size() ? x[i].toLongLong() : 0, q = i < y.size() ? y[i].toLongLong() : 0;
        if (p != q) return p > q;
    }
    return false;
}

QVariantMap IsoLibrary::identify(const QString &name) const {
    for (const auto &source : sources_) {
        const auto match = QRegularExpression(source.pattern, QRegularExpression::CaseInsensitiveOption).match(name);
        if (!match.hasMatch()) continue;
        const auto version = match.captured(1).replace('_', '.');
        const auto parts = version.split('.');
        const auto preset =
                fill(source.preset, {{"v", version}, {"m", parts.value(0)}, {"mm", parts.mid(0, 2).join('.')}});
        return {{"source", source.id}, {"sourceName", source.name}, {"version", version}, {"preset", preset},
                {"setup", Unattended::kindForFile(name)}};
    }
    return {};
}

void IsoLibrary::rescan() {
    QVariantList rows;
    QHash<QString, QString> newest;
    auto entries = QDir(folder_).entryInfoList(
            {"*.iso", "*.ISO", "*.qcow2", "*.QCOW2", "*.ova", "*.OVA"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    if (QDir(folder_).absolutePath() != QDir(applianceFolder_).absolutePath())
        entries += QDir(applianceFolder_)
                           .entryInfoList({"*.qcow2", "*.QCOW2", "*.ova", "*.OVA"}, QDir::Files | QDir::NoSymLinks,
                                   QDir::Name);
    QSet<QString> seen;
    for (const auto &info : entries) {
        if (seen.contains(info.fileName())) continue;
        seen.insert(info.fileName());
        auto row = identify(info.fileName());
        row["type"] = ApplianceImport::mediaType(info.fileName());
        row["format"] = info.suffix().toLower();
        const auto source = row["source"].toString(), version = row["version"].toString();
        if (!source.isEmpty() && (!newest.contains(source) || newer(version, newest[source]))) newest[source] = version;
        row["name"] = info.fileName();
        row["path"] = info.absoluteFilePath();
        // The space it really takes: a VM disk (qcow2) lists its whole virtual size, but its empty parts take none.
        struct stat st{};
        const bool sized = ::stat(QFile::encodeName(info.absoluteFilePath()).constData(), &st) == 0;
        row["size"] = double(sized ? std::min<qint64>(info.size(), qint64(st.st_blocks) * 512) : info.size());
        row["listedSize"] = double(info.size());
        row["kept"] = kept_.contains(info.fileName());
        // How OmaWare checked it when it downloaded it: "sha256", "sha512", "md5", "unverified", or "" if you added it.
        row["check"] = !checked_.contains(info.fileName())   ? QString()
                       : checked_[info.fileName()].isEmpty() ? QString("unverified")
                                                             : checked_[info.fileName()];
        rows.append(row);
    }
    for (auto &entry : rows) {
        auto row = entry.toMap();
        const auto source = row["source"].toString();
        row["newest"] = source.isEmpty() || newest.value(source) == row["version"].toString() || row["kept"].toBool();
        entry = row;
    }
    files_ = rows;
    freeBytes_ = double(QStorageInfo(folder_).bytesAvailable());
    emit filesChanged();
    emit changed();
}

QVariantList IsoLibrary::sources() const {
    QVariantList result;
    for (const auto &source : sources_) {
        QString have;
        QStringList haveVersions, versions;
        for (const auto &entry : files_) {
            const auto row = entry.toMap();
            if (row["source"] != source.id) continue;
            haveVersions << row["version"].toString();
            if (have.isEmpty() || newer(row["version"].toString(), have)) have = row["version"].toString();
        }
        for (const auto &release : source.releases)
            versions << release.version;
        const auto job = jobs_.value(source.id);
        const auto pick = selected(source);
        // Unverified until a release with a publisher checksum is known; sources that never have one say why.
        const auto trust = !source.trustNote.isEmpty() || (!pick.version.isEmpty() && !verifying(pick.algorithm))
                                   ? "unverified"
                                   : "checked";
        QVariantMap row{{"id", source.id}, {"name", source.name}, {"description", source.description},
                {"category", source.category}, {"color", source.color}, {"kind", source.kind}, {"page", source.page},
                {"note", source.note}, {"media", source.media}, {"trust", source.kind == "page" ? "" : trust},
                {"trustNote", source.trustNote}, {"versions", versions}, {"haveVersions", haveVersions},
                {"selectedVersion", pick.version}, {"selectedFile", pick.file}, {"selectedSize", double(pick.size)},
                {"algorithm", pick.algorithm}, {"checksum", pick.checksum},
                {"selectedHave", !pick.version.isEmpty() && haveVersions.contains(pick.version)},
                {"status", job ? QString(job->unpacking ? "unpacking" : "downloading") : source.status},
                {"error", source.error}, {"have", have}, {"version", source.latest.version},
                {"file", source.latest.file}, {"url", source.latest.url}, {"size", double(source.latest.size)},
                {"upToDate",
                        !have.isEmpty() && !source.latest.version.isEmpty() && !newer(source.latest.version, have)},
                {"updateAvailable",
                        !have.isEmpty() && !source.latest.version.isEmpty() && newer(source.latest.version, have)}};
        if (job) {
            row["received"] = double(job->received);
            row["total"] = double(job->total);
            row["rate"] = double(job->rate);
        }
        if (!source.languages.isEmpty()) {
            row["languages"] = source.languages;
            row["language"] = source.language;
        }
        result.append(row);
    }
    return result;
}

// ---- Release lists ----------------------------------------------------------------------------
QString IsoLibrary::ubuntuLtsCodename(const QByteArray &metaRelease) {
    return ubuntuLtsCodenames(metaRelease).value(0);
}

QStringList IsoLibrary::ubuntuLtsCodenames(const QByteArray &metaRelease) {
    QStringList codenames;
    for (const auto &block :
            QString::fromUtf8(metaRelease).split(QRegularExpression("\\n\\s*\\n"), Qt::SkipEmptyParts)) {
        QHash<QString, QString> fields;
        for (const auto &line : block.split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon > 0) fields[line.left(colon).trimmed()] = line.mid(colon + 1).trimmed();
        }
        // The list is oldest first.
        if (fields.value("Supported") == "1" && QRegularExpression("^[a-z]+$").match(fields.value("Dist")).hasMatch())
            codenames.prepend(fields.value("Dist"));
    }
    return codenames;
}

IsoLibrary::Release IsoLibrary::fromChecksums(const QByteArray &sums, const QString &pattern, const QString &base) {
    Release best;
    const QRegularExpression gnu(R"(^([0-9a-fA-F]{32,128})\s+\*?(\S+)\s*$)"),
            bsd(R"(^(?:SHA256|SHA512|MD5)\s*\((\S+)\)\s*=\s*([0-9a-fA-F]{32,128})\s*$)"), name(pattern);
    for (const auto &raw : QString::fromUtf8(sums).split('\n')) {
        const auto text = raw.trimmed();
        QString file, hash;
        if (auto m = gnu.match(text); m.hasMatch()) {
            hash = m.captured(1);
            file = m.captured(2);
        } else if (auto m = bsd.match(text); m.hasMatch()) {
            file = m.captured(1);
            hash = m.captured(2);
        } else
            continue;
        hash = hash.toLower();
        const auto version = name.match(file).captured(1), algorithm = algorithmFor(hash);
        if (version.isEmpty() || algorithm.isEmpty()) continue;
        const bool stronger =
                version == best.version && file == best.file && strength(algorithm) > strength(best.algorithm);
        if (!best.version.isEmpty() && !newer(version, best.version) && !stronger) continue;
        best = {version, file, base + file, hash, 0, algorithm};
    }
    return best;
}

IsoLibrary::Release IsoLibrary::fedora(const QByteArray &releasesJson, const QString &variant) {
    return fedoraReleases(releasesJson, variant).value(0);
}

QList<IsoLibrary::Release> IsoLibrary::fedoraReleases(const QByteArray &releasesJson, const QString &variant) {
    QList<Release> found;
    for (const auto &value : QJsonDocument::fromJson(releasesJson).array()) {
        const auto o = value.toObject();
        const auto version = o["version"].toString(), link = o["link"].toString();
        if (o["arch"].toString() != "x86_64" || o["variant"].toString() != variant ||
                o["subvariant"].toString() != variant)
            continue;
        if (!QRegularExpression("^[0-9]+$").match(version).hasMatch() || !link.startsWith("https://") ||
                !link.endsWith(".iso"))
            continue;
        const auto sha = o["sha256"].toString().toLower();
        if (!QRegularExpression("^[0-9a-f]{64}$").match(sha).hasMatch()) continue;
        if (std::any_of(found.cbegin(), found.cend(), [&](const Release &r) { return r.version == version; })) continue;
        found << Release{version, QUrl(link).fileName(), link, sha, o["size"].toString().toLongLong()};
    }
    std::sort(
            found.begin(), found.end(), [](const Release &a, const Release &b) { return newer(a.version, b.version); });
    return found;
}

IsoLibrary::Release IsoLibrary::securityOnion(const QByteArray &markdown) {
    const auto text = QString::fromUtf8(markdown);
    const auto link = QRegularExpression(
            R"((https://download\.securityonion\.net/file/securityonion/(securityonion-([0-9][0-9.-]*)\.iso))\s)")
                              .match(text);
    const auto sha = QRegularExpression(R"(SHA256:\s*([0-9A-Fa-f]{64})\b)").match(text);
    if (!link.hasMatch() || !sha.hasMatch()) return {};
    return {link.captured(3), link.captured(2), link.captured(1), sha.captured(1).toLower(), 0};
}

QVariantMap IsoLibrary::evaluationLinks(const QByteArray &html, const QString &product) {
    QVariantMap links;
    const QRegularExpression label(R"re(aria-label="([^"]*)")re"),
            href(R"re(href="(https://go\.microsoft\.com/fwlink/\?[^"]+)")re"),
            code(R"(\(([a-z]{2}-[A-Za-z]{2,4})\)\s*$)");
    auto tags = QRegularExpression("<a\\s[^>]*>").globalMatch(QString::fromUtf8(html));
    while (tags.hasNext()) {
        const auto tag = tags.next().captured(0);
        const auto name = label.match(tag).captured(1), link = href.match(tag).captured(1);
        const auto language = code.match(name).captured(1);
        if (link.isEmpty() || language.isEmpty() || !name.contains(product) || !name.contains("ISO") ||
                name.contains("LTSC") || links.contains(language))
            continue;
        links[language] = QString(link).replace("&amp;", "&");
    }
    return links;
}

IsoLibrary::Release IsoLibrary::alpine(const QByteArray &yaml, const QString &flavor, const QString &base) {
    // latest-releases.yaml is a list of "- key: value" blocks, one per image flavor.
    QHash<QString, QString> entry;
    Release found;
    auto take = [&] {
        if (entry.value("flavor") == flavor && entry.value("iso").endsWith(".iso") &&
                QRegularExpression("^[0-9a-f]{64}$").match(entry.value("sha256")).hasMatch())
            found = {entry.value("version"), entry.value("iso"), base + entry.value("iso"), entry.value("sha256"),
                    entry.value("size").toLongLong()};
    };
    for (const auto &line : QString::fromUtf8(yaml).split('\n')) {
        auto text = line.trimmed();
        if (text == "-" || text.startsWith("- ")) {
            take();
            entry.clear();
            text = text.mid(1).trimmed();
        }
        const auto colon = text.indexOf(':');
        if (colon > 0) entry[text.left(colon).trimmed()] = text.mid(colon + 1).trimmed();
    }
    take();
    return found;
}

QStringList IsoLibrary::newestFolders(const QByteArray &listing, const QString &pattern, int count, bool perMajor) {
    QStringList all;
    const QRegularExpression href(R"(href="([^"/?]+)/")"), wanted(pattern);
    auto it = href.globalMatch(QString::fromUtf8(listing));
    while (it.hasNext()) {
        const auto name = it.next().captured(1);
        if (wanted.match(name).hasMatch() && !all.contains(name)) all << name;
    }
    std::sort(all.begin(), all.end(), [](const QString &a, const QString &b) { return newer(a, b); });
    QStringList picked;
    for (const auto &name : all) {
        if (picked.size() >= count) break;
        // Rocky and AlmaLinux keep "9" next to "9.6": one folder per major version, its newest.
        if (perMajor && std::any_of(picked.cbegin(), picked.cend(),
                                [&](const QString &p) { return majorOf(p) == majorOf(name); }))
            continue;
        picked << name;
    }
    return picked;
}

QString IsoLibrary::newestFolder(const QByteArray &listing, const QString &pattern) {
    QString best;
    const QRegularExpression href(R"(href="([^"/?]+)/")"), wanted(pattern);
    auto it = href.globalMatch(QString::fromUtf8(listing));
    while (it.hasNext()) {
        const auto name = it.next().captured(1);
        if (wanted.match(name).hasMatch() && (best.isEmpty() || newer(name, best))) best = name;
    }
    return best;
}

void IsoLibrary::get(const QUrl &url, Done done, int attempt, const QList<QPair<QByteArray, QByteArray>> &headers) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "OmaWare/" + QCoreApplication::applicationVersion());
    for (const auto &header : headers)
        request.setRawHeader(header.first, header.second);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    auto reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, done, attempt, headers] {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        // Publishers' servers occasionally fail a request; try once more before giving up.
        if (!ok && attempt == 0 && reply->error() != QNetworkReply::ContentNotFoundError) {
            QTimer::singleShot(1500, this, [this, url, done, headers] { get(url, done, 1, headers); });
            return;
        }
        done(ok, reply->readAll(), reply->url());
    });
}

void IsoLibrary::check() {
    for (auto &source : sources_)
        if (source.kind == "download" && !jobs_.contains(source.id)) resolve(source);
    emit changed();
}

void IsoLibrary::settle(const QString &id, const Release &release, const QString &failure) {
    settleAll(id, release.file.isEmpty() ? QList<Release>{} : QList<Release>{release}, failure);
}

void IsoLibrary::settleAll(const QString &id, QList<Release> releases, const QString &failure) {
    --pending_;
    if (auto s = find(id)) {
        // Only releases OmaWare can download; a checksum is required unless the source is known to lack one.
        releases.erase(std::remove_if(releases.begin(), releases.end(),
                               [&](const Release &r) {
                                   return r.file.isEmpty() || (r.checksum.isEmpty() && s->trustNote.isEmpty() &&
                                                                      s->lookup.value("type") != "microsoft");
                               }),
                releases.end());
        std::stable_sort(releases.begin(), releases.end(),
                [](const Release &a, const Release &b) { return newer(a.version, b.version); });
        QList<Release> unique;
        for (const auto &r : releases)
            if (std::none_of(unique.cbegin(), unique.cend(), [&](const Release &u) { return u.version == r.version; }))
                unique << r;
        if (!unique.isEmpty()) {
            s->releases = unique;
            s->latest = unique.first();
            s->status = "ready";
            s->error.clear();
        } else {
            s->status = "error";
            s->error = failure.isEmpty() ? "The latest release couldn't be found." : failure;
        }
    }
    emit changed();
}

std::function<void(const QList<IsoLibrary::Release> &, const QString &)> IsoLibrary::gather(
        const QString &id, int lookups) {
    struct State {
        QList<Release> found;
        QString failure;
        int left = 0;
    };

    auto state = std::make_shared<State>();
    state->left = lookups;
    return [this, id, state](const QList<Release> &found, const QString &failure) {
        state->found << found;
        if (state->failure.isEmpty()) state->failure = failure;
        // Earlier versions are extras: the source is ready as long as one lookup found something.
        if (--state->left == 0) settleAll(id, state->found, state->found.isEmpty() ? state->failure : QString());
    };
}

void IsoLibrary::resolve(Source &source) {
    const auto id = source.id, type = source.lookup.value("type").toString();
    const auto pattern = source.lookup.value("remote", source.pattern).toString();
    const int count = std::max(1, source.lookup.value("count", 1).toInt());
    source.status = "checking";
    source.error.clear();
    ++pending_;
    // Reads several checksum lists ({sums URL, base URL}) and settles the source with the newest match of each.
    auto sumsMany = [this, id, pattern](const QList<QPair<QString, QString>> &lists) {
        auto done = gather(id, lists.size());
        for (const auto &list : lists) {
            const auto base = list.second;
            get(QUrl(list.first), [pattern, base, done](bool ok, const QByteArray &data, const QUrl &) {
                const auto release = ok ? fromChecksums(data, pattern, base) : Release{};
                done(release.file.isEmpty() ? QList<Release>{} : QList<Release>{release}, ok ? QString{} : offline);
            });
        }
    };
    if (type == "ubuntu") {
        const auto base = source.lookup.value("base").toString();
        get(QUrl("https://changelogs.ubuntu.com/meta-release-lts"),
                [this, id, base, count, sumsMany](bool ok, const QByteArray &data, const QUrl &) {
                    const auto codenames = ok ? ubuntuLtsCodenames(data).mid(0, count) : QStringList{};
                    if (codenames.isEmpty()) {
                        settle(id, {}, ok ? "Ubuntu's release list couldn't be read." : offline);
                        return;
                    }
                    QList<QPair<QString, QString>> lists;
                    for (const auto &codename : codenames) {
                        const auto folder = fill(base, {{"codename", codename}});
                        lists.append({folder + "SHA256SUMS", folder});
                    }
                    sumsMany(lists);
                });
    } else if (type == "sums") {
        QList<QPair<QString, QString>> lists{
                {source.lookup.value("sums").toString(), source.lookup.value("base").toString()}};
        for (const auto &more : source.lookup.value("more").toList()) {
            const auto pair = more.toStringList();
            if (pair.size() == 2) lists.append({pair[0], pair[1]});
        }
        sumsMany(lists);
    } else if (type == "folder") {
        const auto lookup = source.lookup;
        get(QUrl(lookup.value("index").toString()), [this, id, lookup, count, sumsMany](
                                                            bool ok, const QByteArray &data, const QUrl &) {
            const auto versions = ok ? newestFolders(data, lookup.value("folder", R"(^[0-9]+(\.[0-9]+)*$)").toString(),
                                               count, lookup.value("perMajor").toBool())
                                     : QStringList{};
            if (versions.isEmpty()) {
                settle(id, {}, ok ? "The release list couldn't be read." : offline);
                return;
            }
            QList<QPair<QString, QString>> lists;
            for (const auto &version : versions) {
                const QHash<QString, QString> values{{"version", version}, {"major", majorOf(version)}};
                lists.append(
                        {fill(lookup.value("sums").toString(), values), fill(lookup.value("base").toString(), values)});
            }
            sumsMany(lists);
        });
    } else if (type == "securityonion") {
        get(QUrl("https://raw.githubusercontent.com/Security-Onion-Solutions/securityonion/3/main/"
                 "DOWNLOAD_AND_VERIFY_ISO.md"),
                [this, id](bool ok, const QByteArray &data, const QUrl &) {
                    settle(id, ok ? securityOnion(data) : Release{},
                            ok ? "Security Onion's download page couldn't be read." : offline);
                });
    } else if (type == "caine") {
        // CAINE's download page links the ISO and a file with its SHA-256.
        get(QUrl("https://www.caine-live.net/page5/page5.html"),
                [this, id, sumsMany](bool ok, const QByteArray &data, const QUrl &) {
                    const auto m = QRegularExpression(
                            R"re(href="(https://www\.caine-live\.net/page5/(caine[0-9][0-9.]*\.iso)\.sha256\.txt)")re")
                                           .match(QString::fromUtf8(data));
                    if (!ok || !m.hasMatch()) {
                        settle(id, {}, ok ? "CAINE's download page couldn't be read." : offline);
                        return;
                    }
                    sumsMany({{m.captured(1), "https://www.caine-live.net/Downloads/"}});
                });
    } else if (type == "fedora") {
        const auto variant = source.lookup.value("variant").toString();
        get(QUrl("https://fedoraproject.org/releases.json"),
                [this, id, variant, count](bool ok, const QByteArray &data, const QUrl &) {
                    settleAll(id, ok ? fedoraReleases(data, variant).mid(0, std::max(count, 2)) : QList<Release>{},
                            ok ? QString{} : offline);
                });
    } else if (type == "alpine") {
        const auto flavor = source.lookup.value("flavor").toString(),
                   base = QString("https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/");
        get(QUrl(base + "latest-releases.yaml"),
                [this, id, flavor, base](bool ok, const QByteArray &data, const QUrl &) {
                    settle(id, ok ? alpine(data, flavor, base) : Release{}, ok ? QString{} : offline);
                });
    } else if (type == "popos") {
        // Pop!_OS follows Ubuntu LTS versions: try this LTS year, then the ones before.
        const int year = QDate::currentDate().year() % 100;
        QStringList versions;
        for (int y = year - year % 2; y >= year - 4; y -= 2)
            versions << QString("%1.04").arg(y, 2, 10, QChar('0'));
        auto attempt = std::make_shared<std::function<void(int)>>();
        *attempt = [this, id, versions, attempt](int i) {
            if (i >= versions.size()) {
                settle(id, {}, "Pop!_OS's release list couldn't be read.");
                return;
            }
            get(QUrl("https://api.pop-os.org/builds/" + versions[i] + "/intel"), [this, id, i, attempt](bool ok,
                                                                                         const QByteArray &data,
                                                                                         const QUrl &) {
                const auto o = QJsonDocument::fromJson(data).object();
                const auto url = o["url"].toString(), sha = o["sha_sum"].toString().toLower();
                if (!ok || !url.startsWith("https://") || !QRegularExpression("^[0-9a-f]{64}$").match(sha).hasMatch()) {
                    (*attempt)(i + 1);
                    return;
                }
                settle(id, {o["version"].toString(), QUrl(url).fileName(), url, sha, qint64(o["size"].toDouble())}, {});
            });
        };
        (*attempt)(0);
    } else if (type == "nixos") {
        // NixOS releases are YY.05 and YY.11; the newest one with a published image wins.
        const int year = QDate::currentDate().year() % 100;
        QStringList versions;
        for (int y = year; y >= year - 1; --y)
            versions << QString("%1.11").arg(y, 2, 10, QChar('0')) << QString("%1.05").arg(y, 2, 10, QChar('0'));
        auto attempt = std::make_shared<std::function<void(int)>>();
        *attempt = [this, id, versions, attempt](int i) {
            if (i >= versions.size()) {
                settle(id, {}, "NixOS's release list couldn't be read.");
                return;
            }
            const auto channel = "https://channels.nixos.org/nixos-" + versions[i] +
                                 "/latest-nixos-graphical-x86_64-linux.iso.sha256";
            get(QUrl(channel), [this, id, i, attempt](bool ok, const QByteArray &data, const QUrl &finalUrl) {
                // The channel link redirects to the exact release; its checksum names the file next to it.
                const auto m = QRegularExpression(R"(^([0-9a-f]{64})\s+(\S+\.iso)\s*$)")
                                       .match(QString::fromUtf8(data).trimmed());
                if (!ok || !m.hasMatch()) {
                    (*attempt)(i + 1);
                    return;
                }
                const auto base = finalUrl.toString().section('/', 0, -2) + "/";
                const auto version = QRegularExpression(R"(^nixos-graphical-([0-9][0-9.]*)\.[0-9a-f]+-)")
                                             .match(m.captured(2))
                                             .captured(1);
                settle(id, {version, m.captured(2), base + m.captured(2), m.captured(1), 0}, {});
            });
        };
        (*attempt)(0);
    } else if (type == "microsoft") {
        get(QUrl(source.lookup.value("page").toString()),
                [this, id](bool ok, const QByteArray &data, const QUrl &) {
                    const auto page = ok ? windowsPage(data) : QVariantMap{};
                    auto s = find(id);
                    if (!s || page.isEmpty()) {
                        settle(id, {}, ok ? "Microsoft's download page couldn't be read." : offline);
                        return;
                    }
                    s->version = page["version"].toString();
                    s->edition = page["edition"].toString();
                    s->languages = page["languages"].toStringList();
                    s->hashes.clear();
                    const auto hashes = page["hashes"].toMap();
                    for (auto it = hashes.cbegin(); it != hashes.cend(); ++it)
                        s->hashes[it.key()] = it.value().toString();
                    if (!s->languages.contains(s->language))
                        s->language = windowsLanguage(QLocale::system(), s->languages);
                    settle(id, windowsRelease(*s), {});
                },
                0, {{"User-Agent", browserAgent}});
    } else if (type == "evaluation") {
        const auto product = source.lookup.value("product").toString();
        get(QUrl(source.lookup.value("page").toString()),
                [this, id, product](bool ok, const QByteArray &data, const QUrl &) {
                    const auto links = ok ? evaluationLinks(data, product) : QVariantMap{};
                    auto s = find(id);
                    if (!s || links.isEmpty()) {
                        settle(id, {}, ok ? "Microsoft's Evaluation Center page couldn't be read." : offline);
                        return;
                    }
                    // Languages by name ("English (United States)"), the computer's own first if Microsoft offers it.
                    s->links.clear();
                    s->languages.clear();
                    QString mine;
                    const auto system = QLocale::system().bcp47Name();
                    for (auto it = links.cbegin(); it != links.cend(); ++it) {
                        const QLocale locale(QString(it.key()).replace('-', '_'));
                        const auto name = QLocale::languageToString(locale.language()) + " (" +
                                          QLocale::territoryToString(locale.territory()) + ")";
                        s->links[name] = it.value().toString();
                        s->languages << name;
                        if (it.key().compare(system, Qt::CaseInsensitive) == 0 ||
                                (mine.isEmpty() && it.key().compare("en-US", Qt::CaseInsensitive) == 0))
                            mine = name;
                    }
                    s->languages.sort();
                    if (!s->languages.contains(s->language)) s->language = mine.isEmpty() ? s->languages.first() : mine;
                    resolveEvaluation(id);
                },
                0, {{"User-Agent", browserAgent}});
    } else
        settle(id, {}, "");
}

// The evaluation link redirects to the ISO itself; its name says the build, and its size is known up front.
void IsoLibrary::resolveEvaluation(const QString &id) {
    auto s = find(id);
    if (!s) return;
    const auto pattern = s->pattern;
    head(QUrl(s->links.value(s->language)), [this, id, pattern](bool ok, const QUrl &finalUrl, qint64 size) {
        const auto file = finalUrl.fileName();
        const auto version =
                QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption).match(file).captured(1);
        if (!ok || version.isEmpty() || finalUrl.scheme() != "https") {
            settle(id, {}, ok ? "Microsoft's download link didn't lead to an ISO." : offline);
            return;
        }
        settle(id, {version, file, finalUrl.toString(), {}, size, {}}, {});
    });
}

void IsoLibrary::head(const QUrl &url, std::function<void(bool, const QUrl &, qint64)> done) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, browserAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    auto reply = network_.head(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        done(reply->error() == QNetworkReply::NoError, reply->url(),
                reply->header(QNetworkRequest::ContentLengthHeader).toLongLong());
    });
}

IsoLibrary::Release IsoLibrary::selected(const Source &source) const {
    if (source.lookup.value("type") == "microsoft") return source.latest;
    for (const auto &release : source.releases)
        if (!source.chosen.isEmpty() && release.version == source.chosen) return release;
    return source.latest;
}

void IsoLibrary::setVersion(const QString &id, const QString &version) {
    auto s = find(id);
    if (!s || jobs_.contains(id) || s->chosen == version) return;
    s->chosen = version;
    emit changed();
    emit versionChosen(id, version);
}

// ---- Windows from Microsoft --------------------------------------------------------------------
QVariantMap IsoLibrary::windowsPage(const QByteArray &html) {
    const auto text = QString::fromUtf8(html);
    const auto version = QRegularExpression(R"(Version ([0-9]{2}H[12]))").match(text).captured(1);
    const auto edition = QRegularExpression(R"re(<option value="([0-9]{1,8})">Windows)re").match(text).captured(1);
    QStringList languages;
    QVariantMap hashes;
    auto rows = QRegularExpression(R"(<td>([^<]{2,60}?) 64-bit</td>\s*<td>([0-9A-Fa-f]{64})</td>)").globalMatch(text);
    while (rows.hasNext()) {
        const auto row = rows.next();
        const auto language = row.captured(1).trimmed();
        if (!hashes.contains(language)) languages << language;
        hashes[language] = row.captured(2).toLower();
    }
    if (version.isEmpty() || edition.isEmpty() || languages.isEmpty()) return {};
    return {{"version", version}, {"edition", edition}, {"languages", languages}, {"hashes", hashes}};
}

QString IsoLibrary::windowsSku(const QByteArray &skusJson, const QString &language) {
    for (const auto &value : QJsonDocument::fromJson(skusJson).object()["Skus"].toArray()) {
        const auto sku = value.toObject();
        if (sku["Language"].toString() == language || sku["LocalizedLanguage"].toString() == language) {
            const auto id = sku["Id"].toVariant().toString();
            if (QRegularExpression("^[0-9]{1,12}$").match(id).hasMatch()) return id;
        }
    }
    return {};
}

QString IsoLibrary::windowsLink(const QByteArray &linksJson, QString &error) {
    const auto o = QJsonDocument::fromJson(linksJson).object();
    for (const auto &value : o["Errors"].toArray()) {
        const auto text = value.toObject()["Value"].toString();
        error = text.contains("Sentinel") || value.toObject()["Key"].toString().contains("Sentinel")
                        ? "Microsoft refused the automated download from your network (this happens with some VPNs and "
                          "providers). Download it on Microsoft's website instead and save it in your ISO folder."
                        : "Microsoft didn't give a download link: " + text;
        return {};
    }
    for (const auto &value : o["ProductDownloadOptions"].toArray()) {
        const auto link = value.toObject()["Uri"].toString();
        if (link.startsWith("https://") && QUrl(link).fileName().contains("x64", Qt::CaseInsensitive)) return link;
    }
    error = "Microsoft didn't give a download link for 64-bit Windows.";
    return {};
}

QString IsoLibrary::windowsLanguage(const QLocale &locale, const QStringList &available) {
    QString wanted;
    switch (locale.language()) {
    case QLocale::English:
        wanted = locale.territory() == QLocale::UnitedStates ? "English" : "English International";
        break;
    case QLocale::Chinese:
        wanted = locale.script() == QLocale::TraditionalChineseScript || locale.territory() == QLocale::Taiwan ||
                                 locale.territory() == QLocale::HongKong
                         ? "Chinese Traditional"
                         : "Chinese Simplified";
        break;
    case QLocale::Portuguese:
        wanted = locale.territory() == QLocale::Brazil ? "Brazilian Portuguese" : "Portuguese";
        break;
    case QLocale::Spanish:
        wanted = locale.territory() == QLocale::Mexico ? "Spanish (Mexico)" : "Spanish";
        break;
    case QLocale::French:
        wanted = locale.territory() == QLocale::Canada ? "French Canadian" : "French";
        break;
    case QLocale::Serbian:
        wanted = "Serbian Latin";
        break;
    case QLocale::NorwegianBokmal:
    case QLocale::NorwegianNynorsk:
        wanted = "Norwegian";
        break;
    default:
        wanted = QLocale::languageToString(locale.language());
    }
    if (available.contains(wanted)) return wanted;
    return available.contains("English") ? QString("English") : available.value(0);
}

IsoLibrary::Release IsoLibrary::windowsRelease(const Source &source) const {
    if (source.version.isEmpty() || !source.hashes.contains(source.language)) return {};
    // Named like the ISOs from Microsoft's website, so either kind is recognized.
    const auto language = QString(source.language).remove(QRegularExpression("[^A-Za-z]"));
    return {source.version, "Win11_" + source.version + "_" + language + "_x64.iso", {},
            source.hashes.value(source.language), 0};
}

void IsoLibrary::setLanguage(const QString &id, const QString &language) {
    auto s = find(id);
    if (!s || jobs_.contains(id) || (!s->languages.isEmpty() && !s->languages.contains(language))) return;
    s->language = language;
    if (s->lookup.value("type") == "evaluation") {
        if (s->links.contains(language) && s->status != "checking") {
            s->status = "checking";
            ++pending_;
            resolveEvaluation(id);
        }
    } else if (!s->version.isEmpty()) {
        s->latest = windowsRelease(*s);
        s->releases = {s->latest};
    }
    emit changed();
}

// Microsoft hands out a download link only to a "session" that has been through the same checks its
// website runs in a browser: register the session, answer the ov-df challenge, then ask for the
// language's SKU and its link (which works for 24 hours).
void IsoLibrary::downloadWindows(const QString &id) {
    auto s = find(id);
    if (!s) return;
    const auto session = QUuid::createUuid().toString(QUuid::WithoutBraces), edition = s->edition,
               language = s->language, page = s->lookup.value("page").toString();
    const auto release = s->latest;
    const QString profile = "606624d44113", instance = "560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175";
    const QList<QPair<QByteArray, QByteArray>> browser{{"User-Agent", browserAgent}};
    s->status = "starting";
    s->error.clear();
    emit changed();
    auto failed = [this, id](bool ok, const QString &why) {
        if (auto s = find(id)) s->status = "ready";
        fail(id, ok ? why : offline);
    };
    get(
            QUrl("https://vlscppe.microsoft.com/tags?org_id=y6jn8c31&session_id=" + session),
            [=, this](bool ok, const QByteArray &, const QUrl &) {
                if (!ok) {
                    failed(false, {});
                    return;
                }
                get(
                        QUrl("https://ov-df.microsoft.com/mdt.js?instanceId=" + instance +
                                "&PageId=si&session_id=" + session),
                        [=, this](bool ok, const QByteArray &script, const QUrl &) {
                            const auto text = QString::fromUtf8(script);
                            const auto w = QRegularExpression("[?&]w=([A-F0-9]+)").match(text).captured(1),
                                       ticks = QRegularExpression(R"(rticks=\"\+?([0-9]+))").match(text).captured(1);
                            if (!ok || w.isEmpty() || ticks.isEmpty()) {
                                failed(ok, "Microsoft's download check has changed, so OmaWare can't download Windows "
                                           "right now. Use Microsoft's website instead.");
                                return;
                            }
                            const auto reply = "https://ov-df.microsoft.com/?session_id=" + session +
                                               "&CustomerId=" + instance + "&PageId=si&w=" + w +
                                               "&mdt=" + QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                               "&rticks=" + ticks;
                            get(
                                    QUrl(reply),
                                    [=, this](bool ok, const QByteArray &, const QUrl &) {
                                        if (!ok) {
                                            failed(false, {});
                                            return;
                                        }
                                        const auto skus =
                                                "https://www.microsoft.com/software-download-connector/api/"
                                                "getskuinformationbyproductedition?profile=" +
                                                profile + "&ProductEditionId=" + edition +
                                                "&SKU=undefined&friendlyFileName=undefined&Locale=en-US&sessionID=" +
                                                session;
                                        get(
                                                QUrl(skus),
                                                [=, this](bool ok, const QByteArray &data, const QUrl &) {
                                                    const auto sku = ok ? windowsSku(data, language) : QString{};
                                                    if (sku.isEmpty()) {
                                                        failed(ok, "Microsoft didn't offer Windows 11 in " + language +
                                                                           ".");
                                                        return;
                                                    }
                                                    const auto links =
                                                            "https://www.microsoft.com/software-download-connector/api/"
                                                            "GetProductDownloadLinksBySku?profile=" +
                                                            profile + "&productEditionId=undefined&SKU=" + sku +
                                                            "&friendlyFileName=undefined&Locale=en-US&sessionID=" +
                                                            session;
                                                    get(QUrl(links),
                                                            [=, this](bool ok, const QByteArray &data, const QUrl &) {
                                                                QString why;
                                                                const auto link =
                                                                        ok ? windowsLink(data, why) : QString{};
                                                                if (link.isEmpty()) {
                                                                    failed(ok, why);
                                                                    return;
                                                                }
                                                                if (auto s = find(id)) s->status = "ready";
                                                                fetch(id, QUrl(link), release.checksum, release.file,
                                                                        release.size);
                                                            },
                                                            0,
                                                            {{"User-Agent", browserAgent}, {"Referer", page.toUtf8()}});
                                                },
                                                0, browser);
                                    },
                                    0, browser);
                        },
                        0, browser);
            },
            0, browser);
}

// ---- Downloads --------------------------------------------------------------------------------
bool IsoLibrary::download(const QString &id) {
    auto source = find(id);
    if (!source || source->kind != "download" || jobs_.contains(id) || source->status == "starting") return false;
    if (source->lookup.value("type") == "microsoft") {
        if (source->edition.isEmpty() || source->latest.checksum.isEmpty()) return false;
        downloadWindows(id);
        return true;
    }
    const auto release = selected(*source);
    if (release.url.isEmpty()) return false;
    return fetch(id, QUrl(release.url), release.checksum, release.file, release.size, release.algorithm);
}

bool IsoLibrary::fetch(const QString &id, const QUrl &url, const QString &checksum, const QString &file, qint64 size,
        const QString &algorithm) {
    if (jobs_.contains(id)) return false;
    // Only a plain .iso (or compressed .iso.bz2, or a .7z VM image) name inside the library folder, never a path.
    const bool packed = file.endsWith(".iso.bz2", Qt::CaseInsensitive),
               image = file.endsWith(".7z", Qt::CaseInsensitive);
    if (QFileInfo(file).fileName() != file || file.startsWith('.') ||
            !(file.endsWith(".iso", Qt::CaseInsensitive) || packed || image)) {
        fail(id, "The release has an unexpected file name.");
        return false;
    }
    if (algorithm.isEmpty()) {
        // Downloading without a checksum is only for sources marked unverified, whose cards say so.
        const auto s = find(id);
        if (!s || s->trustNote.isEmpty()) {
            fail(id, "The release has no valid checksum, so it can't be verified.");
            return false;
        }
    } else if (!hashOf(algorithm) || algorithmFor(checksum.toLower()) != algorithm) {
        fail(id, "The release has no valid checksum, so it can't be verified.");
        return false;
    }
    QDir().mkpath(folder_);
    const qint64 needed = size > 0 ? size * (packed ? 3 : image ? 6 : 1) + qint64(512) * 1024 * 1024 : 0;
    if (needed > 0 && QStorageInfo(folder_).bytesAvailable() < needed) {
        fail(id, "There isn't enough free space in " + folder_ + ".");
        return false;
    }
    auto job = new Job;
    job->checksum = checksum.toLower();
    job->algorithm = algorithm;
    job->file = file;
    job->total = size;
    if (const auto kind = hashOf(algorithm)) job->hash = std::make_unique<QCryptographicHash>(*kind);
    job->out = std::make_unique<QFile>(folder_ + "/" + file + ".part");
    if (!job->out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete job;
        fail(id, "Couldn't write to " + folder_ + ".");
        return false;
    }
    job->url = url;
    job->sample.start();
    jobs_[id] = job;
    if (auto s = find(id)) s->error.clear();
    request(id);
    emit changed();
    return true;
}

// Starts (or, after a dropped connection, continues) a download. A resumed request asks for the rest of
// the file; a server that sends the whole file again starts it over, so the checksum still covers every byte.
void IsoLibrary::request(const QString &id) {
    auto job = jobs_.value(id);
    if (!job) return;
    job->waiting = false;
    QNetworkRequest http(job->url);
    http.setHeader(QNetworkRequest::UserAgentHeader, "OmaWare/" + QCoreApplication::applicationVersion());
    http.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    job->offset = job->received;
    job->checkResume = job->received > 0;
    job->stalled = false;
    job->quiet.start();
    // A server can stop sending without closing the connection; a minute of silence counts as a dropped one.
    if (!stallWatch_) {
        stallWatch_ = new QTimer(this);
        stallWatch_->setInterval(5000);
        connect(stallWatch_, &QTimer::timeout, this, [this] {
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                auto job = it.value();
                if (job->unpacking || job->waiting || !job->reply) continue;
                if (job->received != job->lastSeen) {
                    job->lastSeen = job->received;
                    job->quiet.restart();
                    continue;
                }
                if (job->quiet.elapsed() >= stallLimit()) {
                    job->stalled = true;
                    job->reply->abort();
                }
            }
        });
    }
    stallWatch_->start();
    if (job->checkResume) http.setRawHeader("Range", "bytes=" + QByteArray::number(job->received) + "-");
    job->reply = network_.get(http);
    auto reply = job->reply.data();
    connect(reply, &QNetworkReply::readyRead, this, [this, id, reply] {
        auto job = jobs_.value(id);
        if (!job || job->reply != reply) return;
        if (job->checkResume) {
            job->checkResume = false;
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) {
                job->out->resize(0);
                job->out->seek(0);
                if (job->hash) job->hash->reset();
                job->received = job->offset = job->lastBytes = 0;
            }
        }
        const auto data = reply->readAll();
        if (job->hash) job->hash->addData(data);
        if (job->out->write(data) != data.size()) {
            cancel(id);
            fail(id, "Writing the download failed. Is the disk full?");
            return;
        }
        job->received += data.size();
        if (job->sample.elapsed() >= 500) {
            job->rate = (job->received - job->lastBytes) * 1000 / std::max<qint64>(1, job->sample.elapsed());
            job->lastBytes = job->received;
            job->sample.restart();
            emit changed();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, id, reply](qint64, qint64 total) {
        if (auto job = jobs_.value(id); job && job->reply == reply && total > 0) job->total = job->offset + total;
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply] {
        auto job = jobs_.value(id);
        reply->deleteLater();
        if (!job || job->reply != reply || job->unpacking) return;
        const auto error = reply->error();
        if (error == QNetworkReply::NoError) {
            if (job->checkResume) {
                job->checkResume = false;
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) {
                    job->out->resize(0);
                    job->out->seek(0);
                    if (job->hash) job->hash->reset();
                    job->received = 0;
                }
            }
            const auto rest = reply->readAll();
            if (job->hash) job->hash->addData(rest);
            job->out->write(rest);
            job->received += rest.size();
            complete(id);
            return;
        }
        const auto part = job->out->fileName();
        // A dropped connection (common on slow mirrors) is picked up again where it stopped.
        // Retries only run out when a server sends nothing at all; a slow one that keeps moving can finish.
        if (job->received > job->offset) job->retries = 0;
        const bool retry = (job->stalled || (error != QNetworkReply::OperationCanceledError &&
                                                    error != QNetworkReply::ContentNotFoundError &&
                                                    error != QNetworkReply::ContentAccessDenied)) &&
                           job->retries < 5;
        if (retry) {
            if (!job->checkResume) {
                const auto rest = reply->readAll();
                if (job->hash) job->hash->addData(rest);
                job->out->write(rest);
                job->received += rest.size();
            }
            ++job->retries;
            job->waiting = true;
            job->reply = nullptr;
            emit changed();
            QTimer::singleShot(2000 * job->retries, this, [this, id] {
                if (auto job = jobs_.value(id); job && job->waiting) request(id);
            });
            return;
        }
        const bool cancelled = error == QNetworkReply::OperationCanceledError && !job->stalled;
        const auto why = job->stalled ? QString("the server stopped sending data") : reply->errorString();
        job->out->close();
        QFile::remove(part);
        jobs_.remove(id);
        delete job;
        if (!cancelled)
            fail(id, "The download failed: " + why);
        else
            emit changed();
    });
}

void IsoLibrary::complete(const QString &id) {
    auto job = jobs_.value(id);
    if (!job) return;
    job->out->close();
    const auto part = job->out->fileName(), final = folder_ + "/" + job->file;
    auto drop = [&] {
        jobs_.remove(id);
        delete job;
    };
    if (job->hash && QString::fromLatin1(job->hash->result().toHex()) != job->checksum) {
        QFile::remove(part);
        drop();
        fail(id, "The downloaded file doesn't match its published checksum, so it was deleted. Try again.");
        return;
    }
    QFile::remove(final);
    if (!QFile::rename(part, final)) {
        QFile::remove(part);
        drop();
        fail(id, "The finished download couldn't be saved.");
        return;
    }
    if (final.endsWith(".bz2", Qt::CaseInsensitive)) {
        unpack(id, job, final);
        return;
    }
    if (final.endsWith(".7z", Qt::CaseInsensitive)) {
        extract(id, job, final);
        return;
    }
    const auto name = find(id) ? find(id)->name : job->file, file = job->file;
    remember(id, file, job->algorithm);
    drop();
    rescan();
    emit finished(id, true, name + " is ready: " + file);
}

// Verified compressed images are unpacked next to themselves; the compressed copy is then removed.
void IsoLibrary::unpack(const QString &id, Job *job, const QString &packed) {
    job->unpacking = true;
    const auto target = packed.left(packed.size() - 4);
    auto process = new QProcess(this);
    job->unpacker = process;
    process->setStandardOutputFile(target + ".part");
    const auto algorithm = job->algorithm;
    connect(process, &QProcess::finished, this,
            [this, id, job, process, packed, target, algorithm](int code, QProcess::ExitStatus status) {
                process->deleteLater();
                const bool ok = status == QProcess::NormalExit && code == 0;
                QFile::remove(packed);
                jobs_.remove(id);
                const auto name = find(id) ? find(id)->name : QFileInfo(target).fileName();
                delete job;
                if (!ok) {
                    QFile::remove(target + ".part");
                    fail(id, "The download couldn't be unpacked. Is bzip2 installed?");
                    return;
                }
                QFile::remove(target);
                QFile::rename(target + ".part", target);
                remember(id, QFileInfo(target).fileName(), algorithm);
                rescan();
                emit finished(id, true, name + " is ready: " + QFileInfo(target).fileName());
            });
    connect(process, &QProcess::errorOccurred, this, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) emit process->finished(127, QProcess::CrashExit);
    });
    process->start("bzip2", {"-dc", packed});
    emit changed();
}

// A downloaded VM image (Kali's .7z) holds one qcow2 disk: it is unpacked into appliances/, named after the
// download so the shop recognizes it, and the archive is removed.
void IsoLibrary::extract(const QString &id, Job *job, const QString &packed) {
    job->unpacking = true;
    const bool created = !QFileInfo::exists(applianceFolder_);
    QDir().mkpath(applianceFolder_);
    // Agent provisioning refuses shared-writable libraries.
    if (created) QFile::setPermissions(applianceFolder_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto target = applianceFolder_ + "/" + QFileInfo(packed).completeBaseName() + ".qcow2";
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto error = std::make_shared<QString>();
    job->stop = stop;
    auto thread = QThread::create([packed, target, stop, error] {
        auto archive = archive_read_new();
        archive_read_support_format_7zip(archive);
        archive_read_support_filter_all(archive);
        if (archive_read_open_filename(archive, QFile::encodeName(packed).constData(), 1 << 20) != ARCHIVE_OK) {
            *error = "The VM image couldn't be opened.";
            archive_read_free(archive);
            return;
        }
        archive_entry *entry = nullptr;
        bool found = false;
        while (!found && archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
            if (archive_entry_filetype(entry) != AE_IFREG ||
                    !QString::fromUtf8(archive_entry_pathname(entry)).endsWith(".qcow2", Qt::CaseInsensitive))
                continue;
            found = true;
            QFile out(target + ".part");
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                *error = "Couldn't write the VM disk: " + out.errorString();
                break;
            }
            out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            const void *block = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            int status = ARCHIVE_OK;
            while (!*stop && (status = archive_read_data_block(archive, &block, &size, &offset)) == ARCHIVE_OK) {
                // Holes in the image stay holes on disk.
                if (out.pos() != offset && !out.seek(offset)) {
                    *error = "Couldn't write the VM disk: " + out.errorString();
                    break;
                }
                if (out.write(static_cast<const char *>(block), qint64(size)) != qint64(size)) {
                    *error = "Couldn't write the VM disk: " + out.errorString();
                    break;
                }
            }
            if (*stop)
                *error = "cancelled";
            else if (error->isEmpty() && status != ARCHIVE_EOF)
                *error = "The VM image is damaged: " + QString::fromUtf8(archive_error_string(archive));
            if (error->isEmpty() && out.size() < offset + qint64(size)) out.resize(offset + qint64(size));
            out.close();
        }
        if (!found && error->isEmpty()) *error = "The download has no VM disk (qcow2) in it.";
        archive_read_free(archive);
    });
    job->unpacker = thread;
    const auto algorithm = job->algorithm;
    connect(thread, &QThread::finished, this, [this, id, job, thread, packed, target, error, algorithm] {
        thread->deleteLater();
        QFile::remove(packed);
        jobs_.remove(id);
        const auto name = find(id) ? find(id)->name : QFileInfo(target).fileName();
        delete job;
        if (!error->isEmpty()) {
            QFile::remove(target + ".part");
            if (*error == "cancelled") {
                emit changed();
                return;
            }
            fail(id, *error);
            return;
        }
        QFile::remove(target);
        QFile::rename(target + ".part", target);
        remember(id, QFileInfo(target).fileName(), algorithm);
        rescan();
        emit finished(id, true, name + " is ready: " + QFileInfo(target).fileName());
    });
    thread->start();
    emit changed();
}

// Notes how a finished download was checked, and keeps an earlier version picked on purpose.
void IsoLibrary::remember(const QString &id, const QString &file, const QString &algorithm) {
    checked_[file] = algorithm;
    const auto s = find(id);
    if (s && !s->latest.version.isEmpty() && !s->chosen.isEmpty() && s->chosen != s->latest.version &&
            !kept_.contains(file))
        kept_ << file;
    saveMeta();
}

void IsoLibrary::cancel(const QString &id) {
    auto job = jobs_.value(id);
    if (!job) return;
    if (job->unpacking) {
        if (auto p = qobject_cast<QProcess *>(job->unpacker))
            p->kill();
        else if (job->stop)
            *job->stop = true;
        return;
    }
    if (job->reply) {
        job->reply->abort();
        return;
    }
    // Waiting to reconnect after a dropped connection: nothing is in flight, so stop here.
    job->out->close();
    QFile::remove(job->out->fileName());
    jobs_.remove(id);
    delete job;
    emit changed();
}

void IsoLibrary::fail(const QString &id, const QString &message) {
    if (auto s = find(id)) s->error = message;
    emit changed();
    emit finished(id, false, message);
}

namespace {
// Only plain .iso files directly inside the folder, never links or anything being downloaded.
bool deletable(const QString &folder, const QString &name, const QStringList &busy) {
    const QFileInfo info(folder + "/" + name);
    return QFileInfo(name).fileName() == name && !ApplianceImport::mediaType(name).isEmpty() && !info.isSymLink() &&
           info.isFile() && !busy.contains(name) && !busy.contains(name + ".bz2");
}
}

bool IsoLibrary::remove(const QString &name) {
    return removeAll({name}) == 1;
}

int IsoLibrary::removeAll(const QStringList &names) {
    QStringList busy;
    for (auto job : jobs_)
        busy << job->file;
    int removed = 0;
    for (const auto &name : names) {
        for (const auto &entry : files_) {
            const auto row = entry.toMap();
            if (row["name"].toString() != name) continue;
            const auto folder = QFileInfo(row["path"].toString()).absolutePath();
            if ((folder == QDir(folder_).absolutePath() || folder == QDir(applianceFolder_).absolutePath()) &&
                    deletable(folder, name, busy) && QFile::remove(folder + "/" + name)) {
                ++removed;
                kept_.removeAll(name);
                checked_.remove(name);
            }
            break;
        }
    }
    if (removed) saveMeta();
    rescan();
    return removed;
}

// ---- Adding ISOs from elsewhere ----
namespace {
QString freeName(const QString &folder, const QString &name) {
    const auto taken = [&](const QString &n) {
        return QFileInfo::exists(folder + "/" + n) || QFileInfo::exists(folder + "/" + n + ".part");
    };
    if (!taken(name)) return name;
    const QFileInfo info(name);
    for (int i = 2;; ++i) {
        const auto candidate = QString("%1 (%2).%3").arg(info.completeBaseName()).arg(i).arg(info.suffix());
        if (!taken(candidate)) return candidate;
    }
}

bool sameFile(const QString &a, const QString &b) {
    struct stat x{}, y{};
    return ::stat(QFile::encodeName(a).constData(), &x) == 0 && ::stat(QFile::encodeName(b).constData(), &y) == 0 &&
           x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
}

QString IsoLibrary::importName() const {
    return import_ ? import_->names.value(import_->current.load()) : QString();
}

double IsoLibrary::importProgress() const {
    return import_ && import_->total > 0 ? double(import_->copied.load()) / double(import_->total) : 0;
}

bool IsoLibrary::importFiles(const QVariantList &urls) {
    if (import_) {
        emit imported({}, false, "A file is still being copied. Try again when it's done.");
        return false;
    }
    QDir().mkpath(folder_);
    auto job = std::make_shared<Import>();
    for (const auto &value : urls) {
        const QUrl url(value.toString());
        const QFileInfo info(url.isLocalFile() ? url.toLocalFile() : value.toString());
        const auto type = ApplianceImport::mediaType(info.fileName());
        if (info.isSymLink() || !info.isFile() || type.isEmpty()) {
            job->skipped << (info.fileName().isEmpty() ? value.toString() : info.fileName());
            continue;
        }
        const auto destination = type == "iso" ? folder_ : applianceFolder_;
        const bool created = !QFileInfo::exists(destination);
        if (!QDir().mkpath(destination)) {
            job->error = "Could not create media folder.";
            continue;
        }
        // Agent provisioning refuses shared-writable libraries, which a umask of 002 would otherwise create.
        if (created && type != "iso")
            QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const auto here = QFileInfo(destination).canonicalFilePath();
        if (info.canonicalPath() == here) {
            job->already << info.absoluteFilePath();
            continue;
        }
        // The same file (or one with the same name and size) is already in the folder.
        const QFileInfo existing(destination + "/" + info.fileName());
        if (!existing.isSymLink() && existing.isFile() &&
                (sameFile(existing.filePath(), info.filePath()) || (type == "iso" && existing.size() == info.size()))) {
            job->already << existing.absoluteFilePath();
            continue;
        }
        const auto name = freeName(destination, info.fileName());
        const auto target = destination + "/" + name;
        // On the same disk a hard link is instant and shares the space; otherwise copy it.
        if (type == "iso" && ::link(QFile::encodeName(info.absoluteFilePath()).constData(),
                                     QFile::encodeName(target).constData()) == 0) {
            job->paths << target;
            ++job->linked;
            continue;
        }
        job->from << info.absoluteFilePath();
        job->to << target;
        job->names << name;
        job->total += info.size();
    }
    if (job->total > 0 && job->total > QStorageInfo(folder_).bytesAvailable()) {
        job->error = QString("There isn't enough free space in %1 to copy %2.")
                             .arg(folder_, job->names.size() == 1 ? job->names.first() : "these ISOs");
        job->from.clear();
        job->to.clear();
        job->names.clear();
        job->total = 0;
    }
    import_ = job;
    if (job->from.isEmpty()) {
        QMetaObject::invokeMethod(this, &IsoLibrary::finishImport, Qt::QueuedConnection);
        emit changed();
        return !job->paths.isEmpty() || !job->already.isEmpty();
    }
    importThread_ = QThread::create([job] {
        QByteArray buffer(4 << 20, Qt::Uninitialized);
        for (int i = 0; i < job->from.size() && !job->cancel; ++i) {
            job->current = i;
            QFile in(job->from[i]), out(job->to[i] + ".part");
            if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                job->error = QString("Couldn't copy %1: %2")
                                     .arg(job->names[i], in.isOpen() ? out.errorString() : in.errorString());
                return;
            }
            out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            QString failure;
            while (!job->cancel) {
                const auto n = in.read(buffer.data(), buffer.size());
                if (n < 0) {
                    failure = in.errorString();
                    break;
                }
                if (n == 0) break;
                if (out.write(buffer.constData(), n) != n) {
                    failure = out.errorString();
                    break;
                }
                job->copied += n;
            }
            out.close();
            if (!failure.isEmpty() || job->cancel || !out.rename(job->to[i])) {
                if (failure.isEmpty() && !job->cancel) failure = out.errorString();
                out.remove();
                if (!failure.isEmpty()) job->error = QString("Couldn't copy %1: %2").arg(job->names[i], failure);
                return;
            }
            job->copiedFiles = i + 1;
        }
    });
    connect(importThread_, &QThread::finished, this, &IsoLibrary::finishImport);
    if (!importTicker_) {
        importTicker_ = new QTimer(this);
        importTicker_->setInterval(250);
        connect(importTicker_, &QTimer::timeout, this, &IsoLibrary::changed);
    }
    importTicker_->start();
    importThread_->start();
    emit changed();
    return true;
}

void IsoLibrary::cancelImport() {
    if (import_) import_->cancel = true;
}

void IsoLibrary::finishImport() {
    if (!import_) return;
    if (importThread_) {
        importThread_->wait();
        importThread_->deleteLater();
        importThread_ = nullptr;
    }
    if (importTicker_) importTicker_->stop();
    const auto job = std::move(import_);
    import_.reset();
    auto paths = job->paths;
    for (int i = 0; i < job->copiedFiles.load(); ++i)
        paths << job->to[i];
    const auto added = paths.size();
    paths << job->already;
    const auto file = [](const QString &path) {
        return QFileInfo(path).fileName();
    };
    QStringList message;
    if (added == 1)
        message << QString("Added %1 to your media.").arg(file(paths.first()));
    else if (added > 1)
        message << QString("Added %1 files.").arg(added);
    if (job->already.size() == 1)
        message << QString("%1 is already in your media.").arg(file(job->already.first()));
    else if (job->already.size() > 1)
        message << QString("%1 were already in your media.").arg(job->already.size());
    if (job->skipped.size() == 1)
        message << QString("Only ISO, OVA and QCOW2 files can be added, so %1 was skipped.").arg(job->skipped.first());
    else if (job->skipped.size() > 1)
        message << QString("Only ISO, OVA and QCOW2 files can be added, so %1 other files were skipped.")
                           .arg(job->skipped.size());
    if (job->cancel) message << "Copying was cancelled.";
    if (!job->error.isEmpty()) message << job->error;
    rescan();
    emit imported(paths, !paths.isEmpty() && job->error.isEmpty() && !job->cancel, message.join(' '));
}
