// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <toml++/toml.h>
#include <cmath>

static QVariantMap withUiColors(QVariantMap colors);
void Theme::setTextScale(qreal scale) {
    if (!std::isfinite(scale)) return;
    scale = std::clamp(scale, qreal(1), qreal(1.3));
    if (qFuzzyCompare(textScale_, scale)) return;
    textScale_ = scale; emit changed();
}
bool Theme::omarchyAvailable() const { return QFileInfo::exists(path_); }
void Theme::setReducedMotion(bool reduced) {
    if (reducedMotion_ == reduced) return;
    reducedMotion_ = reduced; emit changed();
}
static QVariantMap fallback(bool light) {
    return withUiColors({{"background", light ? "#f4f6fa" : "#0e131d"}, {"foreground", light ? "#172238" : "#e5eaf3"},
        {"accent", light ? "#245dc1" : "#91b6ff"}, {"surface", light ? "#ffffff" : "#171f2c"},
        {"muted", light ? "#5d6a80" : "#9aa9c0"}, {"border", light ? "#dce3ee" : "#2b3649"}});
}
// Phosphor green on near-black. Status hues stay distinct from the green accent.
static QVariantMap hacker() {
    auto colors = withUiColors({{"background", "#020604"}, {"foreground", "#8dffaa"}, {"accent", "#00ff5f"},
        {"surface", "#06110a"}, {"muted", "#3fae62"}, {"border", "#0f3a1e"}});
    colors["sidebar"] = "#030a06"; colors["field"] = "#040d07"; colors["raised"] = "#081a0e";
    colors["line"] = "#0b2614"; colors["subtle"] = "#0a2413"; colors["accentSoft"] = "#07301a";
    colors["success"] = "#5dff8f"; colors["warning"] = "#ffd75f"; colors["danger"] = "#ff5f6d"; colors["dangerSoft"] = "#2a0d10";
    return colors;
}
static double luminance(QColor c) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}
static QColor blend(QColor a, QColor b, double amount) {
    return QColor::fromRgbF(a.redF() * (1 - amount) + b.redF() * amount,
        a.greenF() * (1 - amount) + b.greenF() * amount,
        a.blueF() * (1 - amount) + b.blueF() * amount);
}
static QVariantMap withUiColors(QVariantMap colors) {
    QColor bg(colors["background"].toString()), fg(colors["foreground"].toString());
    QColor accent(colors["accent"].toString());
    const bool light = luminance(bg) > .5;
    // Pick the higher-contrast ink independently of whether the window is light.
    colors["accentText"] = luminance(accent) > .179 ? "#000000" : "#ffffff";
    colors["subtle"] = blend(bg, fg, .08).name();
    colors["accentSoft"] = blend(QColor(colors["surface"].toString()), accent, light ? .10 : .13).name();
    colors["raised"] = light ? "#ffffff" : blend(QColor(colors["surface"].toString()), fg, .035).name();
    colors["field"] = light ? "#ffffff" : blend(bg, fg, .025).name();
    colors["sidebar"] = light ? "#ffffff" : blend(bg, fg, .025).name();
    colors["line"] = blend(QColor(colors["surface"].toString()), fg, .08).name();
    colors["success"] = light ? "#247449" : "#8bd5a8";
    colors["warning"] = light ? "#885c0c" : "#ebc17a";
    colors["danger"] = light ? "#b43e4b" : "#f29aa3";
    colors["dangerSoft"] = blend(QColor(colors["surface"].toString()), QColor(colors["danger"].toString()), .10).name();
    return colors;
}
bool Theme::parse(const QByteArray &data, QVariantMap &colors, QString &error) {
    try {
        auto table = toml::parse(std::string_view(data.constData(), size_t(data.size())));
        QVariantMap candidate;
        for (auto key : {"background", "foreground", "accent"}) {
            auto value = table[key].value<std::string>();
            QColor color(value ? QString::fromStdString(*value) : QString{});
            if (!color.isValid() || color.alpha() != 255) { error = QString("Missing or invalid color: %1").arg(key); return false; }
            candidate[key] = color.name();
        }
        auto bg = QColor(candidate["background"].toString());
        auto fg = QColor(candidate["foreground"].toString());
        auto hi = std::max(luminance(bg), luminance(fg)), lo = std::min(luminance(bg), luminance(fg));
        if ((hi + .05) / (lo + .05) < 4.5) { error = "Palette text contrast is below 4.5:1"; return false; }
        bool light = luminance(bg) > .5;
        candidate["surface"] = blend(bg, fg, light ? .025 : .045).name();
        candidate["border"] = blend(bg, fg, .16).name();
        QColor muted = blend(bg, fg, .68);
        // Secondary text remains readable with arbitrary live Omarchy palettes.
        for (int step = 0; step < 16 && (std::max(luminance(muted), luminance(QColor(candidate["surface"].toString()))) + .05) /
               (std::min(luminance(muted), luminance(QColor(candidate["surface"].toString()))) + .05) < 4.5; ++step)
            muted = blend(muted, fg, .20).toRgb();
        candidate["muted"] = muted.name();
        colors = withUiColors(candidate);
        return true;
    } catch (const toml::parse_error &e) {
        error = QString::fromUtf8(e.description().data(), qsizetype(e.description().size()));
        return false;
    }
}
Theme::Theme(QString path, QObject *parent) : QObject(parent), path_(std::move(path)), colors_(fallback(false)) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { debounce_.start(); });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
    connect(&debounce_, &QTimer::timeout, this, &Theme::reload);
    reload();
}
void Theme::watch() {
    auto paths = watcher_.files() + watcher_.directories();
    if (!paths.isEmpty()) watcher_.removePaths(paths);
    if (QFileInfo::exists(path_)) watcher_.addPath(path_);
    QString directory = QFileInfo(path_).absolutePath();
    // Include parents that survive rm/mv of theme/current. Watch only existing dirs.
    for (int i = 0; i < 5; ++i) {
        QDir dir(directory);
        if (dir.exists()) watcher_.addPath(dir.absolutePath());
        auto parent = QFileInfo(directory).absolutePath();
        if (parent == directory) break;
        directory = parent;
    }
}
void Theme::reload() {
    watch();
    if (mode_ != "omarchy") return;
    QFile file(path_);
    QString failure;
    QVariantMap candidate;
    if (!file.open(QIODevice::ReadOnly)) failure = "Palette unavailable; using last good palette or dark fallback";
    else if (file.size() > 65536) failure = "Palette exceeds 64 KiB limit";
    else if (parse(file.readAll(), candidate, failure)) colors_ = candidate;
    status_ = failure.isEmpty() ? "Following Omarchy · live palette" : failure;
    emit changed();
}
void Theme::setMode(QString mode) {
    if (mode != "dark" && mode != "light" && mode != "omarchy" && mode != "hacker") return;
    mode_ = mode;
    if (mode == "omarchy") reload();
    else { colors_ = mode == "hacker" ? hacker() : fallback(mode == "light"); status_ = "Built-in " + mode + " palette"; emit changed(); }
}
