// SPDX-License-Identifier: GPL-3.0-or-later
#include "unattended.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamWriter>

namespace {
bool matches(const QString &pattern, const QString &text) {
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
}

bool retailWindows(const QString &file) {
    return matches(R"(^Win(?:dows)?11_.*\.iso$)", file);
}

bool enterpriseEval(const QString &file) {
    return matches(R"(_CLIENTENTERPRISEEVAL_[A-Za-z]+_x64FRE_[A-Za-z-]+\.iso$)", file);
}

bool serverEval(const QString &file) {
    return matches(R"(_SERVER_EVAL_x64FRE_[A-Za-z-]+\.iso$)", file);
}
}

namespace Unattended {
QString kindForFile(const QString &file) {
    if (retailWindows(file) || enterpriseEval(file) || serverEval(file)) return "windows";
    if (matches(R"(^ubuntu-[0-9]+(?:\.[0-9]+)*-live-server-amd64\.iso$)", file)) return "subiquity";
    return {};
}

QString windowsImage(const QString &file) {
    if (serverEval(file)) return "Windows Server 2025 Datacenter Evaluation (Desktop Experience)";
    if (enterpriseEval(file)) return "Windows 11 Enterprise Evaluation";
    return "Windows 11 Pro";
}

// Microsoft's published KMS client setup key for Windows 11 Pro: it selects the edition so Setup doesn't
// ask for a key, and activates nothing. Activate with your own license afterwards.
QString windowsKey(const QString &file) {
    return retailWindows(file) ? QStringLiteral("W269N-WFGWX-YVC9B-4J6C9-T83GX") : QString();
}

QString windowsLanguage(const QString &file) {
    // Microsoft's evaluation copies end in the language tag ("…_x64FRE_en-us.iso").
    const auto tag =
            QRegularExpression(R"(_x64FRE_([a-z]{2,3}-[a-z]{2,4})\.iso$)", QRegularExpression::CaseInsensitiveOption)
                    .match(file)
                    .captured(1);
    if (!tag.isEmpty()) {
        const auto parts = tag.split('-');
        return parts[0].toLower() + "-" + parts[1].toUpper();
    }
    // Windows 11 from Microsoft's website (or OmaWare): "Win11_26H2_EnglishInternational_x64.iso".
    static const QHash<QString, QString> names{{"arabic", "ar-SA"}, {"brazilianportuguese", "pt-BR"},
            {"bulgarian", "bg-BG"}, {"chinesesimplified", "zh-CN"}, {"chinesetraditional", "zh-TW"},
            {"croatian", "hr-HR"}, {"czech", "cs-CZ"}, {"danish", "da-DK"}, {"dutch", "nl-NL"}, {"english", "en-US"},
            {"englishinternational", "en-GB"}, {"estonian", "et-EE"}, {"finnish", "fi-FI"}, {"french", "fr-FR"},
            {"frenchcanadian", "fr-CA"}, {"german", "de-DE"}, {"greek", "el-GR"}, {"hebrew", "he-IL"},
            {"hungarian", "hu-HU"}, {"italian", "it-IT"}, {"japanese", "ja-JP"}, {"korean", "ko-KR"},
            {"latvian", "lv-LV"}, {"lithuanian", "lt-LT"}, {"norwegian", "nb-NO"}, {"polish", "pl-PL"},
            {"portuguese", "pt-PT"}, {"romanian", "ro-RO"}, {"russian", "ru-RU"}, {"serbianlatin", "sr-Latn-RS"},
            {"slovak", "sk-SK"}, {"slovenian", "sl-SI"}, {"spanish", "es-ES"}, {"spanishmexico", "es-MX"},
            {"swedish", "sv-SE"}, {"thai", "th-TH"}, {"turkish", "tr-TR"}, {"ukrainian", "uk-UA"}};
    const auto name =
            QRegularExpression(R"(^Win(?:dows)?11_[0-9A-Za-z]+_([A-Za-z]+)_x64)").match(file).captured(1).toLower();
    return names.value(name, "en-US");
}

QString windowsTimeZone(const QString &iana) {
    const auto id = QTimeZone::ianaIdToWindowsId(iana.toUtf8());
    return id.isEmpty() ? QStringLiteral("UTC") : QString::fromUtf8(id);
}

QString keyboardFor(const QString &locale) {
    const QLocale l(locale);
    switch (l.language()) {
    case QLocale::German:
        return l.territory() == QLocale::Switzerland ? "ch" : "de";
    case QLocale::French:
        return l.territory() == QLocale::Canada        ? "ca"
               : l.territory() == QLocale::Switzerland ? "ch"
               : l.territory() == QLocale::Belgium     ? "be"
                                                       : "fr";
    case QLocale::Spanish:
        return l.territory() == QLocale::Spain ? "es" : "latam";
    case QLocale::Italian:
        return "it";
    case QLocale::Portuguese:
        return l.territory() == QLocale::Brazil ? "br" : "pt";
    case QLocale::English:
        return l.territory() == QLocale::UnitedKingdom || l.territory() == QLocale::Ireland ? "gb" : "us";
    case QLocale::Swedish:
        return "se";
    case QLocale::NorwegianBokmal:
    case QLocale::NorwegianNynorsk:
        return "no";
    case QLocale::Danish:
        return "dk";
    case QLocale::Finnish:
        return "fi";
    case QLocale::Polish:
        return "pl";
    case QLocale::Czech:
        return "cz";
    case QLocale::Hungarian:
        return "hu";
    case QLocale::Turkish:
        return "tr";
    case QLocale::Japanese:
        return "jp";
    default:
        return "us";
    }
}

QString computerName(const QString &hostname) {
    auto name = QString(hostname).toUpper().remove(QRegularExpression("[^A-Z0-9-]")).left(15);
    while (name.endsWith('-'))
        name.chop(1);
    return name.isEmpty() || QRegularExpression("^[0-9]+$").match(name).hasMatch() ? QStringLiteral("OMAWARE-VM")
                                                                                   : name;
}

QByteArray autounattend(const Settings &s, const QString &file) {
    const auto language = windowsLanguage(file);
    // The account's language settings follow this computer; the installer's own follow the ISO.
    const auto host = QLocale(s.locale).bcp47Name();
    const auto userLocale = QRegularExpression("^[a-z]{2,3}-[A-Z]{2}$").match(host).hasMatch() ? host : language;
    QByteArray out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.writeStartDocument();
    x.writeStartElement("unattend");
    x.writeDefaultNamespace("urn:schemas-microsoft-com:unattend");
    x.writeNamespace("http://schemas.microsoft.com/WMIConfig/2002/State", "wcm");
    const QString wcm = "http://schemas.microsoft.com/WMIConfig/2002/State";
    auto component = [&](const QString &name) {
        x.writeStartElement("component");
        x.writeAttribute("name", name);
        x.writeAttribute("processorArchitecture", "amd64");
        x.writeAttribute("publicKeyToken", "31bf3856ad364e35");
        x.writeAttribute("language", "neutral");
        x.writeAttribute("versionScope", "nonSxS");
    };
    auto text = [&](const QString &name, const QString &value) {
        x.writeTextElement(name, value);
    };
    auto added = [&](const QString &name) {
        x.writeStartElement(name);
        x.writeAttribute(wcm, "action", "add");
    };
    auto locales = [&](const QString &ui) {
        text("InputLocale", userLocale);
        text("SystemLocale", userLocale);
        text("UILanguage", ui);
        text("UserLocale", userLocale);
    };
    auto password = [&](const QString &element) {
        x.writeStartElement(element);
        text("Value", s.password);
        text("PlainText", "true");
        x.writeEndElement();
    };

    // windowsPE: language, a fresh disk layout, the edition, and the license terms.
    x.writeStartElement("settings");
    x.writeAttribute("pass", "windowsPE");
    component("Microsoft-Windows-International-Core-WinPE");
    x.writeStartElement("SetupUILanguage");
    text("UILanguage", language);
    x.writeEndElement();
    locales(language);
    x.writeEndElement();
    component("Microsoft-Windows-Setup");
    x.writeStartElement("DiskConfiguration");
    added("Disk");
    text("DiskID", "0");
    text("WillWipeDisk", "true");
    x.writeStartElement("CreatePartitions");
    auto partition = [&](int order, const QString &type, const QString &size) {
        added("CreatePartition");
        text("Order", QString::number(order));
        text("Type", type);
        if (size.isEmpty())
            text("Extend", "true");
        else
            text("Size", size);
        x.writeEndElement();
    };
    if (s.uefi) {
        partition(1, "EFI", "300");
        partition(2, "MSR", "16");
        partition(3, "Primary", {});
    } else
        partition(1, "Primary", {});
    x.writeEndElement();
    x.writeStartElement("ModifyPartitions");
    auto modify = [&](int order, int id, const QString &format, const QString &label, const QString &letter,
                          bool active) {
        added("ModifyPartition");
        text("Order", QString::number(order));
        text("PartitionID", QString::number(id));
        text("Format", format);
        text("Label", label);
        if (!letter.isEmpty()) text("Letter", letter);
        if (active) text("Active", "true");
        x.writeEndElement();
    };
    if (s.uefi) {
        modify(1, 1, "FAT32", "System", {}, false);
        modify(2, 3, "NTFS", "Windows", "C", false);
    } else
        modify(1, 1, "NTFS", "Windows", "C", true);
    x.writeEndElement();
    x.writeEndElement();
    x.writeEndElement(); // Disk, DiskConfiguration
    x.writeStartElement("ImageInstall");
    x.writeStartElement("OSImage");
    x.writeStartElement("InstallFrom");
    added("MetaData");
    text("Key", "/IMAGE/NAME");
    text("Value", windowsImage(file));
    x.writeEndElement();
    x.writeEndElement();
    x.writeStartElement("InstallTo");
    text("DiskID", "0");
    text("PartitionID", s.uefi ? "3" : "1");
    x.writeEndElement();
    x.writeEndElement();
    x.writeEndElement();
    x.writeStartElement("UserData");
    text("AcceptEula", "true");
    if (const auto key = windowsKey(file); !key.isEmpty()) {
        x.writeStartElement("ProductKey");
        text("Key", key);
        text("WillShowUI", "OnError");
        x.writeEndElement();
    }
    x.writeEndElement();
    x.writeEndElement(); // component
    x.writeEndElement(); // settings

    // specialize: the computer's name and time zone.
    x.writeStartElement("settings");
    x.writeAttribute("pass", "specialize");
    component("Microsoft-Windows-Shell-Setup");
    text("ComputerName", computerName(s.hostname));
    text("TimeZone", windowsTimeZone(s.timezone));
    x.writeEndElement();
    x.writeEndElement();

    // oobeSystem: no questions, and a local administrator account (no Microsoft account, no network needed).
    x.writeStartElement("settings");
    x.writeAttribute("pass", "oobeSystem");
    component("Microsoft-Windows-International-Core");
    locales(language);
    x.writeEndElement();
    component("Microsoft-Windows-Shell-Setup");
    x.writeStartElement("OOBE");
    text("HideEULAPage", "true");
    text("HideOEMRegistrationScreen", "true");
    text("HideOnlineAccountScreens", "true");
    text("HideWirelessSetupInOOBE", "true");
    text("ProtectYourPC", "3");
    x.writeEndElement();
    x.writeStartElement("UserAccounts");
    // Windows Server's own Administrator gets the same password, so its setup doesn't stop to ask for one.
    if (serverEval(file)) password("AdministratorPassword");
    x.writeStartElement("LocalAccounts");
    added("LocalAccount");
    password("Password");
    text("Name", s.user);
    text("DisplayName", s.user);
    text("Group", "Administrators");
    x.writeEndElement();
    x.writeEndElement();
    x.writeEndElement(); // UserAccounts
    x.writeEndElement();
    x.writeEndElement();
    x.writeEndElement(); // unattend
    x.writeEndDocument();
    return out;
}

QByteArray subiquityUserData(const Settings &s) {
    const QJsonObject autoinstall{
            {"version", 1},
            {"locale", s.locale + ".UTF-8"},
            {"keyboard", QJsonObject{{"layout", s.keyboard}}},
            {"timezone", s.timezone},
            {"identity", QJsonObject{{"hostname", s.hostname}, {"username", s.user}, {"realname", s.user},
                                 {"password", s.passwordHash}}},
            {"ssh", QJsonObject{{"install-server", true}, {"allow-pw", true}}},
            {"packages", QJsonArray{"qemu-guest-agent"}},
            {"storage", QJsonObject{{"layout", QJsonObject{{"name", "lvm"}}}}},
            // OmaWare takes the installation media out and starts the new system once the VM is off.
            {"shutdown", "poweroff"},
    };
    // cloud-init reads JSON as YAML, so nothing needs hand escaping.
    return "#cloud-config\n" + QJsonDocument(QJsonObject{{"autoinstall", autoinstall}}).toJson(QJsonDocument::Indented);
}

QByteArray subiquityMetaData(const QString &instanceId) {
    return QJsonDocument(QJsonObject{{"instance-id", instanceId}}).toJson(QJsonDocument::Compact) + "\n";
}
}
