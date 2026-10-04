// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(qreal textScale READ textScale WRITE setTextScale NOTIFY changed)
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY changed)
    // True when an Omarchy palette file exists; other distributions default to the built-in dark theme.
    Q_PROPERTY(bool omarchyAvailable READ omarchyAvailable NOTIFY changed)
    // Every theme for the picker: key, title, detail, light, and swatches (background, surface, accent,
    // foreground, success, warning, danger). The Omarchy entry shows the live palette.
    Q_PROPERTY(QVariantList themes READ themes NOTIFY changed)
    // The window backdrop: "dots", "grid", "scanlines" or "none".
    Q_PROPERTY(QString texture READ texture NOTIFY changed)
public:
    explicit Theme(QString path, QObject *parent = nullptr);

    QVariantMap colors() const { return colors_; }

    QString status() const { return status_; }

    QString mode() const { return mode_; }

    Q_INVOKABLE void setMode(QString mode);

    qreal textScale() const { return textScale_; }

    bool reducedMotion() const { return reducedMotion_; }

    bool omarchyAvailable() const;
    QVariantList themes() const;
    QString texture() const;
    static QStringList keys();
    static QVariantMap builtin(const QString &key);
    Q_INVOKABLE void setTextScale(qreal scale);
    Q_INVOKABLE void setReducedMotion(bool reduced);
    void reload();
    static bool parse(const QByteArray &data, QVariantMap &colors, QString &error);
signals:
    void changed();

private:
    void watch();
    QString path_, status_, mode_ = "omarchy";
    QVariantMap colors_, omarchy_;
    qreal textScale_ = 1;
    bool reducedMotion_ = false;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
};
