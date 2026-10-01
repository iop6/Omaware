// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QVariantMap>
#include <QVariantList>
namespace Configuration {
QString hardware(const QString &xml, const QVariantMap &values, QString &error);
QVariantList changes(const QString &before, const QString &after);
QString revert(const QString &baseline, const QString &current, const QString &key, QString &error);
QString networkXml(const QVariantMap &values, const QString &uuid, const QString &bridge, QString &error);
}
class PendingChanges {
public:
    explicit PendingChanges(QString uuid);
    bool prepare(const QString &xml, QString &error);
    bool saved(const QString &xml, QString &error);
    // Applies a change that is already live to the baseline too, so it is not listed as pending.
    bool rebase(const QString &baseline, const QString &xml, QString &error);
    QVariantList items(const QString &xml) const;
    QString baseline() const;
    bool matches(const QString &xml) const;
    void clear();
private:
    QString path_;
    QVariantMap data_;
    bool write(QString &error);
};
