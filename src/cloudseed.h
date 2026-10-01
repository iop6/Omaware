// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// First-boot setup for VMs made from cloud images: the small "cidata" disc cloud-init reads (its
// NoCloud source) with the user, password hash, SSH key, network addresses and extra packages.
// Values are written as JSON, which cloud-init's YAML reader accepts and which needs no hand escaping.
namespace CloudSeed {
struct Nic {
    QString mac;
    QString address;   // "10.20.0.10/24" for a fixed address; empty for DHCP
};
struct Settings {
    QString instanceId, hostname, user, passwordHash, sshKey;
    // The VM's own SSH host key, made by OmaWare so it can check it is talking to the right VM.
    QString hostKeyPrivate, hostKeyPublic;
    QStringList packages, commands;
    QList<Nic> nics;
    bool guestAgent = false;   // install and start the QEMU guest agent (needs internet on first boot)
};
// A Linux user name cloud-init and every distribution accept.
bool validUser(const QString &user);
// A host name from a VM name: lower case letters, digits and dashes.
QString hostname(const QString &name);
// SHA-512 crypt hash ("$6$…") with a random salt, for cloud-init's passwd field.
QString hashPassword(const QString &password);
QByteArray userData(const Settings &settings);
QByteArray metaData(const Settings &settings);
QByteArray networkConfig(const Settings &settings);
// Writes an ISO 9660 image with Joliet names holding these files in its root folder.
bool writeIso(const QString &path, const QString &volumeId, QList<QPair<QString, QByteArray>> files, QString &error);
}
