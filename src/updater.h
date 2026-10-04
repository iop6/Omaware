// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantMap>

class QNetworkReply;

// Keeps a self-contained OmaWare app folder up to date from GitHub Releases.
//
// check() reads the latest release; download() fetches its Linux package next to the app folder,
// checks it against the release's SHA256SUMS and every file inside against the package's own
// checksums, and unpacks it. install() swaps the new folder in (keeping the old one as
// "<folder>.previous"), and restart() starts the new version and asks this one to quit. The window
// pauses running VMs before calling it, as it does whenever OmaWare closes. Development builds ("-dev") and installs
// that aren't a self-contained folder (for example a build installed to ~/.local) are never changed; they only point to
// the releases page.
class Updater : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString current READ current NOTIFY changed)
    Q_PROPERTY(QString latest READ latest NOTIFY changed)
    // idle, checking, upToDate, available, downloading, ready, error
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString notes READ notes NOTIFY changed)
    Q_PROPERTY(QString page READ page NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    // Whether this copy can update itself in place.
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY changed)
    Q_PROPERTY(bool development READ development NOTIFY changed)
public:
    explicit Updater(QObject *parent = nullptr);

    QString current() const { return current_; }

    QString latest() const { return latest_; }

    QString status() const { return status_; }

    QString error() const { return error_; }

    QString notes() const { return notes_; }

    QString page() const { return page_; }

    double progress() const { return progress_; }

    bool canInstall() const;

    // Development builds, and copies without a version (such as test runs), never update themselves.
    bool development() const { return current_.isEmpty() || current_.contains("-dev"); }

    Q_INVOKABLE void check();
    Q_INVOKABLE void download();
    // Swaps the downloaded version in. Returns false (with error set) if it couldn't.
    Q_INVOKABLE bool install();
    // Starts this app folder's launcher again and asks the running copy to quit.
    Q_INVOKABLE bool restart();

    Q_INVOKABLE bool installAndRestart() { return install() && restart(); }

    // For tests: another app folder, version or release feed.
    void setAppDir(const QString &dir) {
        appDir_ = dir;
        emit changed();
    }

    void setCurrent(const QString &version) {
        current_ = version;
        emit changed();
    }

    void setFeed(const QUrl &url) { feed_ = url; }

    QString appDir() const { return appDir_; }

    // The fields OmaWare needs from GitHub's "latest release" JSON.
    static QVariantMap parseRelease(const QByteArray &json);
    // Numeric version comparison; "1.1.0-dev" counts as before "1.1.0".
    static bool newer(const QString &a, const QString &b);
    // Files listed in a SHA256SUMS file that are missing or don't match, relative to dir.
    static QStringList mismatches(const QString &dir);

signals:
    void changed();
    void quitRequested();

private:
    void set(const QString &status, const QString &error = {});
    // The stages of download(); discard() ends one with an error and removes what it staged.
    QNetworkRequest request(const QUrl &url) const;
    void discard(const QString &error);
    void downloadPackage(const QString &expected);
    void unpack();
    QString checkUnpacked(const QString &dir) const;

    QString stageRoot() const { return appDir_ + ".update"; }

    QString current_, latest_, status_ = "idle", error_, notes_, page_;
    QString package_, packageUrl_, sumsUrl_, staged_;
    double progress_ = 0;
    QString appDir_;
    QUrl feed_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
};
