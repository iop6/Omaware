// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QVariantMap>
#include <QVariantList>
namespace Diagnostics {
QString nextStep(const QString &message);
QVariantMap advice(const QString &message);
}
class ActivityLog {
public:
    explicit ActivityLog(QString path = {});
    QVariantList load() const;
    bool append(QVariantMap entry, QVariantList &entries, QString &error) const;
    bool clear(QString &error) const;
private:
    bool save(const QVariantList &entries, QString &error) const;
    QString path_;
};
