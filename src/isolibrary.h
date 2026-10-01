// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <memory>

class QNetworkReply;
class QThread;
class QTimer;

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
    // Adding ISOs from elsewhere (drag and drop): the file being copied and how far along it is.
    Q_PROPERTY(bool importing READ importing NOTIFY changed)
    Q_PROPERTY(QString importName READ importName NOTIFY changed)
    Q_PROPERTY(double importProgress READ importProgress NOTIFY changed)
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
    bool importing() const { return !!import_; }
    QString importName() const;
    double importProgress() const;

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
    // Sources offered in several languages (Windows): which one to download.
    Q_INVOKABLE void setLanguage(const QString &id, const QString &language);
    // Adds ISO files (paths or file:// URLs) to the folder. A file on the same disk is linked, which is
    // instant and takes no extra space; anything else is copied in the background. Other kinds of
    // files are skipped. imported() reports the result; returns false if nothing could be added.
    Q_INVOKABLE bool importFiles(const QVariantList &urls);
    Q_INVOKABLE void cancelImport();

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
    // Microsoft's Windows download page: {version ("26H2"), edition (product edition id), languages
    // (in page order), hashes (language -> SHA-256)}. Empty if the page couldn't be read.
    static QVariantMap windowsPage(const QByteArray &html);
    // The SKU id for a language in Microsoft's SKU list (matches its name or display name).
    static QString windowsSku(const QByteArray &skusJson, const QString &language);
    // The 64-bit ISO link from Microsoft's download-link reply; sets error if Microsoft refused.
    static QString windowsLink(const QByteArray &linksJson, QString &error);
    // The Windows language that matches a locale, e.g. "English" for en_US, "English International" for en_GB.
    static QString windowsLanguage(const QLocale &locale, const QStringList &available);
    // Sorts version strings numerically ("24.04.10" after "24.04.9").
    static bool newer(const QString &a, const QString &b);

signals:
    void changed();
    void filesChanged();
    void finished(const QString &id, bool ok, const QString &message);
    // paths: the dropped ISOs as they are now in the folder (including ones that were already there).
    void imported(const QStringList &paths, bool ok, const QString &message);

private:
    struct Source {
        QString id, name, description, category, color, kind = "download", pattern, preset, note, page;
        QVariantMap lookup = {};   // how to find the newest release
        QString status = "unknown", error = {};
        Release latest = {};
        // Windows: the edition on Microsoft's page, its languages and their checksums.
        QString edition = {}, version = {}, language = {};
        QStringList languages = {};
        QHash<QString, QString> hashes = {};
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
        // Resuming after a dropped connection: where the current request started, and how many retries.
        QUrl url;
        qint64 offset = 0;
        int retries = 0;
        bool checkResume = false, waiting = false, stalled = false;
        qint64 lastSeen = 0;
        QElapsedTimer quiet;   // time since data last arrived
    };
    struct Import {
        QStringList from, to, names;        // the copies still to make
        QStringList paths, already, skipped;  // results so far
        int linked = 0;
        std::atomic<int> copiedFiles{0};
        qint64 total = 0;
        std::atomic<qint64> copied{0};
        std::atomic<int> current{0};
        std::atomic<bool> cancel{false};
        QString error;
    };
    void finishImport();
    using Done = std::function<void(bool ok, const QByteArray &data, const QUrl &finalUrl)>;
    Source *find(const QString &id);
    const Source *find(const QString &id) const;
    void get(const QUrl &url, Done done, int attempt = 0, const QList<QPair<QByteArray, QByteArray>> &headers = {});
    Release windowsRelease(const Source &source) const;
    void downloadWindows(const QString &id);
    void resolve(Source &source);
    void settle(const QString &id, const Release &release, const QString &failure);
    void unpack(const QString &id, Job *job, const QString &packed);
    void request(const QString &id);
    void complete(const QString &id);
    void fail(const QString &id, const QString &message);
    QString folder_;
    QList<Source> sources_;
    QVariantList files_;
    QHash<QString, Job *> jobs_;
    int pending_ = 0;
    bool autoCheck_ = true;
    std::shared_ptr<Import> import_;
    QThread *importThread_ = nullptr;
    QTimer *importTicker_ = nullptr;
    QTimer *stallWatch_ = nullptr;
    QNetworkAccessManager network_;
};
