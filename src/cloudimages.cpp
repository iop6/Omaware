// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloudimages.h"
#include "isolibrary.h"
#include "paths.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStorageInfo>

namespace {
const QHash<QString, QString> names{{"ubuntu", "Ubuntu Server"}, {"debian", "Debian"}, {"fedora", "Fedora Cloud"}};

// Local files are named <id>-<version>.qcow2, for example ubuntu-24.04.qcow2.
QRegularExpression localName(const QString &id) {
    return QRegularExpression("^" + id + "-([0-9][0-9.]*)\\.qcow2$");
}
}

CloudImages::CloudImages(QObject *parent) : QObject(parent) {}

QStringList CloudImages::ids() {
    return {"ubuntu", "debian", "fedora"};
}

QString CloudImages::folder() {
    return Paths::root() + "/images";
}

QVariantMap CloudImages::local(const QString &id) {
    if (!ids().contains(id)) return {};
    QString best, bestVersion;
    for (const auto &file : QDir(folder()).entryList({id + "-*.qcow2"}, QDir::Files | QDir::NoSymLinks)) {
        const auto m = localName(id).match(file);
        if (m.hasMatch() && (bestVersion.isEmpty() || IsoLibrary::newer(m.captured(1), bestVersion))) {
            best = file;
            bestVersion = m.captured(1);
        }
    }
    if (best.isEmpty()) return {};
    const auto major = bestVersion.section('.', 0, 0);
    const QString preset = id == "ubuntu" ? "ubuntu" + bestVersion.section('.', 0, 1) : id + major;
    return {{"path", folder() + "/" + best}, {"version", bestVersion}, {"preset", preset},
            {"name", names.value(id) + " " + bestVersion}};
}

QVariantMap CloudImages::ubuntuLts(const QByteArray &metaRelease) {
    QVariantMap best;
    for (const auto &block :
            QString::fromUtf8(metaRelease).split(QRegularExpression("\\n\\s*\\n"), Qt::SkipEmptyParts)) {
        QHash<QString, QString> fields;
        for (const auto &line : block.split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon > 0) fields[line.left(colon).trimmed()] = line.mid(colon + 1).trimmed();
        }
        const auto version = QRegularExpression("^([0-9]+\\.[0-9]+)").match(fields.value("Version")).captured(1);
        if (fields.value("Supported") == "1" && QRegularExpression("^[a-z]+$").match(fields.value("Dist")).hasMatch() &&
                !version.isEmpty())
            best = {{"codename", fields.value("Dist")}, {"version", version}};
    }
    return best;
}

CloudImages::Image CloudImages::fedoraCloud(const QByteArray &json) {
    Image best;
    for (const auto &value : QJsonDocument::fromJson(json).array()) {
        const auto o = value.toObject();
        const auto version = o["version"].toString(), link = o["link"].toString(),
                   sha = o["sha256"].toString().toLower();
        if (o["arch"].toString() != "x86_64" || o["variant"].toString() != "Cloud" ||
                o["subvariant"].toString() != "Cloud_Base")
            continue;
        if (!QRegularExpression("^[0-9]+$").match(version).hasMatch() || !link.startsWith("https://") ||
                !link.contains("-Generic-") || !link.endsWith(".qcow2"))
            continue;
        if (!QRegularExpression("^[0-9a-f]{64}$").match(sha).hasMatch() ||
                (!best.version.isEmpty() && !IsoLibrary::newer(version, best.version)))
            continue;
        best.version = version;
        best.url = link;
        best.hash = sha;
    }
    return best;
}

QString CloudImages::hashFor(const QByteArray &sums, const QString &file) {
    const QRegularExpression line("^([0-9a-fA-F]{64}|[0-9a-fA-F]{128})\\s+\\*?(\\S+)\\s*$");
    for (const auto &raw : QString::fromUtf8(sums).split('\n')) {
        const auto m = line.match(raw.trimmed());
        if (m.hasMatch() && m.captured(2) == file) return m.captured(1).toLower();
    }
    return {};
}

void CloudImages::get(const QUrl &url, std::function<void(bool, const QByteArray &)> done) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    auto reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        done(reply->error() == QNetworkReply::NoError, reply->readAll());
    });
}

void CloudImages::resolve(const QString &id, std::function<void(const Image &, const QString &)> done) {
    const QString offline = "Couldn't reach the publisher to download the " + names.value(id) +
                            " image. Check the internet connection.";
    if (id == "ubuntu") {
        get(QUrl("https://changelogs.ubuntu.com/meta-release-lts"),
                [this, done, offline](bool ok, const QByteArray &data) {
                    const auto lts = ok ? ubuntuLts(data) : QVariantMap{};
                    if (lts.isEmpty()) {
                        done({}, ok ? "Ubuntu's release list couldn't be read." : offline);
                        return;
                    }
                    const auto codename = lts["codename"].toString(),
                               base = "https://cloud-images.ubuntu.com/" + codename + "/current/";
                    const auto file = codename + "-server-cloudimg-amd64.img";
                    get(QUrl(base + "SHA256SUMS"), [done, offline, base, file, lts](bool ok, const QByteArray &sums) {
                        Image image;
                        image.version = lts["version"].toString();
                        image.url = base + file;
                        image.hash = ok ? hashFor(sums, file) : QString{};
                        done(image, image.hash.isEmpty()
                                            ? (ok ? "Ubuntu's checksum list doesn't include the cloud image." : offline)
                                            : QString{});
                    });
                });
    } else if (id == "debian") {
        const QString base = "https://cloud.debian.org/images/cloud/trixie/latest/",
                      file = "debian-13-genericcloud-amd64.qcow2";
        get(QUrl(base + "SHA512SUMS"), [done, offline, base, file](bool ok, const QByteArray &sums) {
            Image image;
            image.version = "13";
            image.url = base + file;
            image.hash = ok ? hashFor(sums, file) : QString{};
            image.algorithm = QCryptographicHash::Sha512;
            done(image, image.hash.isEmpty()
                                ? (ok ? "Debian's checksum list doesn't include the cloud image." : offline)
                                : QString{});
        });
    } else if (id == "fedora") {
        get(QUrl("https://fedoraproject.org/releases.json"), [done, offline](bool ok, const QByteArray &data) {
            const auto image = ok ? fedoraCloud(data) : Image{};
            done(image, image.url.isEmpty() ? (ok ? "Fedora's release list doesn't include a cloud image." : offline)
                                            : QString{});
        });
    } else
        done({}, "Unknown image “" + id + "”.");
}

void CloudImages::ensure(const QString &id, std::function<void(bool, const QString &)> done) {
    if (!local(id).isEmpty()) {
        done(true, {});
        return;
    }
    if (auto job = jobs_.value(id)) {
        job->waiting.append(done);
        return;
    }
    auto job = new Job;
    job->waiting.append(done);
    jobs_[id] = job;
    emit changed();
    resolve(id, [this, id](const Image &image, const QString &error) {
        if (!error.isEmpty()) {
            finish(id, false, error);
            return;
        }
        download(id, image);
    });
}

void CloudImages::download(const QString &id, const Image &image) {
    auto job = jobs_.value(id);
    if (!job) return;
    job->image = image;
    job->image.id = id;
    QDir().mkpath(folder());
    if (QStorageInfo(folder()).bytesAvailable() < qint64(4) * 1024 * 1024 * 1024) {
        finish(id, false, "There isn't enough free space in " + folder() + " for the image.");
        return;
    }
    job->out = std::make_unique<QFile>(folder() + "/" + id + "-" + image.version + ".qcow2.part");
    if (!job->out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finish(id, false, "Couldn't write to " + folder() + ".");
        return;
    }
    job->hash = std::make_unique<QCryptographicHash>(image.algorithm);
    QNetworkRequest request{QUrl(image.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    job->reply = network_.get(request);
    connect(job->reply, &QNetworkReply::downloadProgress, this, [this, job](qint64 received, qint64 total) {
        job->received = received;
        job->total = total;
        emit changed();
    });
    connect(job->reply, &QNetworkReply::readyRead, this, [job] {
        const auto data = job->reply->readAll();
        job->hash->addData(data);
        job->out->write(data);
    });
    connect(job->reply, &QNetworkReply::finished, this, [this, id, job] {
        job->reply->deleteLater();
        const auto data = job->reply->readAll();
        job->hash->addData(data);
        job->out->write(data);
        job->out->close();
        const auto part = job->out->fileName(), final = part.left(part.size() - 5);
        if (job->reply->error() != QNetworkReply::NoError) {
            QFile::remove(part);
            finish(id, false, "The image download failed: " + job->reply->errorString());
            return;
        }
        if (QString::fromLatin1(job->hash->result().toHex()) != job->image.hash) {
            QFile::remove(part);
            finish(id, false, "The downloaded image doesn't match its published checksum, so it was deleted.");
            return;
        }
        QFile::remove(final);
        if (!QFile::rename(part, final)) {
            QFile::remove(part);
            finish(id, false, "The image couldn't be saved.");
            return;
        }
        finish(id, true, {});
    });
}

void CloudImages::finish(const QString &id, bool ok, const QString &message) {
    auto job = jobs_.take(id);
    if (!job) return;
    const auto waiting = job->waiting;
    delete job;
    emit changed();
    for (const auto &done : waiting)
        done(ok, message);
}

QVariantMap CloudImages::progress() const {
    QVariantMap out;
    for (auto it = jobs_.cbegin(); it != jobs_.cend(); ++it)
        out[it.key()] = it.value()->total > 0 ? double(it.value()->received) / it.value()->total : 0.0;
    return out;
}

void CloudImages::cancelAll() {
    for (const auto &id : jobs_.keys()) {
        auto job = jobs_.value(id);
        if (job->reply) {
            job->reply->disconnect(this);
            job->reply->abort();
        }
        if (job->out) {
            job->out->close();
            QFile::remove(job->out->fileName());
        }
        finish(id, false, "Cancelled.");
    }
}
