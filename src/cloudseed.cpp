// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloudseed.h"
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <algorithm>
#include <crypt.h>

bool CloudSeed::validUser(const QString &user) {
    static const QStringList reserved{"root", "daemon", "bin", "sys", "sync", "nobody", "admin", "ubuntu", "debian", "fedora"};
    return QRegularExpression("^[a-z_][a-z0-9_-]{0,31}$").match(user).hasMatch() && !reserved.contains(user);
}
QString CloudSeed::hostname(const QString &name) {
    auto host = name.toLower();
    host.replace(QRegularExpression("[^a-z0-9-]+"), "-");
    host.remove(QRegularExpression("^-+|-+$"));
    return host.left(63).isEmpty() ? "vm" : host.left(63);
}
QString CloudSeed::hashPassword(const QString &password) {
    static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    QByteArray salt = "$6$";
    for (int i = 0; i < 16; ++i) salt += alphabet[QRandomGenerator::system()->bounded(64)];
    crypt_data data{};
    const char *hash = crypt_r(password.toUtf8().constData(), salt.constData(), &data);
    // Failed hashes start with '*' on libxcrypt; never return those.
    return hash && QByteArray(hash).startsWith("$6$") ? QString::fromLatin1(hash) : QString{};
}
QByteArray CloudSeed::userData(const Settings &s) {
    QJsonObject user{{"name", s.user}, {"gecos", s.user}, {"shell", "/bin/bash"}, {"lock_passwd", false},
        {"sudo", "ALL=(ALL) NOPASSWD:ALL"}};
    if (!s.passwordHash.isEmpty()) user["passwd"] = s.passwordHash;
    if (!s.sshKey.isEmpty()) user["ssh_authorized_keys"] = QJsonArray{s.sshKey};
    QJsonObject config{{"hostname", s.hostname}, {"fqdn", s.hostname}, {"users", QJsonArray{user}}, {"ssh_pwauth", true}};
    if (!s.hostKeyPrivate.isEmpty() && !s.hostKeyPublic.isEmpty()) {
        config["ssh_keys"] = QJsonObject{{"ed25519_private", s.hostKeyPrivate}, {"ed25519_public", s.hostKeyPublic}};
        config["ssh_genkeytypes"] = QJsonArray{};
        config["ssh_deletekeys"] = true;
    }
    QStringList packages = s.packages;
    if (s.guestAgent && !packages.contains("qemu-guest-agent")) packages.prepend("qemu-guest-agent");
    if (!packages.isEmpty()) { config["packages"] = QJsonArray::fromStringList(packages); config["package_update"] = true; }
    QJsonArray commands;
    if (s.guestAgent) commands.append(QJsonArray{"systemctl", "enable", "--now", "qemu-guest-agent"});
    for (const auto &command : s.commands) commands.append(QJsonArray{"sh", "-c", command});
    if (!commands.isEmpty()) config["runcmd"] = commands;
    // Lets OmaWare (and an agent) see when first-boot setup has finished.
    config["final_message"] = "OmaWare setup finished after $UPTIME seconds";
    return "#cloud-config\n" + QJsonDocument(config).toJson(QJsonDocument::Indented);
}
QByteArray CloudSeed::metaData(const Settings &s) {
    return QJsonDocument(QJsonObject{{"instance-id", s.instanceId}, {"local-hostname", s.hostname}}).toJson(QJsonDocument::Indented);
}
QByteArray CloudSeed::networkConfig(const Settings &s) {
    QJsonObject ethernets;
    for (int i = 0; i < s.nics.size(); ++i) {
        const auto &nic = s.nics[i];
        QJsonObject entry{{"match", QJsonObject{{"macaddress", nic.mac.toLower()}}}, {"set-name", QString("eth%1").arg(i)}};
        if (nic.address.isEmpty()) entry["dhcp4"] = true;
        else { entry["dhcp4"] = false; entry["addresses"] = QJsonArray{nic.address}; }
        // A VM on several networks would otherwise wait for every adapter at boot.
        entry["optional"] = true;
        ethernets[QString("eth%1").arg(i)] = entry;
    }
    return QJsonDocument(QJsonObject{{"version", 2}, {"ethernets", ethernets}}).toJson(QJsonDocument::Indented);
}

// ---- ISO 9660 with Joliet --------------------------------------------------------------------
namespace {
constexpr int Sector = 2048;
void both16(QByteArray &b, int at, quint16 v) { b[at] = char(v & 0xff); b[at + 1] = char(v >> 8); b[at + 2] = char(v >> 8); b[at + 3] = char(v & 0xff); }
void both32(QByteArray &b, int at, quint32 v) {
    for (int i = 0; i < 4; ++i) { b[at + i] = char((v >> (8 * i)) & 0xff); b[at + 7 - i] = char((v >> (8 * i)) & 0xff); }
}
void le32(QByteArray &b, int at, quint32 v) { for (int i = 0; i < 4; ++i) b[at + i] = char((v >> (8 * i)) & 0xff); }
void be32(QByteArray &b, int at, quint32 v) { for (int i = 0; i < 4; ++i) b[at + 3 - i] = char((v >> (8 * i)) & 0xff); }
void put(QByteArray &b, int at, const QByteArray &bytes) { std::copy(bytes.begin(), bytes.end(), b.begin() + at); }
// A fixed-width identifier: ASCII padded with spaces, or UCS-2 big-endian padded with UCS-2 spaces.
QByteArray field(const QString &text, int width, bool joliet) {
    QByteArray out;
    if (joliet) { for (const QChar c : text.left(width / 2)) { out += char(c.unicode() >> 8); out += char(c.unicode() & 0xff); } while (out.size() < width) out += QByteArray("\0 ", 2); }
    else { out = text.toLatin1().left(width); out += QByteArray(width - out.size(), ' '); }
    return out;
}
QByteArray recordDate(const QDateTime &t) {
    const auto u = t.toUTC();
    QByteArray d(7, '\0');
    d[0] = char(u.date().year() - 1900); d[1] = char(u.date().month()); d[2] = char(u.date().day());
    d[3] = char(u.time().hour()); d[4] = char(u.time().minute()); d[5] = char(u.time().second());
    return d;
}
QByteArray volumeDate(const QDateTime &t) { return t.toUTC().toString("yyyyMMddHHmmss").toLatin1() + "00" + QByteArray(1, '\0'); }
QByteArray record(const QByteArray &name, quint32 lba, quint32 size, bool directory, const QDateTime &time) {
    QByteArray r(33 + name.size() + (name.size() % 2 == 0 ? 1 : 0), '\0');
    r[0] = char(r.size());
    both32(r, 2, lba); both32(r, 10, size);
    put(r, 18, recordDate(time));
    r[25] = directory ? 2 : 0;
    both16(r, 28, 1);
    r[32] = char(name.size());
    put(r, 33, name);
    return r;
}
QByteArray pathTable(quint32 rootLba, bool bigEndian) {
    QByteArray t(10, '\0');
    t[0] = 1;
    if (bigEndian) { be32(t, 2, rootLba); t[6] = 0; t[7] = 1; } else { le32(t, 2, rootLba); t[6] = 1; t[7] = 0; }
    return t;
}
QByteArray descriptor(int type, bool joliet, const QString &volume, quint32 sectors, quint32 tableL, quint32 tableM, quint32 rootLba, const QDateTime &time) {
    QByteArray d(Sector, '\0');
    d[0] = char(type); put(d, 1, "CD001"); d[6] = 1;
    put(d, 8, field("LINUX", 32, joliet));
    put(d, 40, field(volume, 32, joliet));
    both32(d, 80, sectors);
    if (joliet) put(d, 88, "%/E");
    both16(d, 120, 1); both16(d, 124, 1); both16(d, 128, Sector);
    both32(d, 132, 10);
    le32(d, 140, tableL); be32(d, 148, tableM);
    put(d, 156, record(QByteArray(1, '\0'), rootLba, Sector, true, time));
    for (const auto &[at, width] : QList<QPair<int, int>>{{190, 128}, {318, 128}, {446, 128}, {574, 128}, {702, 37}, {739, 37}, {776, 37}})
        put(d, at, field({}, width, joliet && width % 2 == 0));
    put(d, 574, field("OMAWARE", 128, joliet));
    put(d, 813, volumeDate(time)); put(d, 830, volumeDate(time));
    put(d, 847, QByteArray(16, '0') + QByteArray(1, '\0')); put(d, 864, QByteArray(16, '0') + QByteArray(1, '\0'));
    d[881] = 1;
    return d;
}
}
bool CloudSeed::writeIso(const QString &path, const QString &volumeId, QList<QPair<QString, QByteArray>> files, QString &error) {
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first.toUpper() < b.first.toUpper(); });
    if (files.size() > 20) { error = "Too many files for a setup disc."; return false; }
    for (const auto &file : files)
        if (!QRegularExpression("^[a-z0-9][a-z0-9_.-]{0,29}$").match(file.first).hasMatch()) { error = "Setup disc file names must be short and plain."; return false; }
    // Sectors: 16 PVD, 17 Joliet SVD, 18 terminator, 19–20 path tables, 21–22 Joliet path tables,
    // 23 root folder, 24 Joliet root folder, then the files.
    const quint32 isoRoot = 23, jolietRoot = 24;
    quint32 next = 25;
    QList<quint32> starts;
    for (const auto &file : files) { starts << next; next += std::max<quint32>(1, (file.second.size() + Sector - 1) / Sector); }
    const quint32 total = next;
    const auto now = QDateTime::currentDateTimeUtc();
    auto folder = [&](bool joliet, quint32 self) {
        QByteArray dir = record(QByteArray(1, '\0'), self, Sector, true, now) + record(QByteArray(1, '\1'), self, Sector, true, now);
        for (int i = 0; i < files.size(); ++i) {
            QByteArray name;
            if (joliet) for (const QChar c : files[i].first) { name += char(c.unicode() >> 8); name += char(c.unicode() & 0xff); }
            else name = files[i].first.toUpper().toLatin1() + ";1";
            dir += record(name, starts[i], quint32(files[i].second.size()), false, now);
        }
        if (dir.size() > Sector) return QByteArray{};
        return dir + QByteArray(Sector - dir.size(), '\0');
    };
    const auto isoFolder = folder(false, isoRoot), jolietFolder = folder(true, jolietRoot);
    if (isoFolder.isEmpty() || jolietFolder.isEmpty()) { error = "The setup disc's file list is too long."; return false; }
    QByteArray image(16 * Sector, '\0');
    image += descriptor(1, false, volumeId, total, 19, 20, isoRoot, now);
    image += descriptor(2, true, volumeId, total, 21, 22, jolietRoot, now);
    QByteArray terminator(Sector, '\0'); terminator[0] = char(255); put(terminator, 1, "CD001"); terminator[6] = 1;
    image += terminator;
    for (const auto &[root, big] : QList<QPair<quint32, bool>>{{isoRoot, false}, {isoRoot, true}, {jolietRoot, false}, {jolietRoot, true}}) {
        auto table = pathTable(root, big); image += table + QByteArray(Sector - table.size(), '\0');
    }
    image += isoFolder + jolietFolder;
    for (const auto &file : files) {
        image += file.second;
        const int used = file.second.size() % Sector;
        image += QByteArray(file.second.isEmpty() ? Sector : used ? Sector - used : 0, '\0');
    }
    if (image.size() != qsizetype(total) * Sector) { error = "The setup disc came out the wrong size."; return false; }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(image) != image.size()) { error = "Couldn't write the setup disc " + path + "."; return false; }
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return true;
}
