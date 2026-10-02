// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDir>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

// No backend/libvirt dependency: private, bounded staging and independent disk conversion.
// Only the disk is imported from an OVA; its OVF hardware settings are reported, not applied.
class ApplianceImport {
public:
    // Staging can need tens of GiB, so callers pass a folder on the destination's filesystem.
    explicit ApplianceImport(const QString &stagingParent = QDir::tempPath());
    static QString mediaType(const QString &path);
    bool prepare(const QString &source, const QString &originalName, QString &error);
    bool convert(const QString &destination, QString &error) const;
    quint64 capacity() const { return capacity_; }
    // Plain-language notes on what was checked and what was not carried over.
    QStringList notes() const { return notes_; }
private:
    bool stageOva(int fd, QString &error);
    bool stageCopy(int fd, QString &error);
    QTemporaryDir staging_;
    QString disk_, format_;
    QStringList notes_;
    quint64 capacity_ = 0;
};
