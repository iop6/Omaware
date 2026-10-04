// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QSettings>
#include <QVariantMap>

class Workspace : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    explicit Workspace(QObject *parent = nullptr);

    int revision() const { return revision_; }

    Q_INVOKABLE QVariant get(QString key, QVariant fallback = {}) const;
    Q_INVOKABLE void set(QString key, QVariant value);
    Q_INVOKABLE QVariantMap vm(QString uuid) const;
    Q_INVOKABLE void saveVm(QString uuid, QVariantMap value);
    Q_INVOKABLE void copy(QString text);
    Q_INVOKABLE QString localPath(QString url) const;
    // Writes text to a file the user chose (a path or file: URL), replacing it in one step.
    Q_INVOKABLE bool saveText(QString url, QString text) const;
    // The built-in user guide ("USER-GUIDE.md" or "NETWORKS.md"), shown by the Help window.
    Q_INVOKABLE QString guide(QString name) const;
    static QVariantMap mediaFiles(QString folder);
signals:
    void changed();

private:
    QSettings settings_;
    int revision_ = 0;
};
