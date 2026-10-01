// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QThread>
#include <QVariantList>
#include <libvirt/libvirt.h>
#include <atomic>
#include <thread>
#include <memory>
#include <QTimer>
#include <QElapsedTimer>
#include "diagnostics.h"
#include <unistd.h>

struct GraphicsSocket {
    explicit GraphicsSocket(int value, QString domainUuid = {}) : fd(value), uuid(std::move(domainUuid)) {}
    ~GraphicsSocket() { if (fd >= 0) ::close(fd); }
    int fd;
    QString uuid;
};
using GraphicsHandle = std::shared_ptr<GraphicsSocket>;
Q_DECLARE_METATYPE(GraphicsHandle)

class VmWorker : public QObject {
    Q_OBJECT
public:
    explicit VmWorker(QString uri, bool storageOnly = false) : uri_(std::move(uri)), storageOnly_(storageOnly) {}
    // Only this atomic crosses threads directly; all libvirt work stays on the worker.
    void requestCheckpointCancel() { checkpointCancel_ = true; }
    void resetCheckpointCancel() { checkpointCancel_ = false; }
public slots:
    void open();
    void refresh();
    void action(QString uuid, QString operation);
    // Power actions on several VMs; "power-on" starts stopped VMs and resumes paused ones.
    void bulk(QVariantList uuids, QString operation);
    // Pauses every running OmaWare-managed VM except `skip` before the app closes.
    void pauseForExit(QString skip);
    void createTest();
    void openConsole(QString uuid);
    void inspect(QString uuid, quint64 request, bool guestInfo);
    void configureNetwork(QString uuid, QString mac, QString networkId, QString model, bool linkUp, bool remove, QString revision);
    // Adds an adapter on this network to each VM, live on running ones where the guest allows.
    void connectVms(QVariantList uuids, QString networkId);
    // Plugs or pulls virtual cables: [{uuid, mac}], live on running VMs and in the saved definition.
    void setLinks(QVariantList targets, bool up);
    void manage(QString operation, QVariantMap input);
    void cancelRestart();
    void stop();
signals:
    void inventory(QVariantList rows);
    void connection(bool connected, QString message);
    void finished(QString message, bool ok);
    void graphics(GraphicsHandle socket);
    void created(QString uuid);
    void lifecycle();
    void inspected(QString uuid, quint64 request, QVariantMap details);
    void networkConfigured(QString uuid, bool ok, QString message);
    void linksSet(bool ok, QString message);
    void exitPaused(QStringList uuids, QStringList failures);
    void managed(QString operation, bool ok, QVariantMap result);
    void progress(QString message);
    void checkpointProgress(QVariantMap job);
private:
    QString error(const QString &context);
    bool owned(virDomainPtr domain);
    QString power(virDomainPtr domain, const QString &operation);
    bool changeNetwork(const QString &uuid, const QString &mac, const QString &networkId, const QString &model,
        bool linkUp, bool remove, const QString &revision, QString &message);
    bool detachLive(virDomainPtr domain, const QString &device, const QString &mac, QString &why);
    bool attachLive(virDomainPtr domain, const QString &device, QString &why);
    static int event(virConnectPtr, virDomainPtr, int, int, void *opaque);
    static void closed(virConnectPtr, int, void *opaque);
    QString uri_;
    virConnectPtr conn_ = nullptr;
    int callback_ = -1;
    int timer_ = -1;
    std::atomic_bool running_{false};
    std::thread events_;
    std::atomic_uint64_t generation_{0};
    QHash<QString, QPair<qulonglong, qint64>> cpuSamples_;
    QTimer *restartTimer_ = nullptr;
    QString restartUuid_;
    int restartTicks_ = 0;
    std::atomic_bool checkpointCancel_{false};
    bool storageOnly_ = false;
};

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList domains READ domains NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString uri READ uri CONSTANT)
    Q_PROPERTY(QVariantMap details READ details NOTIFY detailsChanged)
    Q_PROPERTY(bool detailsBusy READ detailsBusy NOTIFY detailsChanged)
    Q_PROPERTY(QVariantMap management READ management NOTIFY managementChanged)
    Q_PROPERTY(QVariantList activity READ activity NOTIFY activityChanged)
    Q_PROPERTY(QString activityWarning READ activityWarning NOTIFY activityChanged)
    Q_PROPERTY(QVariantMap checkpointJob READ checkpointJob NOTIFY checkpointJobChanged)
public:
    explicit Backend(QString uri, QObject *parent = nullptr);
    ~Backend() override;
    QVariantList domains() const { return rows_; }
    QString message() const { return message_; }
    QString uri() const { return uri_; }
    bool connected() const { return connected_; }
    bool busy() const { return busy_; }
    QVariantMap details() const { return details_; }
    bool detailsBusy() const { return detailsBusy_; }
    QVariantMap management() const { return management_; }
    QVariantList activity() const { return activity_; }
    QString activityWarning() const { return activityWarning_; }
    Q_INVOKABLE void clearActivity();
    Q_INVOKABLE QVariantMap errorAdvice(QString message) const { return Diagnostics::advice(message); }
    Q_INVOKABLE void showRecovery(QString action, QString uuid = {}) { emit recoveryRequested(action, uuid); }
    QVariantMap checkpointJob() const { return checkpointJob_; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void createTest();
    Q_INVOKABLE void action(QString uuid, QString operation);
    Q_INVOKABLE void bulkAction(QVariantList uuids, QString operation);
    // Never waits for the busy flag: closing the app queues this behind any running operation.
    Q_INVOKABLE void pauseForExit();
    Q_INVOKABLE void openConsole(QString uuid);
    Q_INVOKABLE void inspect(QString uuid, bool guestInfo = false);
    Q_INVOKABLE bool configureNetwork(QString uuid, QString mac, QString networkId, QString model, bool linkUp, bool remove, QString revision);
    Q_INVOKABLE bool connectVms(QVariantList uuids, QString networkId);
    // Never waits for the busy flag: pulling a cable is a safety action and is queued behind any running operation.
    Q_INVOKABLE void setLinks(QVariantList targets, bool up);
    Q_INVOKABLE bool request(QString operation, QVariantMap input = {});
    Q_INVOKABLE void cancelRestart();
    Q_INVOKABLE void cancelCheckpoint();
signals:
    void changed();
    void graphics(GraphicsHandle socket);
    void created(QString uuid);
    void operationFinished(QString message, bool ok);
    void lifecycle();
    void detailsChanged();
    void networkConfigured(QString uuid, bool ok, QString message);
    void linksSet(bool ok, QString message);
    void pausedForExit(QStringList uuids, QStringList failures);
    void commandFinished(QString operation, bool ok, QVariantMap result);
    void managementChanged();
    void activityChanged();
    void recoveryRequested(QString action, QString uuid);
    void checkpointJobChanged();
private:
    bool begin();
    QThread thread_;
    VmWorker *worker_;
    QThread storageThread_;
    VmWorker *storageWorker_;
    QVariantList rows_;
    QString uri_, message_ = "Connecting…";
    bool connected_ = false, busy_ = false;
    QVariantMap details_;
    QString inspectedUuid_;
    quint64 detailRequest_ = 0;
    bool detailsBusy_ = false;
    QVariantMap management_;
    QVariantList activity_;
    ActivityLog activityLog_;
    QString activityWarning_;
    QVariantMap checkpointJob_;
    QElapsedTimer jobClock_, rateClock_;
    qulonglong previousBytes_ = 0;
};
