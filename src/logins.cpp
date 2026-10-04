// SPDX-License-Identifier: GPL-3.0-or-later
#include "logins.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

// The Secret Service's secret: session, parameters, value and content type.
struct DBusSecret {
    QDBusObjectPath session;
    QByteArray parameters, value;
    QString contentType;
};
Q_DECLARE_METATYPE(DBusSecret)

QDBusArgument &operator<<(QDBusArgument &a, const DBusSecret &s) {
    a.beginStructure();
    a << s.session << s.parameters << s.value << s.contentType;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, DBusSecret &s) {
    a.beginStructure();
    a >> s.session >> s.parameters >> s.value >> s.contentType;
    a.endStructure();
    return a;
}

using SecretMap = QMap<QDBusObjectPath, DBusSecret>;
Q_DECLARE_METATYPE(SecretMap)

namespace {
QString testFolder;

QString folder() {
    return testFolder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : testFolder;
}

QString indexPath() {
    return folder() + "/logins.json";
}

QString privatePath() {
    return folder() + "/private/logins.json";
}

QVariantMap readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
}

bool writeJson(const QString &path, const QVariantMap &data, bool secret) {
    const auto dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) return false;
    if (secret) QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    // Set before any secret is written.
    if (secret) file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(QJsonDocument::fromVariant(data).toJson());
    return file.commit();
}

// ---- Secret Service (org.freedesktop.secrets) ----
const QString service = "org.freedesktop.secrets";

QMap<QString, QString> attributes(const QString &name) {
    return {{"application", "omaware"}, {"omaware-login", name}};
}
}

// Waits for a Secret Service prompt (for example "unlock your keyring") to be answered.
class PromptWaiter : public QObject {
    Q_OBJECT
public:
    QEventLoop loop;
    bool dismissed = true;
public slots:

    void completed(bool d, const QDBusVariant &) {
        dismissed = d;
        loop.quit();
    }
};

namespace {
bool runPrompt(const QDBusObjectPath &prompt) {
    if (prompt.path() == "/" || prompt.path().isEmpty()) return true;
    auto bus = QDBusConnection::sessionBus();
    PromptWaiter waiter;
    bus.connect(service, prompt.path(), "org.freedesktop.Secret.Prompt", "Completed", &waiter,
            SLOT(completed(bool, QDBusVariant)));
    QDBusInterface(service, prompt.path(), "org.freedesktop.Secret.Prompt", bus).call("Prompt", QString());
    QTimer::singleShot(180000, &waiter.loop, &QEventLoop::quit);
    waiter.loop.exec();
    return !waiter.dismissed;
}

struct Keyring {
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusObjectPath session;

    bool open() {
        static const bool registered = [] {
            qDBusRegisterMetaType<DBusSecret>();
            qDBusRegisterMetaType<SecretMap>();
            return true;
        }();
        Q_UNUSED(registered);
        if (!testFolder.isEmpty() || !bus.isConnected()) return false;
        // Activatable services (gnome-keyring) may not be running yet; the first call starts them.
        if (!bus.interface()->isServiceRegistered(service) &&
                !bus.interface()->activatableServiceNames().value().contains(service))
            return false;
        QDBusInterface secrets(service, "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", bus);
        auto reply = secrets.call("OpenSession", QString("plain"), QVariant::fromValue(QDBusVariant(QString())));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) return false;
        session = reply.arguments().at(1).value<QDBusObjectPath>();
        return !session.path().isEmpty();
    }

    QDBusInterface secrets() {
        return QDBusInterface(service, "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", bus);
    }

    // The items for this login, unlocked (asking the user if the keyring is locked).
    QList<QDBusObjectPath> items(const QString &name) {
        auto reply = secrets().call("SearchItems", QVariant::fromValue(attributes(name)));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) return {};
        auto unlocked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
        const auto locked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
        if (!locked.isEmpty()) {
            auto unlock = secrets().call("Unlock", QVariant::fromValue(locked));
            if (unlock.type() == QDBusMessage::ReplyMessage && unlock.arguments().size() >= 2) {
                unlocked += qdbus_cast<QList<QDBusObjectPath>>(unlock.arguments().at(0));
                if (runPrompt(unlock.arguments().at(1).value<QDBusObjectPath>())) unlocked += locked;
            }
        }
        return unlocked;
    }

    bool store(const QString &name, const QString &user, const QString &password) {
        auto alias = secrets().call("ReadAlias", QString("default"));
        if (alias.type() != QDBusMessage::ReplyMessage || alias.arguments().isEmpty()) return false;
        auto collection = alias.arguments().at(0).value<QDBusObjectPath>();
        if (collection.path() == "/") return false;
        // Unlock the default collection if needed.
        auto unlock = secrets().call("Unlock", QVariant::fromValue(QList<QDBusObjectPath>{collection}));
        if (unlock.type() == QDBusMessage::ReplyMessage && unlock.arguments().size() >= 2 &&
                !runPrompt(unlock.arguments().at(1).value<QDBusObjectPath>()))
            return false;
        QVariantMap properties{{"org.freedesktop.Secret.Item.Label", "OmaWare VM login: " + name + " (" + user + ")"},
                {"org.freedesktop.Secret.Item.Attributes", QVariant::fromValue(attributes(name))}};
        DBusSecret secret{session, {}, password.toUtf8(), "text/plain; charset=utf8"};
        QDBusInterface target(service, collection.path(), "org.freedesktop.Secret.Collection", bus);
        auto reply = target.call("CreateItem", properties, QVariant::fromValue(secret), true);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) return false;
        const auto item = reply.arguments().at(0).value<QDBusObjectPath>();
        return item.path() != "/" || runPrompt(reply.arguments().at(1).value<QDBusObjectPath>());
    }

    bool read(const QString &name, QString &password) {
        const auto found = items(name);
        if (found.isEmpty()) return false;
        auto reply = secrets().call("GetSecrets", QVariant::fromValue(found), QVariant::fromValue(session));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return false;
        const auto map = qdbus_cast<SecretMap>(reply.arguments().at(0));
        if (map.isEmpty()) return false;
        password = QString::fromUtf8(map.first().value);
        return true;
    }

    void erase(const QString &name) {
        for (const auto &item : items(name)) {
            auto reply = QDBusInterface(service, item.path(), "org.freedesktop.Secret.Item", bus).call("Delete");
            if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
                runPrompt(reply.arguments().at(0).value<QDBusObjectPath>());
        }
    }

    ~Keyring() {
        if (!session.path().isEmpty())
            QDBusInterface(service, session.path(), "org.freedesktop.Secret.Session", bus).call("Close");
    }
};
}

void Logins::setTestMode(const QString &f) {
    testFolder = f;
}

QVariantList Logins::list() {
    QVariantList out;
    const auto index = readJson(indexPath());
    for (auto it = index.begin(); it != index.end(); ++it) {
        auto entry = it.value().toMap();
        entry["name"] = it.key();
        out.append(entry);
    }
    return out;
}

QString Logins::user(const QString &name) {
    return readJson(indexPath()).value(name).toMap().value("user").toString();
}

bool Logins::exists(const QString &name) {
    return readJson(indexPath()).contains(name);
}

bool Logins::save(const QString &name, const QString &user, const QString &password, QString &store, QString &error) {
    if (name.trimmed().isEmpty() || name.size() > 64) {
        error = "Give the login a name of up to 64 characters.";
        return false;
    }
    if (password.isEmpty() || password.size() > 256) {
        error = "Enter a password.";
        return false;
    }
    Keyring keyring;
    if (keyring.open() && keyring.store(name, user, password)) {
        store = "keyring";
        // A password saved earlier in the private file is no longer needed.
        auto secrets = readJson(privatePath());
        if (secrets.remove(name)) writeJson(privatePath(), secrets, true);
    } else {
        auto secrets = readJson(privatePath());
        secrets[name] = password;
        if (!writeJson(privatePath(), secrets, true)) {
            error = "Couldn't save the password.";
            return false;
        }
        store = "file";
    }
    auto index = readJson(indexPath());
    index[name] = QVariantMap{{"user", user}, {"store", store}};
    if (!writeJson(indexPath(), index, false)) {
        error = "Couldn't save the login.";
        return false;
    }
    return true;
}

bool Logins::password(const QString &name, QString &password, QString &error) {
    const auto entry = readJson(indexPath()).value(name).toMap();
    if (entry.isEmpty()) {
        error = "There's no saved login called “" + name + "”.";
        return false;
    }
    if (entry["store"] == "keyring") {
        Keyring keyring;
        if (keyring.open() && keyring.read(name, password)) return true;
        error = "The password store didn't give out the password for “" + name + "”. Unlock it and try again.";
        return false;
    }
    password = readJson(privatePath()).value(name).toString();
    if (password.isEmpty()) {
        error = "The password for “" + name + "” is missing.";
        return false;
    }
    return true;
}

bool Logins::remove(const QString &name) {
    auto index = readJson(indexPath());
    const auto entry = index.take(name).toMap();
    if (entry.isEmpty()) return false;
    if (entry["store"] == "keyring") {
        Keyring keyring;
        if (keyring.open()) keyring.erase(name);
    }
    auto secrets = readJson(privatePath());
    if (secrets.remove(name)) writeJson(privatePath(), secrets, true);
    return writeJson(indexPath(), index, false);
}

QString Logins::generate() {
    // No look-alike characters (0/O, 1/l/I).
    static const QString alphabet = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    QStringList groups;
    for (int g = 0; g < 4; ++g) {
        QString group;
        for (int i = 0; i < 4; ++i)
            group += alphabet[QRandomGenerator::system()->bounded(int(alphabet.size()))];
        groups << group;
    }
    return groups.join('-');
}

#include "logins.moc"
