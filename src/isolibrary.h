// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <functional>
#include <memory>

class QNetworkReply;

// Installation ISOs in OmaWare's data folder, and the ISO shop: the latest official releases of
// popular operating systems. It contacts publishers only when asked: check() reads their release
// lists, download() fetches one ISO into isos/ and keeps it only if its SHA-256 matches the
// publisher's checksum. Compressed images are unpacked after they are verified.
class IsoLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    // OmaWare's data folder, with vms/ and isos/ inside.
    Q_PROPERTY(QString root READ root CONSTANT)
    // Source ids in display order; stable, so views keep their items while details change.
    Q_PROPERTY(QStringList sourceIds READ sourceIds CONSTANT)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(bool downloading READ downloading NOTIFY changed)
    // Whether opening the shop looks up the latest releases (tests turn this off to stay offline).
    Q_PROPERTY(bool autoCheck MEMBER autoCheck_ NOTIFY changed)
public:
    explicit IsoLibrary(QObject *parent = nullptr);
    ~IsoLibrary() override;
    QString folder() const { return folder_; }
    QString root() const;
    void setFolder(const QString &folder);
    QStringList sourceIds() const;
    QVariantList sources() const;
    QVariantList files() const { return files_; }
    bool checking() const { return pending_ > 0; }
    bool downloading() const { return !jobs_.isEmpty(); }

    // Looks up the newest release of every source.
    Q_INVOKABLE void check();
    // Downloads a source's newest release (after check()). Returns false if it can't start.
    Q_INVOKABLE bool download(const QString &id);
    Q_INVOKABLE void cancel(const QString &id);
    // Deletes an ISO in the library folder (only plain files directly inside it).
    Q_INVOKABLE bool remove(const QString &name);
    // Deletes several ISOs at once; returns how many were deleted.
    Q_INVOKABLE int removeAll(const QStringList &names);
    Q_INVOKABLE void rescan();
    // The source an ISO file belongs to: {source, sourceName, version, preset}. The preset is the
    // libosinfo id that best matches, for example "ubuntu24.04" or "rocky10".
    Q_INVOKABLE QVariantMap identify(const QString &name) const;

    // Fetches any URL into the folder, verified against sha256 (used by download() and tests).
    bool fetch(const QString &id, const QUrl &url, const QString &sha256, const QString &file, qint64 size = 0);

    struct Release { QString version, file, url, sha256; qint64 size = 0; };
    // Parsers for the publishers' release lists; empty on failure.
    static QString ubuntuLtsCodename(const QByteArray &metaRelease);
    // GNU ("hash  file") or BSD ("SHA256 (file) = hash") checksum lists.
    static Release fromChecksums(const QByteArray &sums, const QString &pattern, const QString &base);
    static Release fedora(const QByteArray &releasesJson, const QString &variant);
    static Release alpine(const QByteArray &latestReleasesYaml, const QString &flavor, const QString &base);
    // The newest numeric folder in a web server's directory listing, e.g. "10.2" or "26.7".
    static QString newestFolder(const QByteArray &listing, const QString &pattern = R"(^[0-9]+(\.[0-9]+)*$)");
    // Sorts version strings numerically ("24.04.10" after "24.04.9").
    static bool newer(const QString &a, const QString &b);

signals:
    void changed();
    void filesChanged();
    void finished(const QString &id, bool ok, const QString &message);

private:
    struct Source {
        QString id, name, description, category, color, kind = "download", pattern, preset, note, page;
        QVariantMap lookup = {};   // how to find the newest release
        QString status = "unknown", error = {};
        Release latest = {};
    };
    struct Job {
        QPointer<QNetworkReply> reply;
        std::unique_ptr<QFile> out;
        QCryptographicHash hash{QCryptographicHash::Sha256};
        QString sha256, file;
        qint64 received = 0, total = 0, rate = 0, lastBytes = 0;
        QElapsedTimer sample;
        bool unpacking = false;
        QPointer<QObject> unpacker;
    };
    using Done = std::function<void(bool ok, const QByteArray &data, const QUrl &finalUrl)>;
    Source *find(const QString &id);
    const Source *find(const QString &id) const;
    void get(const QUrl &url, Done done, int attempt = 0);
    void resolve(Source &source);
    void settle(const QString &id, const Release &release, const QString &failure);
    void unpack(const QString &id, Job *job, const QString &packed);
    void fail(const QString &id, const QString &message);
    QString folder_;
    QList<Source> sources_;
    QVariantList files_;
    QHash<QString, Job *> jobs_;
    int pending_ = 0;
    bool autoCheck_ = true;
    QNetworkAccessManager network_;
};
