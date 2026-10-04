// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>

// "Set it up for me": answers for an operating system's own installer, so it installs without questions.
// The answers go on a small extra disc OmaWare attaches to the new VM:
// - windows:   autounattend.xml, which Windows Setup reads from any drive.
// - subiquity: Ubuntu Server's autoinstall config on a "cidata" disc. Ubuntu only skips its confirmation
//              prompt with "autoinstall" on the kernel command line, so the first boot starts the
//              installer's kernel directly (see VmWorker::start).
// Ubuntu's installer powers the VM off when it's done; OmaWare then removes the answers and the
// installation media and starts the new system.
namespace Unattended {
struct Settings {
    QString user, password, passwordHash; // passwordHash: SHA-512 crypt for Linux; Windows needs the password
    QString hostname;                     // already a valid host name
    QString timezone;                     // IANA, e.g. "Europe/Berlin"
    QString locale;                       // e.g. "en_US"
    QString keyboard;                     // XKB layout, e.g. "us"
    bool uefi = false;
};

// What an installation ISO supports, from its file name: "windows", "subiquity" or "".
QString kindForFile(const QString &fileName);
// windows: the image to install from a Windows ISO, and the product key it needs ("" for evaluation copies).
QString windowsImage(const QString &fileName);
QString windowsKey(const QString &fileName);
// windows: the language of a Windows ISO from its file name ("en-US" when unknown).
QString windowsLanguage(const QString &fileName);
// windows: Windows' name for an IANA time zone ("UTC" when unknown).
QString windowsTimeZone(const QString &iana);
// The keyboard layout that most people with this locale use ("us" when unsure).
QString keyboardFor(const QString &locale);
QByteArray autounattend(const Settings &settings, const QString &fileName);
QByteArray subiquityUserData(const Settings &settings);
QByteArray subiquityMetaData(const QString &instanceId);
// A Windows computer name: up to 15 letters, digits and dashes.
QString computerName(const QString &hostname);
// The namespace of the <omaware:setup> marker on a VM still being set up.
inline const char *ns = "https://omaware.org/xmlns/setup/1";
}
