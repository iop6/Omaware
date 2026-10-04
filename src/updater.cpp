// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <memory>

#ifndef OMAWARE_UPDATE_REPO
#define OMAWARE_UPDATE_REPO "iop6/Omaware"
#endif

Updater::Updater(QObject *parent) : QObject(parent) {
    current_ = QCoreApplication::applicationVersion();
    appDir_ = QCoreApplication::applicationDirPath();
    feed_ = QUrl(QStringLiteral("https://api.github.com/repos/" OMAWARE_UPDATE_REPO "/releases/latest"));
    page_ = QStringLiteral("https://github.com/" OMAWARE_UPDATE_REPO "/releases/latest");
}

void Updater::set(const QString &status, const QString &error) {
    status_ = status;
    error_ = error;
    emit changed();
}

bool Updater::canInstall() const {
    const QFileInfo dir(appDir_), parent(QFileInfo(appDir_).absolutePath());
    return !development() && QFileInfo(appDir_ + "/SHA256SUMS").isFile() && QFileInfo(appDir_ + "/omaware").isFile() &&
           dir.isWritable() && parent.isWritable();
}

bool Updater::newer(const QString &a, const QString &b) {
    const auto base = [](const QString &v) {
        return v.section('-', 0, 0);
    };
    const auto x = base(a).split('.'), y = base(b).split('.');
    for (int i = 0; i < std::max(x.size(), y.size()); ++i) {
        const auto p = i < x.size() ? x[i].toLongLong() : 0, q = i < y.size() ? y[i].toLongLong() : 0;
        if (p != q) return p > q;
    }
    // Same numbers: a release is newer than a development build of it.
    return !a.contains('-') && b.contains('-');
}

QVariantMap Updater::parseRelease(const QByteArray &json) {
    const auto o = QJsonDocument::fromJson(json).object();
    const auto version = o["tag_name"].toString().remove(QRegularExpression("^v"));
    if (!QRegularExpression(R"(^[0-9]+(\.[0-9]+)*$)").match(version).hasMatch()) return {};
    QVariantMap result{{"version", version}, {"notes", o["body"].toString()}, {"page", o["html_url"].toString()}};
    const auto package = "omaware-" + version + "-linux-x86_64.tar.gz";
    for (const auto &value : o["assets"].toArray()) {
        const auto asset = value.toObject();
        if (asset["name"].toString() == package) {
            result["package"] = package;
            result["packageUrl"] = asset["browser_download_url"].toString();
            result["size"] = asset["size"].toDouble();
        }
        if (asset["name"].toString() == "SHA256SUMS") result["sumsUrl"] = asset["browser_download_url"].toString();
    }
    return result;
}

QStringList Updater::mismatches(const QString &dir) {
    QStringList bad;
    QFile sums(dir + "/SHA256SUMS");
    if (!sums.open(QIODevice::ReadOnly)) return {"SHA256SUMS"};
    int listed = 0;
    for (const auto &line : QString::fromUtf8(sums.readAll()).split('\n', Qt::SkipEmptyParts)) {
        const auto m = QRegularExpression(R"(^([0-9a-f]{64})\s+\*?(\S+)$)").match(line.trimmed());
        if (!m.hasMatch() || m.captured(2).contains("..") || m.captured(2).startsWith('/')) {
            bad << line;
            continue;
        }
        ++listed;
        QFile file(dir + "/" + m.captured(2));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) ||
                QString::fromLatin1(hash.result().toHex()) != m.captured(1))
            bad << m.captured(2);
    }
    if (listed == 0) bad << "SHA256SUMS";
    return bad;
}

void Updater::check() {
    if (status_ == "checking" || status_ == "downloading") return;
    set("checking");
    QNetworkRequest request(feed_);
    request.setHeader(QNetworkRequest::UserAgentHeader, "OmaWare/" + current_);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(20000);
    auto reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            set("error", "Couldn't check for updates. Check your internet connection and try again.");
            return;
        }
        const auto release = parseRelease(reply->readAll());
        if (release.isEmpty()) {
            set("error", "The release information couldn't be read.");
            return;
        }
        latest_ = release["version"].toString();
        notes_ = release["notes"].toString();
        // The window opens this link in the browser, so only a web page.
        if (release["page"].toString().startsWith("https://")) page_ = release["page"].toString();
        package_ = release["package"].toString();
        packageUrl_ = release["packageUrl"].toString();
        sumsUrl_ = release["sumsUrl"].toString();
        if (!newer(latest_, current_)) {
            set("upToDate");
            return;
        }
        set("available");
    });
}

namespace {
const QString incomplete = "The update package is incomplete, so it wasn't used.";
}

void Updater::download() {
    if (status_ != "available" && !(status_ == "error" && !latest_.isEmpty() && newer(latest_, current_))) return;
    if (!canInstall()) {
        set("error", "This copy of OmaWare can't update itself. Download the new version from the releases page.");
        return;
    }
    if (package_.isEmpty() || packageUrl_.isEmpty() || sumsUrl_.isEmpty()) {
        set("error", "This release has no Linux package with checksums yet.");
        return;
    }
    QDir(stageRoot()).removeRecursively();
    if (!QDir().mkpath(stageRoot())) {
        set("error", "Couldn't prepare a folder for the update next to " + appDir_ + ".");
        return;
    }
    progress_ = 0;
    set("downloading");
    // First the checksum list, then the package itself.
    auto sums = network_.get(request(QUrl(sumsUrl_)));
    connect(sums, &QNetworkReply::finished, this, [this, sums] {
        sums->deleteLater();
        QString expected;
        for (const auto &line : QString::fromUtf8(sums->readAll()).split('\n')) {
            const auto m = QRegularExpression(R"(^([0-9a-f]{64})\s+\*?(\S+)\s*$)").match(line.trimmed());
            if (m.hasMatch() && m.captured(2) == package_) expected = m.captured(1);
        }
        if (sums->error() != QNetworkReply::NoError || expected.isEmpty())
            return discard("The update's checksum couldn't be downloaded.");
        downloadPackage(expected);
    });
}

QNetworkRequest Updater::request(const QUrl &url) const {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "OmaWare/" + current_);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

void Updater::discard(const QString &error) {
    QDir(stageRoot()).removeRecursively();
    set("error", error);
}

// Downloads the package into the staging folder, hashing it on the way, and keeps it only if it matches the
// published checksum.
void Updater::downloadPackage(const QString &expected) {
    auto file = std::make_shared<QFile>(stageRoot() + "/" + package_);
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    if (!file->open(QIODevice::WriteOnly)) {
        set("error", "Couldn't save the update.");
        return;
    }
    reply_ = network_.get(request(QUrl(packageUrl_)));
    auto reply = reply_.data();
    connect(reply, &QNetworkReply::readyRead, this, [reply, file, hash] {
        const auto data = reply->readAll();
        hash->addData(data);
        file->write(data);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0) {
            progress_ = double(got) / total;
            emit changed();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, hash, expected] {
        reply->deleteLater();
        const auto rest = reply->readAll();
        hash->addData(rest);
        file->write(rest);
        file->close();
        if (reply->error() != QNetworkReply::NoError)
            return discard("The update couldn't be downloaded: " + reply->errorString());
        if (QString::fromLatin1(hash->result().toHex()) != expected)
            return discard("The update doesn't match its published checksum, so it was deleted.");
        unpack();
    });
}

// Unpacks the package beside itself, then checks the result before offering it.
void Updater::unpack() {
    auto tar = new QProcess(this);
    connect(tar, &QProcess::finished, this, [this, tar](int code, QProcess::ExitStatus status) {
        tar->deleteLater();
        QFile::remove(stageRoot() + "/" + package_);
        const auto dirs = QDir(stageRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        const auto dir = dirs.size() == 1 ? stageRoot() + "/" + dirs.first() : QString{};
        if (status != QProcess::NormalExit || code != 0 || dir.isEmpty()) return discard(incomplete);
        if (const auto problem = checkUnpacked(dir); !problem.isEmpty()) return discard(problem);
        staged_ = dir;
        progress_ = 1;
        set("ready");
    });
    tar->start("tar", {"-xzf", stageRoot() + "/" + package_, "-C", stageRoot(), "--no-same-owner"});
}

// Why the unpacked package in `dir` can't be used, or empty when it can: every file must match the package's
// own checksums, and links may only point inside it.
QString Updater::checkUnpacked(const QString &dir) const {
    if (!QFileInfo(dir + "/omaware").isExecutable() || !mismatches(dir).isEmpty()) return incomplete;
    QDirIterator it(
            dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden, QDirIterator::Subdirectories);
    const auto root = QFileInfo(dir).canonicalFilePath() + "/";
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        if (info.isSymLink() && !QFileInfo(info.symLinkTarget()).canonicalFilePath().startsWith(root))
            return "The update package contains an unexpected link, so it wasn't used.";
    }
    // Release packages carry their own VERSION, covered by their checksums; only older ones without it get one.
    // Either way it is complete on disk before "ready": an update applied straight away restarts into this
    // folder, and the launcher checks every file first.
    if (!QFile::exists(dir + "/VERSION")) {
        QFile version(dir + "/VERSION");
        if (!version.open(QIODevice::WriteOnly) || version.write((latest_ + "\n").toUtf8()) < 0 || !version.flush())
            return "The update couldn't be prepared.";
    }
    return {};
}

bool Updater::install() {
    if (status_ != "ready" || staged_.isEmpty()) return false;
    const auto previous = appDir_ + ".previous";
    QDir(previous).removeRecursively();
    // The running program keeps working from its old folder after the rename.
    if (!QDir().rename(appDir_, previous)) {
        set("error", "Couldn't move the current version aside.");
        return false;
    }
    if (!QDir().rename(staged_, appDir_)) {
        QDir().rename(previous, appDir_);
        set("error", "Couldn't put the new version in place; nothing was changed.");
        return false;
    }
    QDir(stageRoot()).removeRecursively();
    staged_.clear();
    set("installed");
    return true;
}

bool Updater::restart() {
    // The launcher refuses a folder whose files don't match their checksums; say so here instead of
    // quitting into nothing.
    if (!mismatches(appDir_).isEmpty()) {
        set("error", "The new version isn't complete, so OmaWare didn't restart. Start it yourself; your VMs are "
                     "paused and can be resumed there.");
        return false;
    }
    const auto launcher = QFileInfo::exists(appDir_ + "/omaware.sh") ? appDir_ + "/omaware.sh" : appDir_ + "/omaware";
    if (!QProcess::startDetached(launcher, {"--restarted"})) {
        set("error", "OmaWare couldn't start again. Start it yourself; your VMs are paused and can be resumed there.");
        return false;
    }
    emit quitRequested();
    return true;
}
