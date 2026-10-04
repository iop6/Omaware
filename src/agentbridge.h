// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class Backend;
class CloudImages;
class IsoLibrary;
class QLocalSocket;
class QTimer;
class VmWorker;

// Lets AI agents use OmaWare, when the user turns it on in Settings. Agents connect through
// `omaware mcp`, which speaks the Model Context Protocol and forwards each tool call to this bridge
// over a socket only this user can open (in the user's private runtime folder).
//
// What an agent can do, and the safety rules:
// - Only VMs and networks OmaWare created; other VMs aren't shown.
// - Building a lab is a proposal: OmaWare shows the plan and the user approves it and sets the VMs'
//   password in OmaWare itself. The agent never sees passwords; type_login types one for it.
// - Deleting a lab and restoring a snapshot ask the user first.
// - The agent sees and uses a VM's screen (screenshots, mouse, keyboard); OmaWare shows a banner while
//   it does, with a button that turns agent access off.
// - Every action goes into OmaWare's activity log.
// What diagnose_vm suggests from a VM's details (vm.details) and addresses (vm.addresses): {code, hint}.
namespace AgentDiagnosis {
QVariantList hints(const QVariantMap &details, const QVariantMap &addresses);
}

// Session grants: kinds of low-risk change the user can let an agent make without asking each time.
namespace AgentGrants {
// The only kind so far: adapter changes onto owned isolated or host-only networks, or removals.
inline constexpr const char *privateAdapters = "private_adapters";
inline constexpr qint64 lifetimeMs = 8LL * 3600 * 1000;
QString label(const QString &kind);
// The kind of grant that could cover this manage_network_adapter change, or empty when it always asks:
// `action` add/update/remove; `target` the networkOptions entry it ends up on (empty for remove).
QString forAdapterChange(const QString &action, const QVariantMap &target);
}

class AgentBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // What to run to add OmaWare to an agent, e.g. "claude mcp add omaware -- /path/omaware mcp".
    Q_PROPERTY(QString command READ command CONSTANT)
    // The VM whose screen an agent used last: {uuid, name, at (ms since epoch)}.
    Q_PROPERTY(QVariantMap screen READ screen NOTIFY screenChanged)
    // A lab plan waiting for the user: {id, plan, warnings, images}.
    Q_PROPERTY(QVariantMap proposal READ proposal NOTIFY proposalChanged)
    // A question for the user: {id, title, text, action}.
    Q_PROPERTY(QVariantMap confirmation READ confirmation NOTIFY confirmationChanged)
    // The lab being built: {id, name, state, step, steps, message}.
    Q_PROPERTY(QVariantMap build READ build NOTIFY buildChanged)
    Q_PROPERTY(QVariantList labs READ labs NOTIFY labsChanged)
    Q_PROPERTY(QVariantList logins READ logins NOTIFY labsChanged)
    // Kinds of change the user let the agent make without asking, until access is turned off:
    // [{kind, label, expires (ms since epoch)}]. Never saved.
    Q_PROPERTY(QVariantList grants READ grants NOTIFY grantsChanged)
public:
    explicit AgentBridge(Backend *backend, QObject *parent = nullptr);
    ~AgentBridge() override;

    bool enabled() const { return enabled_; }

    void setEnabled(bool on);

    QString error() const { return error_; }

    QString command() const;

    QVariantMap screen() const { return screen_; }

    QVariantMap proposal() const { return proposal_; }

    QVariantMap confirmation() const { return confirmation_; }

    QVariantMap build() const { return build_; }

    QVariantList labs() const;
    QVariantList logins() const;
    QVariantList grants() const;
    static QString socketPath();

    // The user's answers.
    // Build the proposed lab. With `saved`, use the saved login `login`; otherwise save user/password under it.
    Q_INVOKABLE void approve(
            const QString &id, const QString &login, const QString &user, const QString &password, bool saved);
    Q_INVOKABLE void decline(const QString &id);
    // With `grant`, a yes also allows the question's kind of change for the rest of the session.
    Q_INVOKABLE void answer(const QString &id, bool yes, bool grant = false);
    Q_INVOKABLE void revokeGrants();
    Q_INVOKABLE QString generatePassword() const;
    // The lab a VM belongs to: {lab, slug, login, user}, or empty.
    Q_INVOKABLE QVariantMap vmLab(const QString &uuid) const;
    // For "Show password" on a lab VM; empty if it can't be read.
    Q_INVOKABLE QString revealPassword(const QString &login) const;
    // Lab files (TOML): a built lab's plan to share or keep, and one to review and build, even with
    // agent access off. importLab returns what's wrong, or "" once the plan is shown for review.
    Q_INVOKABLE QString labFile(const QString &slug) const;
    Q_INVOKABLE bool exportLab(const QString &slug, const QString &url) const;
    Q_INVOKABLE QString importLab(const QString &url);
    // Turns agent access off and disconnects agents (the banner's Stop button).
    Q_INVOKABLE void stop();

    // One tool call: {ok, result} or {ok: false, error}; screenshots add {image: base64 PNG}.
    // Public so tests can call tools without a socket.
    using Reply = std::function<void(const QVariantMap &)>;
    void handle(const QString &tool, const QVariantMap &args, Reply reply);
    // The OS Shop's library, for get_media (the window creates it).
    void setMedia(IsoLibrary *library);

signals:
    void changed();
    void screenChanged();
    void proposalChanged();
    void confirmationChanged();
    void buildChanged();
    void labsChanged();
    void labImported(bool ok, QString message);
    void grantsChanged();

private:
    using Done = std::function<void(bool ok, const QVariantMap &result)>;
    void listen();
    void accept();
    // An operation through OmaWare's main worker (changes and lists), waiting while it's busy.
    void call(const QString &op, QVariantMap input, Done done, int attempt = 0);
    // An operation on the agent's own worker (screen, input, commands, addresses), never blocking the app.
    void callAgent(const QString &op, QVariantMap input, Done done);
    // Finds an OmaWare VM by name or UUID; empty with an error otherwise.
    QVariantMap findVm(const QString &name, QString &error) const;
    // `grantKind` (see AgentGrants) lets the user allow this kind of change for the session in the same dialog.
    void ask(const QString &title, const QString &text, const QString &action, std::function<void(bool)> then,
            const QString &grantKind = {});
    bool granted(const QString &kind) const;
    void note(const QString &message, bool ok = true);
    void markScreen(const QString &uuid, const QString &name);

    // Tools.
    void overview(Reply reply);
    // `local` marks a plan the user imported: it's built even with agent access off, and an agent's
    // plan can't replace it while it waits for review.
    void proposeLab(const QVariantMap &plan, Reply reply, bool local = false);
    void labStatus(const QVariantMap &args, Reply reply);
    // Answers lab_status calls waiting on this lab.
    void wake(const QString &id);
    void screenshot(const QVariantMap &vm, int maxWidth, Reply reply, const QString &summary = {});
    void input(const QVariantMap &vm, const QVariantMap &args, Reply reply);
    void typeLogin(const QVariantMap &vm, const QVariantMap &args, Reply reply);
    void serialConsole(const QVariantMap &vm, const QVariantMap &args, Reply reply);
    void runCommand(const QVariantMap &vm, const QVariantMap &args, Reply reply);
    void runOverSsh(const QVariantMap &vm, const QVariantMap &lab, const QString &command, int timeout, Reply reply);
    void deleteLab(const QString &slug, Reply reply);
    void setCable(const QVariantMap &vm, const QVariantMap &args, Reply reply);

    void provisioningTool(const QString &tool, const QVariantMap &args, Reply reply);
    QHash<QString, QVariantMap> provisioningStates_, provisioningRequests_;
    void managementTool(const QString &tool, const QVariantMap &vm, const QVariantMap &args, Reply reply);
    void manageNetwork(const QVariantMap &args, Reply reply);
    // get_media: the OS Shop's catalogue, and downloads from it after the user approves.
    void getMedia(const QVariantMap &args, Reply reply);
    QPointer<IsoLibrary> media_;
    // After an approved adapter change that's still pending: clean guest shutdown (resuming a paused VM
    // first), a bounded wait, then start. Never forces power off. `saved` is the adapter.save result.
    void restartForPending(const QVariantMap &vm, const QVariantMap &saved, int timeoutSeconds, Reply reply);
    void waitForVm(const QVariantMap &vm, const QVariantMap &args, Reply reply);
    void transferFile(const QVariantMap &vm, const QVariantMap &args, Reply reply);

    // Building an approved lab.
    void startBuild(const QString &login, const QString &user, const QString &password);
    void buildStep(int index);
    void finishBuild(bool ok, const QString &message);
    void reportBuild();

    Backend *backend_;
    CloudImages *images_;
    QThread agentThread_;
    VmWorker *agentWorker_;
    QLocalServer server_;
    bool enabled_ = false;
    bool localLab_ = false; // the waiting proposal or running build came from a lab file
    QString error_;
    QVariantMap screen_, proposal_, confirmation_, build_;
    QHash<QString, Done> pending_;
    QHash<QString, std::function<void(bool)>> questions_;
    QHash<QString, QString> questionGrants_; // question id -> the kind of change it can grant
    QHash<QString, qint64> grants_;          // kind -> expiry (ms since epoch)
    QHash<QString, QSize> screenSizes_;
    QVariantMap host_;
    // Lab states by id, and lab_status calls waiting for a change.
    QHash<QString, QVariantMap> states_;

    struct Waiter {
        QString lab, token;
        std::function<void()> answer;
    };

    QList<Waiter> statusWaiters_;

    // The build in progress.
    struct Build {
        QString id, login, user, passwordHash;
        QVariantMap plan, lab;
        QList<QPair<QString, std::function<void(std::function<void(const QString &)>)>>> steps;
    } current_;

    QList<std::function<void()>> linkWaiters_;
};
