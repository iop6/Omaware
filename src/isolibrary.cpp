// SPDX-License-Identifier: GPL-3.0-or-later
#include "isolibrary.h"
#include "paths.h"
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Fills {codename}, {version}, {major} and similar placeholders in a URL or preset template.
QString fill(QString text, const QHash<QString, QString> &values) {
    for (auto it = values.cbegin(); it != values.cend(); ++it) text.replace("{" + it.key() + "}", it.value());
    return text;
}
QString majorOf(const QString &version) { return version.section('.', 0, 0); }
const QString offline = "Couldn't reach the publisher. Check your internet connection and try again.";
// How long a download may receive nothing before it is treated as a dropped connection (tests shorten it).
qint64 stallLimit() { const int seconds = qEnvironmentVariableIntValue("OMAWARE_STALL_SECONDS"); return seconds > 0 ? seconds * 1000LL : 60000; }
// Microsoft's download service expects the requests its website makes in a browser.
const QByteArray browserAgent = "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";
}

IsoLibrary::IsoLibrary(QObject *parent) : QObject(parent) {
    // id, name, description, category, badge color, local file pattern (captures the version), preset template, lookup.
    auto add = [this](QString id, QString name, QString description, QString category, QString color, QString pattern, QString preset, QVariantMap lookup) {
        Source source;
        source.id = id; source.name = name; source.description = description; source.category = category; source.color = color;
        source.pattern = pattern; source.preset = preset; source.lookup = lookup;
        if (lookup.value("type") == "page") source.kind = "page";
        source.page = lookup.value("page").toString(); source.note = lookup.value("note").toString();
        sources_.append(source);
    };
    const QString ubuntu = "https://releases.ubuntu.com/{codename}/";
    // Desktop
    add("ubuntu-desktop", "Ubuntu Desktop", "The most popular Linux desktop. Latest long-term support (LTS) release.", "desktop", "#E95420",
        R"(^ubuntu-([0-9][0-9.]*)-desktop-amd64\.iso$)", "ubuntu{mm}", {{"type", "ubuntu"}, {"base", ubuntu}});
    add("kubuntu", "Kubuntu", "Ubuntu with the KDE Plasma desktop. Latest LTS release.", "desktop", "#0079C1",
        R"(^kubuntu-([0-9][0-9.]*)-desktop-amd64\.iso$)", "ubuntu{mm}", {{"type", "ubuntu"}, {"base", "https://cdimage.ubuntu.com/kubuntu/releases/{codename}/release/"}});
    add("xubuntu", "Xubuntu", "Ubuntu with the light Xfce desktop, good for smaller VMs. Latest LTS release.", "desktop", "#2284F2",
        R"(^xubuntu-([0-9][0-9.]*)-desktop-amd64\.iso$)", "ubuntu{mm}", {{"type", "ubuntu"}, {"base", "https://cdimage.ubuntu.com/xubuntu/releases/{codename}/release/"}});
    add("linux-mint", "Linux Mint", "A friendly, Windows-like desktop based on Ubuntu (Cinnamon edition).", "desktop", "#87CF3E",
        R"(^linuxmint-([0-9][0-9.]*)-cinnamon-64bit\.iso$)", "linuxmint{m}",
        {{"type", "folder"}, {"index", "https://mirrors.edge.kernel.org/linuxmint/stable/"}, {"sums", "https://mirrors.edge.kernel.org/linuxmint/stable/{version}/sha256sum.txt"}, {"base", "https://mirrors.edge.kernel.org/linuxmint/stable/{version}/"}});
    add("fedora-workstation", "Fedora Workstation", "Up-to-date software with the GNOME desktop.", "desktop", "#51A2DA",
        R"(^Fedora-Workstation-Live-(?:x86_64-)?([0-9]+)-[0-9.]+(?:\.x86_64)?\.iso$)", "fedora{m}", {{"type", "fedora"}, {"variant", "Workstation"}});
    add("fedora-kde", "Fedora KDE Plasma", "Fedora with the KDE Plasma desktop.", "desktop", "#3C6EB4",
        R"(^Fedora-KDE-[A-Za-z-]*?(?:x86_64-)?([0-9]+)-[0-9.]+(?:\.x86_64)?\.iso$)", "fedora{m}", {{"type", "fedora"}, {"variant", "KDE"}});
    add("debian", "Debian", "The rock-solid base of Ubuntu and many others. Small installer that downloads the rest.", "desktop", "#D70A53",
        R"(^debian-([0-9][0-9.]*)-amd64-netinst\.iso$)", "debian{m}",
        {{"type", "sums"}, {"sums", "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/SHA256SUMS"}, {"base", "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/"}});
    add("arch", "Arch Linux", "A minimal, always-current system you build up yourself. New image every month.", "desktop", "#1793D1",
        R"(^archlinux-([0-9][0-9.]*)-x86_64\.iso$)", "archlinux",
        {{"type", "sums"}, {"sums", "https://geo.mirror.pkgbuild.com/iso/latest/sha256sums.txt"}, {"base", "https://geo.mirror.pkgbuild.com/iso/latest/"}});
    add("opensuse-tumbleweed", "openSUSE Tumbleweed", "A rolling release with the newest software, tested before each snapshot.", "desktop", "#73BA25",
        R"(^openSUSE-Tumbleweed-DVD-x86_64-Snapshot([0-9]+)-Media\.iso$)", "opensusetumbleweed",
        {{"type", "sums"}, {"sums", "https://download.opensuse.org/tumbleweed/iso/openSUSE-Tumbleweed-DVD-x86_64-Current.iso.sha256"}, {"base", "https://download.opensuse.org/tumbleweed/iso/"}});
    add("pop-os", "Pop!_OS", "System76's polished desktop based on Ubuntu, with the COSMIC desktop.", "desktop", "#48B9C7",
        R"(^pop-os_([0-9][0-9.]*)_amd64_intel_[0-9]+\.iso$)", "popos{mm}", {{"type", "popos"}});
    add("nixos", "NixOS", "A Linux system configured entirely from one file; easy to reproduce and roll back.", "desktop", "#5277C3",
        R"(^nixos-graphical-([0-9][0-9.]*)\.[0-9a-f]+-x86_64-linux\.iso$)", "nixos-{mm}", {{"type", "nixos"}});
    // Server
    add("ubuntu-server", "Ubuntu Server", "Ubuntu without a desktop, for servers. Latest LTS release.", "server", "#E95420",
        R"(^ubuntu-([0-9][0-9.]*)-live-server-amd64\.iso$)", "ubuntu{mm}", {{"type", "ubuntu"}, {"base", ubuntu}});
    add("rocky", "Rocky Linux", "A free rebuild of Red Hat Enterprise Linux. Network installer.", "server", "#10B981",
        R"(^Rocky-([0-9][0-9.]*)-x86_64-boot\.iso$)", "rocky{m}",
        {{"type", "folder"}, {"index", "https://download.rockylinux.org/pub/rocky/"}, {"sums", "https://download.rockylinux.org/pub/rocky/{major}/isos/x86_64/CHECKSUM"}, {"base", "https://download.rockylinux.org/pub/rocky/{major}/isos/x86_64/"}});
    add("almalinux", "AlmaLinux", "Another free Red Hat Enterprise Linux rebuild. Network installer.", "server", "#0F4266",
        R"(^AlmaLinux-([0-9][0-9.]*)-x86_64-boot\.iso$)", "almalinux{m}",
        {{"type", "folder"}, {"index", "https://repo.almalinux.org/almalinux/"}, {"sums", "https://repo.almalinux.org/almalinux/{major}/isos/x86_64/CHECKSUM"}, {"base", "https://repo.almalinux.org/almalinux/{major}/isos/x86_64/"}});
    add("alpine", "Alpine Linux", "A tiny, security-minded system; this edition is made for VMs.", "server", "#0D597F",
        R"(^alpine-virt-([0-9][0-9.]*)-x86_64\.iso$)", "alpinelinux{mm}", {{"type", "alpine"}, {"flavor", "alpine-virt"}});
    add("proxmox", "Proxmox VE", "A virtualization platform for running VMs and containers on a server.", "server", "#E57000",
        R"(^proxmox-ve_([0-9][0-9.-]*)\.iso$)", "",
        {{"type", "sums"}, {"sums", "https://enterprise.proxmox.com/iso/SHA256SUMS"}, {"base", "https://enterprise.proxmox.com/iso/"}});
    add("freebsd", "FreeBSD", "A complete BSD operating system, known for networking and storage.", "server", "#AB2B28",
        R"(^FreeBSD-([0-9][0-9.]*)-RELEASE-amd64-disc1\.iso$)", "freebsd{mm}",
        {{"type", "folder"}, {"index", "https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/"}, {"sums", "https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/{version}/CHECKSUM.SHA256-FreeBSD-{version}-RELEASE-amd64"}, {"base", "https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/{version}/"}});
    // Security & networking
    add("kali", "Kali Linux", "Security testing tools, ready to use.", "security", "#367BF0",
        R"(^kali-linux-([0-9][0-9.]*)-installer-amd64\.iso$)", "",
        {{"type", "sums"}, {"sums", "https://cdimage.kali.org/current/SHA256SUMS"}, {"base", "https://cdimage.kali.org/current/"}});
    add("opnsense", "OPNsense", "A firewall and router with a web interface; can route your VMs to the internet.", "security", "#D94F00",
        R"(^OPNsense-([0-9][0-9.]*)-dvd-amd64\.iso$)", "",
        {{"type", "folder"}, {"index", "https://pkg.opnsense.org/releases/"}, {"folder", R"(^[0-9]+\.[0-9]+$)"}, {"sums", "https://pkg.opnsense.org/releases/{version}/OPNsense-{version}-checksums-amd64.sha256"},
         // The checksum comes from OPNsense's own server; the image from an official mirror, which is much faster.
         {"base", "https://mirror.wdc1.us.leaseweb.net/opnsense/releases/{version}/"}, {"remote", R"(^OPNsense-([0-9][0-9.]*)-dvd-amd64\.iso\.bz2$)"}});
    add("pfsense", "pfSense CE", "A popular firewall and router. Netgate offers it through its store (free, with a Netgate account).", "security", "#212121",
        R"(^(?:pfSense-CE|netgate-installer)[-_]([0-9][0-9.]*).*\.iso$)", "",
        {{"type", "page"}, {"page", "https://www.pfsense.org/download/"}, {"note", "Netgate's installer downloads pfSense while installing, so the VM needs internet access during setup."}});
    // Windows
    // Straight from Microsoft's own download service, the way its website (and tools like Fido and Mido) get
    // the 24-hour link, and checked against the SHA-256 Microsoft publishes for each language.
    add("windows-11", "Windows 11", "The official multi-edition ISO from Microsoft, in your language. You need a Windows license to activate it.", "windows", "#0078D4",
        R"(^Win(?:dows)?11_(?:Client_x64_[A-Za-z-]+_)?([0-9][0-9A-Za-z_]*?)(?:_[A-Z][A-Za-z]*_x64)?(?:v[0-9])?\.iso$)", "win11",
        {{"type", "microsoft"}, {"page", "https://www.microsoft.com/en-us/software-download/windows11"},
         {"note", "Microsoft sometimes refuses automated downloads from some networks. If that happens, download it on Microsoft's website and save it in your ISO folder."}});
    setFolder(Paths::isos());
}
IsoLibrary::~IsoLibrary() {
    for (auto id : jobs_.keys()) cancel(id);
    if (importThread_) {
        import_->cancel = true;
        importThread_->disconnect(this);
        importThread_->wait();
        delete importThread_;
    }
}
QString IsoLibrary::root() const { return Paths::root(); }
void IsoLibrary::setFolder(const QString &folder) {
    folder_ = folder;
    QDir().mkpath(folder_);
    rescan();
}
IsoLibrary::Source *IsoLibrary::find(const QString &id) {
    for (auto &source : sources_) if (source.id == id) return &source;
    return nullptr;
}
const IsoLibrary::Source *IsoLibrary::find(const QString &id) const {
    for (const auto &source : sources_) if (source.id == id) return &source;
    return nullptr;
}
QStringList IsoLibrary::sourceIds() const {
    QStringList ids;
    for (const auto &source : sources_) ids << source.id;
    return ids;
}

bool IsoLibrary::newer(const QString &a, const QString &b) {
    const auto x = a.split(QRegularExpression("[^0-9]+"), Qt::SkipEmptyParts), y = b.split(QRegularExpression("[^0-9]+"), Qt::SkipEmptyParts);
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
        const auto preset = fill(source.preset, {{"v", version}, {"m", parts.value(0)}, {"mm", parts.mid(0, 2).join('.')}});
        return {{"source", source.id}, {"sourceName", source.name}, {"version", version}, {"preset", preset}};
    }
    return {};
}

void IsoLibrary::rescan() {
    QVariantList rows;
    QHash<QString, QString> newest;
    const auto entries = QDir(folder_).entryInfoList({"*.iso", "*.ISO"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const auto &info : entries) {
        auto row = identify(info.fileName());
        const auto source = row["source"].toString(), version = row["version"].toString();
        if (!source.isEmpty() && (!newest.contains(source) || newer(version, newest[source]))) newest[source] = version;
        row["name"] = info.fileName(); row["path"] = info.absoluteFilePath(); row["size"] = double(info.size());
        rows.append(row);
    }
    for (auto &entry : rows) {
        auto row = entry.toMap();
        const auto source = row["source"].toString();
        row["newest"] = source.isEmpty() || newest.value(source) == row["version"].toString();
        entry = row;
    }
    files_ = rows;
    emit filesChanged();
    emit changed();
}

QVariantList IsoLibrary::sources() const {
    QVariantList result;
    for (const auto &source : sources_) {
        QString have;
        for (const auto &entry : files_) {
            const auto row = entry.toMap();
            if (row["source"] == source.id && (have.isEmpty() || newer(row["version"].toString(), have))) have = row["version"].toString();
        }
        const auto job = jobs_.value(source.id);
        QVariantMap row{{"id", source.id}, {"name", source.name}, {"description", source.description}, {"category", source.category}, {"color", source.color},
            {"kind", source.kind}, {"page", source.page}, {"note", source.note},
            {"status", job ? QString(job->unpacking ? "unpacking" : "downloading") : source.status},
            {"error", source.error}, {"have", have}, {"version", source.latest.version}, {"file", source.latest.file}, {"url", source.latest.url}, {"size", double(source.latest.size)},
            {"upToDate", !have.isEmpty() && !source.latest.version.isEmpty() && !newer(source.latest.version, have)},
            {"updateAvailable", !have.isEmpty() && !source.latest.version.isEmpty() && newer(source.latest.version, have)}};
        if (job) { row["received"] = double(job->received); row["total"] = double(job->total); row["rate"] = double(job->rate); }
        if (!source.languages.isEmpty()) { row["languages"] = source.languages; row["language"] = source.language; }
        result.append(row);
    }
    return result;
}

// ---- Release lists ----------------------------------------------------------------------------
QString IsoLibrary::ubuntuLtsCodename(const QByteArray &metaRelease) {
    QString codename;
    for (const auto &block : QString::fromUtf8(metaRelease).split(QRegularExpression("\\n\\s*\\n"), Qt::SkipEmptyParts)) {
        QHash<QString, QString> fields;
        for (const auto &line : block.split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon > 0) fields[line.left(colon).trimmed()] = line.mid(colon + 1).trimmed();
        }
        // The list is oldest first; keep the newest supported release.
        if (fields.value("Supported") == "1" && QRegularExpression("^[a-z]+$").match(fields.value("Dist")).hasMatch()) codename = fields.value("Dist");
    }
    return codename;
}
IsoLibrary::Release IsoLibrary::fromChecksums(const QByteArray &sums, const QString &pattern, const QString &base) {
    Release best;
    const QRegularExpression gnu(R"(^([0-9a-fA-F]{64})\s+\*?(\S+)\s*$)"), bsd(R"(^SHA256\s*\((\S+)\)\s*=\s*([0-9a-fA-F]{64})\s*$)"), name(pattern);
    for (const auto &raw : QString::fromUtf8(sums).split('\n')) {
        const auto text = raw.trimmed();
        QString file, hash;
        if (auto m = gnu.match(text); m.hasMatch()) { hash = m.captured(1); file = m.captured(2); }
        else if (auto m = bsd.match(text); m.hasMatch()) { file = m.captured(1); hash = m.captured(2); }
        else continue;
        const auto version = name.match(file).captured(1);
        if (version.isEmpty() || (!best.version.isEmpty() && !newer(version, best.version))) continue;
        best = {version, file, base + file, hash.toLower(), 0};
    }
    return best;
}
IsoLibrary::Release IsoLibrary::fedora(const QByteArray &releasesJson, const QString &variant) {
    Release best;
    for (const auto &value : QJsonDocument::fromJson(releasesJson).array()) {
        const auto o = value.toObject();
        const auto version = o["version"].toString(), link = o["link"].toString();
        if (o["arch"].toString() != "x86_64" || o["variant"].toString() != variant || o["subvariant"].toString() != variant) continue;
        if (!QRegularExpression("^[0-9]+$").match(version).hasMatch() || !link.startsWith("https://") || !link.endsWith(".iso")) continue;
        if (!best.version.isEmpty() && !newer(version, best.version)) continue;
        const auto sha = o["sha256"].toString().toLower();
        if (!QRegularExpression("^[0-9a-f]{64}$").match(sha).hasMatch()) continue;
        best = {version, QUrl(link).fileName(), link, sha, o["size"].toString().toLongLong()};
    }
    return best;
}
IsoLibrary::Release IsoLibrary::alpine(const QByteArray &yaml, const QString &flavor, const QString &base) {
    // latest-releases.yaml is a list of "- key: value" blocks, one per image flavor.
    QHash<QString, QString> entry;
    Release found;
    auto take = [&] {
        if (entry.value("flavor") == flavor && entry.value("iso").endsWith(".iso") && QRegularExpression("^[0-9a-f]{64}$").match(entry.value("sha256")).hasMatch())
            found = {entry.value("version"), entry.value("iso"), base + entry.value("iso"), entry.value("sha256"), entry.value("size").toLongLong()};
    };
    for (const auto &line : QString::fromUtf8(yaml).split('\n')) {
        auto text = line.trimmed();
        if (text == "-" || text.startsWith("- ")) { take(); entry.clear(); text = text.mid(1).trimmed(); }
        const auto colon = text.indexOf(':');
        if (colon > 0) entry[text.left(colon).trimmed()] = text.mid(colon + 1).trimmed();
    }
    take();
    return found;
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
    for (const auto &header : headers) request.setRawHeader(header.first, header.second);
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
    for (auto &source : sources_) if (source.kind == "download" && !jobs_.contains(source.id)) resolve(source);
    emit changed();
}
void IsoLibrary::settle(const QString &id, const Release &release, const QString &failure) {
    --pending_;
    if (auto s = find(id)) {
        if (!release.file.isEmpty() && !release.sha256.isEmpty()) { s->latest = release; s->status = "ready"; }
        else { s->status = "error"; s->error = failure.isEmpty() ? "The latest release couldn't be found." : failure; }
    }
    emit changed();
}
void IsoLibrary::resolve(Source &source) {
    const auto id = source.id, type = source.lookup.value("type").toString();
    const auto pattern = source.lookup.value("remote", source.pattern).toString();
    source.status = "checking"; source.error.clear(); ++pending_;
    auto sums = [this, id, pattern](const QString &sumsUrl, const QString &base) {
        get(QUrl(sumsUrl), [this, id, pattern, base](bool ok, const QByteArray &data, const QUrl &) {
            settle(id, ok ? fromChecksums(data, pattern, base) : Release{}, ok ? QString{} : offline);
        });
    };
    if (type == "ubuntu") {
        const auto base = source.lookup.value("base").toString();
        get(QUrl("https://changelogs.ubuntu.com/meta-release-lts"), [this, id, base, sums](bool ok, const QByteArray &data, const QUrl &) {
            const auto codename = ok ? ubuntuLtsCodename(data) : QString{};
            if (codename.isEmpty()) { settle(id, {}, ok ? "Ubuntu's release list couldn't be read." : offline); return; }
            const auto folder = fill(base, {{"codename", codename}});
            sums(folder + "SHA256SUMS", folder);
        });
    } else if (type == "sums") {
        sums(source.lookup.value("sums").toString(), source.lookup.value("base").toString());
    } else if (type == "folder") {
        const auto lookup = source.lookup;
        get(QUrl(lookup.value("index").toString()), [this, id, lookup, sums](bool ok, const QByteArray &data, const QUrl &) {
            const auto version = ok ? newestFolder(data, lookup.value("folder", R"(^[0-9]+(\.[0-9]+)*$)").toString()) : QString{};
            if (version.isEmpty()) { settle(id, {}, ok ? "The release list couldn't be read." : offline); return; }
            const QHash<QString, QString> values{{"version", version}, {"major", majorOf(version)}};
            sums(fill(lookup.value("sums").toString(), values), fill(lookup.value("base").toString(), values));
        });
    } else if (type == "fedora") {
        const auto variant = source.lookup.value("variant").toString();
        get(QUrl("https://fedoraproject.org/releases.json"), [this, id, variant](bool ok, const QByteArray &data, const QUrl &) {
            settle(id, ok ? fedora(data, variant) : Release{}, ok ? QString{} : offline);
        });
    } else if (type == "alpine") {
        const auto flavor = source.lookup.value("flavor").toString(), base = QString("https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/");
        get(QUrl(base + "latest-releases.yaml"), [this, id, flavor, base](bool ok, const QByteArray &data, const QUrl &) {
            settle(id, ok ? alpine(data, flavor, base) : Release{}, ok ? QString{} : offline);
        });
    } else if (type == "popos") {
        // Pop!_OS follows Ubuntu LTS versions: try this LTS year, then the ones before.
        const int year = QDate::currentDate().year() % 100;
        QStringList versions;
        for (int y = year - year % 2; y >= year - 4; y -= 2) versions << QString("%1.04").arg(y, 2, 10, QChar('0'));
        auto attempt = std::make_shared<std::function<void(int)>>();
        *attempt = [this, id, versions, attempt](int i) {
            if (i >= versions.size()) { settle(id, {}, "Pop!_OS's release list couldn't be read."); return; }
            get(QUrl("https://api.pop-os.org/builds/" + versions[i] + "/intel"), [this, id, i, attempt](bool ok, const QByteArray &data, const QUrl &) {
                const auto o = QJsonDocument::fromJson(data).object();
                const auto url = o["url"].toString(), sha = o["sha_sum"].toString().toLower();
                if (!ok || !url.startsWith("https://") || !QRegularExpression("^[0-9a-f]{64}$").match(sha).hasMatch()) { (*attempt)(i + 1); return; }
                settle(id, {o["version"].toString(), QUrl(url).fileName(), url, sha, qint64(o["size"].toDouble())}, {});
            });
        };
        (*attempt)(0);
    } else if (type == "nixos") {
        // NixOS releases are YY.05 and YY.11; the newest one with a published image wins.
        const int year = QDate::currentDate().year() % 100;
        QStringList versions;
        for (int y = year; y >= year - 1; --y) versions << QString("%1.11").arg(y, 2, 10, QChar('0')) << QString("%1.05").arg(y, 2, 10, QChar('0'));
        auto attempt = std::make_shared<std::function<void(int)>>();
        *attempt = [this, id, versions, attempt](int i) {
            if (i >= versions.size()) { settle(id, {}, "NixOS's release list couldn't be read."); return; }
            const auto channel = "https://channels.nixos.org/nixos-" + versions[i] + "/latest-nixos-graphical-x86_64-linux.iso.sha256";
            get(QUrl(channel), [this, id, i, attempt](bool ok, const QByteArray &data, const QUrl &finalUrl) {
                // The channel link redirects to the exact release; its checksum names the file next to it.
                const auto m = QRegularExpression(R"(^([0-9a-f]{64})\s+(\S+\.iso)\s*$)").match(QString::fromUtf8(data).trimmed());
                if (!ok || !m.hasMatch()) { (*attempt)(i + 1); return; }
                const auto base = finalUrl.toString().section('/', 0, -2) + "/";
                const auto version = QRegularExpression(R"(^nixos-graphical-([0-9][0-9.]*)\.[0-9a-f]+-)").match(m.captured(2)).captured(1);
                settle(id, {version, m.captured(2), base + m.captured(2), m.captured(1), 0}, {});
            });
        };
        (*attempt)(0);
    } else if (type == "microsoft") {
        get(QUrl(source.lookup.value("page").toString()), [this, id](bool ok, const QByteArray &data, const QUrl &) {
            const auto page = ok ? windowsPage(data) : QVariantMap{};
            auto s = find(id);
            if (!s || page.isEmpty()) { settle(id, {}, ok ? "Microsoft's download page couldn't be read." : offline); return; }
            s->version = page["version"].toString(); s->edition = page["edition"].toString(); s->languages = page["languages"].toStringList();
            s->hashes.clear();
            const auto hashes = page["hashes"].toMap();
            for (auto it = hashes.cbegin(); it != hashes.cend(); ++it) s->hashes[it.key()] = it.value().toString();
            if (!s->languages.contains(s->language)) s->language = windowsLanguage(QLocale::system(), s->languages);
            settle(id, windowsRelease(*s), {});
        }, 0, {{"User-Agent", browserAgent}});
    } else settle(id, {}, "");
}

// ---- Windows from Microsoft --------------------------------------------------------------------
QVariantMap IsoLibrary::windowsPage(const QByteArray &html) {
    const auto text = QString::fromUtf8(html);
    const auto version = QRegularExpression(R"(Version ([0-9]{2}H[12]))").match(text).captured(1);
    const auto edition = QRegularExpression(R"re(<option value="([0-9]{1,8})">Windows)re").match(text).captured(1);
    QStringList languages; QVariantMap hashes;
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
            ? "Microsoft refused the automated download from your network (this happens with some VPNs and providers). Download it on Microsoft's website instead and save it in your ISO folder."
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
    case QLocale::English: wanted = locale.territory() == QLocale::UnitedStates ? "English" : "English International"; break;
    case QLocale::Chinese: wanted = locale.script() == QLocale::TraditionalChineseScript || locale.territory() == QLocale::Taiwan || locale.territory() == QLocale::HongKong ? "Chinese Traditional" : "Chinese Simplified"; break;
    case QLocale::Portuguese: wanted = locale.territory() == QLocale::Brazil ? "Brazilian Portuguese" : "Portuguese"; break;
    case QLocale::Spanish: wanted = locale.territory() == QLocale::Mexico ? "Spanish (Mexico)" : "Spanish"; break;
    case QLocale::French: wanted = locale.territory() == QLocale::Canada ? "French Canadian" : "French"; break;
    case QLocale::Serbian: wanted = "Serbian Latin"; break;
    case QLocale::NorwegianBokmal: case QLocale::NorwegianNynorsk: wanted = "Norwegian"; break;
    default: wanted = QLocale::languageToString(locale.language());
    }
    if (available.contains(wanted)) return wanted;
    return available.contains("English") ? QString("English") : available.value(0);
}
IsoLibrary::Release IsoLibrary::windowsRelease(const Source &source) const {
    if (source.version.isEmpty() || !source.hashes.contains(source.language)) return {};
    // Named like the ISOs from Microsoft's website, so either kind is recognized.
    const auto language = QString(source.language).remove(QRegularExpression("[^A-Za-z]"));
    return {source.version, "Win11_" + source.version + "_" + language + "_x64.iso", {}, source.hashes.value(source.language), 0};
}
void IsoLibrary::setLanguage(const QString &id, const QString &language) {
    auto s = find(id);
    if (!s || jobs_.contains(id) || (!s->languages.isEmpty() && !s->languages.contains(language))) return;
    s->language = language;
    if (!s->version.isEmpty()) s->latest = windowsRelease(*s);
    emit changed();
}
// Microsoft hands out a download link only to a "session" that has been through the same checks its
// website runs in a browser: register the session, answer the ov-df challenge, then ask for the
// language's SKU and its link (which works for 24 hours).
void IsoLibrary::downloadWindows(const QString &id) {
    auto s = find(id);
    if (!s) return;
    const auto session = QUuid::createUuid().toString(QUuid::WithoutBraces), edition = s->edition, language = s->language, page = s->lookup.value("page").toString();
    const auto release = s->latest;
    const QString profile = "606624d44113", instance = "560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175";
    const QList<QPair<QByteArray, QByteArray>> browser{{"User-Agent", browserAgent}};
    s->status = "starting"; s->error.clear(); emit changed();
    auto failed = [this, id](bool ok, const QString &why) { if (auto s = find(id)) s->status = "ready"; fail(id, ok ? why : offline); };
    get(QUrl("https://vlscppe.microsoft.com/tags?org_id=y6jn8c31&session_id=" + session), [=, this](bool ok, const QByteArray &, const QUrl &) {
        if (!ok) { failed(false, {}); return; }
        get(QUrl("https://ov-df.microsoft.com/mdt.js?instanceId=" + instance + "&PageId=si&session_id=" + session), [=, this](bool ok, const QByteArray &script, const QUrl &) {
            const auto text = QString::fromUtf8(script);
            const auto w = QRegularExpression("[?&]w=([A-F0-9]+)").match(text).captured(1), ticks = QRegularExpression(R"(rticks=\"\+?([0-9]+))").match(text).captured(1);
            if (!ok || w.isEmpty() || ticks.isEmpty()) { failed(ok, "Microsoft's download check has changed, so OmaWare can't download Windows right now. Use Microsoft's website instead."); return; }
            const auto reply = "https://ov-df.microsoft.com/?session_id=" + session + "&CustomerId=" + instance + "&PageId=si&w=" + w + "&mdt=" + QString::number(QDateTime::currentMSecsSinceEpoch()) + "&rticks=" + ticks;
            get(QUrl(reply), [=, this](bool ok, const QByteArray &, const QUrl &) {
                if (!ok) { failed(false, {}); return; }
                const auto skus = "https://www.microsoft.com/software-download-connector/api/getskuinformationbyproductedition?profile=" + profile + "&ProductEditionId=" + edition + "&SKU=undefined&friendlyFileName=undefined&Locale=en-US&sessionID=" + session;
                get(QUrl(skus), [=, this](bool ok, const QByteArray &data, const QUrl &) {
                    const auto sku = ok ? windowsSku(data, language) : QString{};
                    if (sku.isEmpty()) { failed(ok, "Microsoft didn't offer Windows 11 in " + language + "."); return; }
                    const auto links = "https://www.microsoft.com/software-download-connector/api/GetProductDownloadLinksBySku?profile=" + profile + "&productEditionId=undefined&SKU=" + sku + "&friendlyFileName=undefined&Locale=en-US&sessionID=" + session;
                    get(QUrl(links), [=, this](bool ok, const QByteArray &data, const QUrl &) {
                        QString why;
                        const auto link = ok ? windowsLink(data, why) : QString{};
                        if (link.isEmpty()) { failed(ok, why); return; }
                        if (auto s = find(id)) s->status = "ready";
                        fetch(id, QUrl(link), release.sha256, release.file, release.size);
                    }, 0, {{"User-Agent", browserAgent}, {"Referer", page.toUtf8()}});
                }, 0, browser);
            }, 0, browser);
        }, 0, browser);
    }, 0, browser);
}

// ---- Downloads --------------------------------------------------------------------------------
bool IsoLibrary::download(const QString &id) {
    auto source = find(id);
    if (!source || source->kind != "download" || jobs_.contains(id) || source->status == "starting") return false;
    if (source->lookup.value("type") == "microsoft") {
        if (source->edition.isEmpty() || source->latest.sha256.isEmpty()) return false;
        downloadWindows(id);
        return true;
    }
    if (source->latest.url.isEmpty()) return false;
    return fetch(id, QUrl(source->latest.url), source->latest.sha256, source->latest.file, source->latest.size);
}
bool IsoLibrary::fetch(const QString &id, const QUrl &url, const QString &sha256, const QString &file, qint64 size) {
    if (jobs_.contains(id)) return false;
    // Only a plain .iso (or compressed .iso.bz2) name inside the library folder, never a path.
    const bool packed = file.endsWith(".iso.bz2", Qt::CaseInsensitive);
    if (QFileInfo(file).fileName() != file || file.startsWith('.') || !(file.endsWith(".iso", Qt::CaseInsensitive) || packed)) { fail(id, "The release has an unexpected file name."); return false; }
    if (!QRegularExpression("^[0-9a-f]{64}$").match(sha256.toLower()).hasMatch()) { fail(id, "The release has no valid checksum, so it can't be verified."); return false; }
    QDir().mkpath(folder_);
    const qint64 needed = size > 0 ? size * (packed ? 3 : 1) + qint64(512) * 1024 * 1024 : 0;
    if (needed > 0 && QStorageInfo(folder_).bytesAvailable() < needed) { fail(id, "There isn't enough free space in " + folder_ + "."); return false; }
    auto job = new Job;
    job->sha256 = sha256.toLower(); job->file = file; job->total = size;
    job->out = std::make_unique<QFile>(folder_ + "/" + file + ".part");
    if (!job->out->open(QIODevice::WriteOnly | QIODevice::Truncate)) { delete job; fail(id, "Couldn't write to " + folder_ + "."); return false; }
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
    job->stalled = false; job->quiet.start();
    // A server can stop sending without closing the connection; a minute of silence counts as a dropped one.
    if (!stallWatch_) {
        stallWatch_ = new QTimer(this);
        stallWatch_->setInterval(5000);
        connect(stallWatch_, &QTimer::timeout, this, [this] {
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                auto job = it.value();
                if (job->unpacking || job->waiting || !job->reply) continue;
                if (job->received != job->lastSeen) { job->lastSeen = job->received; job->quiet.restart(); continue; }
                if (job->quiet.elapsed() >= stallLimit()) { job->stalled = true; job->reply->abort(); }
            }
        });
    }
    stallWatch_->start();
    if (job->checkResume) http.setRawHeader("Range", "bytes=" + QByteArray::number(job->received) + "-");
    job->reply = network_.get(http);
    auto reply = job->reply.data();
    connect(reply, &QNetworkReply::readyRead, this, [this, id, reply] {
        auto job = jobs_.value(id); if (!job || job->reply != reply) return;
        if (job->checkResume) {
            job->checkResume = false;
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) {
                job->out->resize(0); job->out->seek(0); job->hash.reset(); job->received = job->offset = job->lastBytes = 0;
            }
        }
        const auto data = reply->readAll();
        job->hash.addData(data);
        if (job->out->write(data) != data.size()) { cancel(id); fail(id, "Writing the download failed. Is the disk full?"); return; }
        job->received += data.size();
        if (job->sample.elapsed() >= 500) {
            job->rate = (job->received - job->lastBytes) * 1000 / std::max<qint64>(1, job->sample.elapsed());
            job->lastBytes = job->received; job->sample.restart();
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
            if (job->checkResume) { job->checkResume = false; if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) { job->out->resize(0); job->out->seek(0); job->hash.reset(); job->received = 0; } }
            const auto rest = reply->readAll(); job->hash.addData(rest); job->out->write(rest); job->received += rest.size();
            complete(id);
            return;
        }
        const auto part = job->out->fileName();
        // A dropped connection (common on slow mirrors) is picked up again where it stopped.
        // Retries only run out when a server sends nothing at all; a slow one that keeps moving can finish.
        if (job->received > job->offset) job->retries = 0;
        const bool retry = (job->stalled || (error != QNetworkReply::OperationCanceledError && error != QNetworkReply::ContentNotFoundError && error != QNetworkReply::ContentAccessDenied)) && job->retries < 5;
        if (retry) {
            if (!job->checkResume) { const auto rest = reply->readAll(); job->hash.addData(rest); job->out->write(rest); job->received += rest.size(); }
            ++job->retries; job->waiting = true; job->reply = nullptr;
            emit changed();
            QTimer::singleShot(2000 * job->retries, this, [this, id] { if (auto job = jobs_.value(id); job && job->waiting) request(id); });
            return;
        }
        const bool cancelled = error == QNetworkReply::OperationCanceledError && !job->stalled;
        const auto why = job->stalled ? QString("the server stopped sending data") : reply->errorString();
        job->out->close(); QFile::remove(part);
        jobs_.remove(id); delete job;
        if (!cancelled) fail(id, "The download failed: " + why); else emit changed();
    });
}
void IsoLibrary::complete(const QString &id) {
    auto job = jobs_.value(id);
    if (!job) return;
    job->out->close();
    const auto part = job->out->fileName(), final = folder_ + "/" + job->file;
    auto drop = [&] { jobs_.remove(id); delete job; };
    if (QString::fromLatin1(job->hash.result().toHex()) != job->sha256) {
        QFile::remove(part); drop();
        fail(id, "The downloaded file doesn't match its published checksum, so it was deleted. Try again.");
        return;
    }
    QFile::remove(final);
    if (!QFile::rename(part, final)) { QFile::remove(part); drop(); fail(id, "The finished download couldn't be saved."); return; }
    if (final.endsWith(".bz2", Qt::CaseInsensitive)) { unpack(id, job, final); return; }
    const auto name = find(id) ? find(id)->name : job->file, file = job->file;
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
    connect(process, &QProcess::finished, this, [this, id, job, process, packed, target](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        const bool ok = status == QProcess::NormalExit && code == 0;
        QFile::remove(packed);
        jobs_.remove(id);
        const auto name = find(id) ? find(id)->name : QFileInfo(target).fileName();
        delete job;
        if (!ok) { QFile::remove(target + ".part"); fail(id, "The download couldn't be unpacked. Is bzip2 installed?"); return; }
        QFile::remove(target);
        QFile::rename(target + ".part", target);
        rescan();
        emit finished(id, true, name + " is ready: " + QFileInfo(target).fileName());
    });
    connect(process, &QProcess::errorOccurred, this, [process](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) emit process->finished(127, QProcess::CrashExit); });
    process->start("bzip2", {"-dc", packed});
    emit changed();
}
void IsoLibrary::cancel(const QString &id) {
    auto job = jobs_.value(id);
    if (!job) return;
    if (job->unpacking) { if (auto p = qobject_cast<QProcess *>(job->unpacker)) p->kill(); return; }
    if (job->reply) { job->reply->abort(); return; }
    // Waiting to reconnect after a dropped connection: nothing is in flight, so stop here.
    job->out->close(); QFile::remove(job->out->fileName());
    jobs_.remove(id); delete job;
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
    return QFileInfo(name).fileName() == name && name.endsWith(".iso", Qt::CaseInsensitive) && !info.isSymLink() && info.isFile()
        && !busy.contains(name) && !busy.contains(name + ".bz2");
}
}
bool IsoLibrary::remove(const QString &name) { return removeAll({name}) == 1; }
int IsoLibrary::removeAll(const QStringList &names) {
    QStringList busy;
    for (auto job : jobs_) busy << job->file;
    int removed = 0;
    for (const auto &name : names)
        if (deletable(folder_, name, busy) && QFile::remove(folder_ + "/" + name)) ++removed;
    rescan();
    return removed;
}

// ---- Adding ISOs from elsewhere ----
namespace {
QString freeName(const QString &folder, const QString &name) {
    const auto taken = [&](const QString &n) { return QFileInfo::exists(folder + "/" + n) || QFileInfo::exists(folder + "/" + n + ".part"); };
    if (!taken(name)) return name;
    const QFileInfo info(name);
    for (int i = 2;; ++i) {
        const auto candidate = QString("%1 (%2).%3").arg(info.completeBaseName()).arg(i).arg(info.suffix());
        if (!taken(candidate)) return candidate;
    }
}
bool sameFile(const QString &a, const QString &b) {
    struct stat x {}, y {};
    return ::stat(QFile::encodeName(a).constData(), &x) == 0 && ::stat(QFile::encodeName(b).constData(), &y) == 0
        && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
}
QString IsoLibrary::importName() const {
    return import_ ? import_->names.value(import_->current.load()) : QString();
}
double IsoLibrary::importProgress() const {
    return import_ && import_->total > 0 ? double(import_->copied.load()) / double(import_->total) : 0;
}
bool IsoLibrary::importFiles(const QVariantList &urls) {
    if (import_) { emit imported({}, false, "An ISO is still being copied. Try again when it's done."); return false; }
    QDir().mkpath(folder_);
    auto job = std::make_shared<Import>();
    const auto here = QFileInfo(folder_).canonicalFilePath();
    for (const auto &value : urls) {
        const QUrl url(value.toString());
        const QFileInfo info(url.isLocalFile() ? url.toLocalFile() : value.toString());
        if (!info.isFile() || info.suffix().compare("iso", Qt::CaseInsensitive) != 0) { job->skipped << (info.fileName().isEmpty() ? value.toString() : info.fileName()); continue; }
        if (info.canonicalPath() == here) { job->already << info.absoluteFilePath(); continue; }
        // The same file (or one with the same name and size) is already in the folder.
        const QFileInfo existing(folder_ + "/" + info.fileName());
        if (existing.isFile() && (sameFile(existing.filePath(), info.filePath()) || existing.size() == info.size())) { job->already << existing.absoluteFilePath(); continue; }
        const auto name = freeName(folder_, info.fileName());
        const auto target = folder_ + "/" + name;
        // On the same disk a hard link is instant and shares the space; otherwise copy it.
        if (::link(QFile::encodeName(info.absoluteFilePath()).constData(), QFile::encodeName(target).constData()) == 0) { job->paths << target; ++job->linked; continue; }
        job->from << info.absoluteFilePath(); job->to << target; job->names << name; job->total += info.size();
    }
    if (job->total > 0 && job->total > QStorageInfo(folder_).bytesAvailable()) {
        job->error = QString("There isn't enough free space in %1 to copy %2.").arg(folder_, job->names.size() == 1 ? job->names.first() : "these ISOs");
        job->from.clear(); job->to.clear(); job->names.clear(); job->total = 0;
    }
    import_ = job;
    if (job->from.isEmpty()) { QMetaObject::invokeMethod(this, &IsoLibrary::finishImport, Qt::QueuedConnection); emit changed(); return !job->paths.isEmpty() || !job->already.isEmpty(); }
    importThread_ = QThread::create([job] {
        QByteArray buffer(4 << 20, Qt::Uninitialized);
        for (int i = 0; i < job->from.size() && !job->cancel; ++i) {
            job->current = i;
            QFile in(job->from[i]), out(job->to[i] + ".part");
            if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) { job->error = QString("Couldn't copy %1: %2").arg(job->names[i], in.isOpen() ? out.errorString() : in.errorString()); return; }
            QString failure;
            while (!job->cancel) {
                const auto n = in.read(buffer.data(), buffer.size());
                if (n < 0) { failure = in.errorString(); break; }
                if (n == 0) break;
                if (out.write(buffer.constData(), n) != n) { failure = out.errorString(); break; }
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
    if (importThread_) { importThread_->wait(); importThread_->deleteLater(); importThread_ = nullptr; }
    if (importTicker_) importTicker_->stop();
    const auto job = std::move(import_);
    import_.reset();
    auto paths = job->paths;
    for (int i = 0; i < job->copiedFiles.load(); ++i) paths << job->to[i];
    const auto added = paths.size();
    paths << job->already;
    const auto file = [](const QString &path) { return QFileInfo(path).fileName(); };
    QStringList message;
    if (added == 1) message << QString("Added %1 to your ISOs.").arg(file(paths.first()));
    else if (added > 1) message << QString("Added %1 ISOs.").arg(added);
    if (job->already.size() == 1) message << QString("%1 is already in your ISOs.").arg(file(job->already.first()));
    else if (job->already.size() > 1) message << QString("%1 were already in your ISOs.").arg(job->already.size());
    if (job->skipped.size() == 1) message << QString("Only .iso files can be added, so %1 was skipped.").arg(job->skipped.first());
    else if (job->skipped.size() > 1) message << QString("Only .iso files can be added, so %1 other files were skipped.").arg(job->skipped.size());
    if (job->cancel) message << "Copying was cancelled.";
    if (!job->error.isEmpty()) message << job->error;
    rescan();
    emit imported(paths, !paths.isEmpty() && job->error.isEmpty() && !job->cancel, message.join(' '));
}
