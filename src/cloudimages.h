// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <functional>
#include <memory>

class QNetworkReply;

// Ready-to-run disk images that publishers make for clouds: a VM made from one boots straight into a
// working system and is set up on first boot by cloud-init (user, password, network). Labs are built
// from these. Each image is downloaded once into images/ next to vms/ and isos/, and kept only if it
// matches the checksum the publisher lists.
class CloudImages : public QObject {
    Q_OBJECT
public:
    explicit CloudImages(QObject *parent = nullptr);

    struct Image {
        QString id, name, preset, file, url, hash;
        QCryptographicHash::Algorithm algorithm = QCryptographicHash::Sha256;
        QString version;
    };

    // Ids offered in plans: "ubuntu", "debian", "fedora".
    static QStringList ids();
    static QString folder();
    // The downloaded image for an id, or empty: {path, version, preset, name}.
    static QVariantMap local(const QString &id);
    // Makes sure an image is here, downloading the newest release if needed, then calls done(ok, message).
    void ensure(const QString &id, std::function<void(bool ok, const QString &message)> done);
    // Download progress per id, 0–1, while downloading.
    QVariantMap progress() const;
    void cancelAll();
    // Parsers, public for tests.
    static QVariantMap ubuntuLts(const QByteArray &metaRelease); // {codename, version}
    static Image fedoraCloud(const QByteArray &releasesJson);
    static QString hashFor(const QByteArray &sums, const QString &file);
signals:
    void changed();

private:
    struct Job {
        Image image;
        QPointer<QNetworkReply> reply;
        std::unique_ptr<QFile> out;
        std::unique_ptr<QCryptographicHash> hash;
        qint64 received = 0, total = 0;
        bool writeFailed = false; // a short write (full disk): the file can't be trusted even if the hash matches
        QList<std::function<void(bool, const QString &)>> waiting;
    };

    void resolve(const QString &id, std::function<void(const Image &, const QString &error)> done);
    void get(const QUrl &url, std::function<void(bool, const QByteArray &)> done);
    void download(const QString &id, const Image &image);
    void finish(const QString &id, bool ok, const QString &message);
    QNetworkAccessManager network_;
    QHash<QString, Job *> jobs_;
};
