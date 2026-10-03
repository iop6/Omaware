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
static double luminance(QColor c) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}
static QColor blend(QColor a, QColor b, double amount) {
    return QColor::fromRgbF(a.redF() * (1 - amount) + b.redF() * amount,
        a.greenF() * (1 - amount) + b.greenF() * amount,
        a.blueF() * (1 - amount) + b.blueF() * amount);
}
// Fills in the derived interface colors a palette doesn't set itself.
static QVariantMap withUiColors(QVariantMap colors) {
    QColor bg(colors["background"].toString()), fg(colors["foreground"].toString());
    QColor accent(colors["accent"].toString()), surface(colors["surface"].toString());
    const bool light = luminance(bg) > .5;
    auto fill = [&](const char *key, const QString &value) { if (!colors.contains(key)) colors[key] = value; };
    fill("subtle", blend(bg, fg, .08).name());
    fill("accentSoft", blend(surface, accent, light ? .10 : .13).name());
    fill("raised", light ? "#ffffff" : blend(surface, fg, .035).name());
    fill("field", light ? "#ffffff" : blend(bg, fg, .025).name());
    fill("sidebar", light ? "#ffffff" : blend(bg, fg, .025).name());
    fill("line", blend(surface, fg, .08).name());
    fill("success", light ? "#247449" : "#8bd5a8");
    fill("warning", light ? "#885c0c" : "#ebc17a");
    fill("danger", light ? "#b43e4b" : "#f29aa3");
    fill("dangerSoft", blend(surface, QColor(colors["danger"].toString()), .10).name());
    return colors;
}
static double contrast(QColor a, QColor b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + .05) / (std::min(x, y) + .05);
}
// Keeps every palette readable: secondary text, status colors and the accent are nudged toward the
// foreground until they stand out from the cards they sit on.
static QVariantMap polish(QVariantMap colors) {
    colors = withUiColors(colors);
    const QColor fg(colors["foreground"].toString()), surface(colors["surface"].toString()), raised(colors["raised"].toString());
    auto lift = [&](const char *key, double wanted) {
        QColor c(colors[key].toString());
        for (int step = 0; step < 20 && std::min(contrast(c, surface), contrast(c, raised)) < wanted; ++step) c = blend(c, fg, .12).toRgb();
        colors[key] = c.name();
    };
    lift("muted", 4.5);
    lift("success", 3.2); lift("warning", 3.2); lift("danger", 3.2); lift("accent", 3);
    const QColor accent(colors["accent"].toString());
    colors["accentText"] = contrast(accent, QColor("#000000")) >= contrast(accent, QColor("#ffffff")) ? "#000000" : "#ffffff";
    return colors;
}
struct BuiltIn { const char *key, *title, *detail, *texture; QVariantMap colors; };
// Built-in themes. Each sets its own surfaces and status colors so warnings, errors and "running"
// look like they belong to the palette; polish() keeps them readable.
static const QList<BuiltIn> &catalog() {
    static const QList<BuiltIn> list{
        {"dark", "Dark", "Deep blue-gray, soft blue accent", "dots", {{"background", "#0d1219"}, {"foreground", "#e6ebf3"}, {"accent", "#8db4ff"},
            {"surface", "#151c27"}, {"muted", "#97a5bb"}, {"border", "#263244"}, {"sidebar", "#0a0f15"}, {"success", "#86d6a6"}, {"warning", "#ecc27c"}, {"danger", "#f2959f"}}},
        {"light", "Light", "Clean white cards on cool gray", "dots", {{"background", "#f4f6fa"}, {"foreground", "#172238"}, {"accent", "#2459c4"},
            {"surface", "#ffffff"}, {"muted", "#5a677d"}, {"border", "#dbe2ed"}, {"sidebar", "#fbfcfe"}, {"field", "#ffffff"}, {"success", "#1f7a4a"}, {"warning", "#8d5d07"}, {"danger", "#b8394a"}}},
        {"hacker", "Hacker", "Phosphor green on black, with scanlines", "scanlines", {{"background", "#020604"}, {"foreground", "#8dffaa"}, {"accent", "#00ff5f"},
            {"surface", "#06110a"}, {"muted", "#3fae62"}, {"border", "#0f3a1e"}, {"sidebar", "#030a06"}, {"field", "#040d07"}, {"raised", "#081a0e"},
            {"line", "#0b2614"}, {"subtle", "#0a2413"}, {"accentSoft", "#07301a"}, {"success", "#5dff8f"}, {"warning", "#ffd75f"}, {"danger", "#ff5f6d"}, {"dangerSoft", "#2a0d10"}}},
        {"tokyo-night", "Tokyo Night", "Night-city blues and neon", "dots", {{"background", "#1a1b26"}, {"foreground", "#c0caf5"}, {"accent", "#7aa2f7"},
            {"surface", "#1f2335"}, {"muted", "#8b93c0"}, {"border", "#2f3549"}, {"sidebar", "#16161e"}, {"success", "#9ece6a"}, {"warning", "#e0af68"}, {"danger", "#f7768e"}}},
        {"catppuccin", "Catppuccin Mocha", "Soothing pastels on a dark base", "dots", {{"background", "#1e1e2e"}, {"foreground", "#cdd6f4"}, {"accent", "#cba6f7"},
            {"surface", "#26263a"}, {"muted", "#a6adc8"}, {"border", "#383a52"}, {"sidebar", "#181825"}, {"success", "#a6e3a1"}, {"warning", "#f9e2af"}, {"danger", "#f38ba8"}}},
        {"catppuccin-latte", "Catppuccin Latte", "The light Catppuccin flavor", "dots", {{"background", "#eff1f5"}, {"foreground", "#4c4f69"}, {"accent", "#8839ef"},
            {"surface", "#fafbfc"}, {"muted", "#6c6f85"}, {"border", "#ccd0da"}, {"sidebar", "#e6e9ef"}, {"field", "#ffffff"}, {"raised", "#ffffff"}, {"success", "#40a02b"}, {"warning", "#b8700f"}, {"danger", "#d20f39"}}},
        {"nord", "Nord", "Arctic, north-bluish calm", "dots", {{"background", "#2e3440"}, {"foreground", "#eceff4"}, {"accent", "#88c0d0"},
            {"surface", "#3b4252"}, {"muted", "#b8c1d1"}, {"border", "#4c566a"}, {"sidebar", "#292e39"}, {"success", "#a3be8c"}, {"warning", "#ebcb8b"}, {"danger", "#e5939b"}}},
        {"gruvbox", "Gruvbox", "Retro warm browns and orange", "dots", {{"background", "#282828"}, {"foreground", "#ebdbb2"}, {"accent", "#fe8019"},
            {"surface", "#32302f"}, {"muted", "#a89984"}, {"border", "#504945"}, {"sidebar", "#1d2021"}, {"success", "#8ec07c"}, {"warning", "#fabd2f"}, {"danger", "#fb4934"}}},
        {"rose-pine", "Rosé Pine", "Muted pine, gold and rose", "dots", {{"background", "#191724"}, {"foreground", "#e0def4"}, {"accent", "#c4a7e7"},
            {"surface", "#1f1d2e"}, {"muted", "#908caa"}, {"border", "#2f2b44"}, {"sidebar", "#14121f"}, {"success", "#9ccfd8"}, {"warning", "#f6c177"}, {"danger", "#eb6f92"}}},
        {"dracula", "Dracula", "Dark purple with vivid accents", "dots", {{"background", "#282a36"}, {"foreground", "#f8f8f2"}, {"accent", "#bd93f9"},
            {"surface", "#303341"}, {"muted", "#a4abd0"}, {"border", "#44475a"}, {"sidebar", "#21222c"}, {"success", "#50fa7b"}, {"warning", "#f1fa8c"}, {"danger", "#ff5555"}}},
        {"everforest", "Everforest", "Green forest, easy on the eyes", "dots", {{"background", "#2d353b"}, {"foreground", "#e2d8bf"}, {"accent", "#a7c080"},
            {"surface", "#343f44"}, {"muted", "#9da9a0"}, {"border", "#475258"}, {"sidebar", "#272e33"}, {"success", "#83c092"}, {"warning", "#dbbc7f"}, {"danger", "#e67e80"}}},
        {"kanagawa", "Kanagawa", "Ink and wave, after Hokusai", "dots", {{"background", "#1f1f28"}, {"foreground", "#dcd7ba"}, {"accent", "#7e9cd8"},
            {"surface", "#2a2a37"}, {"muted", "#a6a69c"}, {"border", "#363646"}, {"sidebar", "#16161d"}, {"success", "#98bb6c"}, {"warning", "#e6c384"}, {"danger", "#e46876"}}},
        {"solarized-light", "Solarized Light", "Warm paper, precise accents", "dots", {{"background", "#fdf6e3"}, {"foreground", "#073642"}, {"accent", "#268bd2"},
            {"surface", "#fffcf2"}, {"muted", "#586e75"}, {"border", "#e6dcc0"}, {"sidebar", "#f5eedb"}, {"field", "#fffdf7"}, {"raised", "#fffefa"}, {"success", "#1d7a70"}, {"warning", "#8a6800"}, {"danger", "#c02b28"}}},
        {"synthwave", "Synthwave", "Neon magenta and cyan on deep purple", "grid", {{"background", "#160c2c"}, {"foreground", "#f5e9ff"}, {"accent", "#ff4fd8"},
            {"surface", "#21123f"}, {"muted", "#b9a3d9"}, {"border", "#3a2463"}, {"sidebar", "#110823"}, {"success", "#3cf2c4"}, {"warning", "#ffd166"}, {"danger", "#ff5f7e"}}},
        {"graphite", "Graphite", "Matte black with an amber accent", "none", {{"background", "#101010"}, {"foreground", "#e4e4e4"}, {"accent", "#f0a33a"},
            {"surface", "#181818"}, {"muted", "#9c9c9c"}, {"border", "#2a2a2a"}, {"sidebar", "#0b0b0b"}, {"success", "#8fc98f"}, {"warning", "#f0c46c"}, {"danger", "#ef7f7f"}}}};
    return list;
}
QStringList Theme::keys() { QStringList out{"omarchy"}; for (const auto &t : catalog()) out << t.key; return out; }
QVariantMap Theme::builtin(const QString &key) {
    for (const auto &t : catalog()) if (key == t.key) return polish(t.colors);
    return {};
}
static QVariantList swatches(const QVariantMap &c) {
    QVariantList out;
    for (auto key : {"background", "surface", "accent", "foreground", "success", "warning", "danger"}) out << c[key];
    return out;
}
QVariantList Theme::themes() const {
    QVariantList out;
    if (omarchyAvailable()) {
        const auto c = omarchy_.isEmpty() ? builtin("dark") : omarchy_;
        out << QVariantMap{{"key", "omarchy"}, {"title", "Follow Omarchy"}, {"detail", "Your desktop's theme, live"}, {"light", luminance(QColor(c["background"].toString())) > .5}, {"swatches", swatches(c)}};
    }
    for (const auto &t : catalog()) {
        const auto c = polish(t.colors);
        out << QVariantMap{{"key", t.key}, {"title", t.title}, {"detail", t.detail}, {"light", luminance(QColor(c["background"].toString())) > .5}, {"swatches", swatches(c)}};
    }
    return out;
}
QString Theme::texture() const {
    for (const auto &t : catalog()) if (mode_ == t.key) return t.texture;
    return "dots";
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
        // Newer Omarchy palettes describe more than three colors; use what they give.
        auto color = [&](const char *key) { auto value = table[key].value<std::string>(); QColor c(value ? QString::fromStdString(*value) : QString{}); return c.isValid() && c.alpha() == 255 ? c : QColor(); };
        if (auto lighter = color("lighter_background"); lighter.isValid()) candidate["surface"] = blend(bg, lighter, .7).name();
        if (auto darker = color("dark_background"); darker.isValid()) { candidate["sidebar"] = darker.name(); candidate["field"] = light ? "#ffffff" : darker.name(); }
        if (auto m = color("muted"); m.isValid()) candidate["muted"] = m.name();
        if (auto selection = color("selection"); selection.isValid()) candidate["accentSoft"] = blend(QColor(candidate["surface"].toString()), selection, .8).name();
        if (auto green = color("green"); green.isValid()) candidate["success"] = green.name();
        if (auto yellow = color("yellow"); yellow.isValid()) candidate["warning"] = yellow.name();
        if (auto red = color("red"); red.isValid()) candidate["danger"] = red.name();
        candidate["border"] = blend(bg, blend(fg, QColor(candidate["accent"].toString()), .3), .16).name();
        colors = polish(candidate);
        return true;
    } catch (const toml::parse_error &e) {
        error = QString::fromUtf8(e.description().data(), qsizetype(e.description().size()));
        return false;
    }
}
Theme::Theme(QString path, QObject *parent) : QObject(parent), path_(std::move(path)), colors_(builtin("dark")) {
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
    // The Omarchy palette is read even while another theme is on, so the picker can show it.
    QFile file(path_);
    QString failure;
    QVariantMap candidate;
    if (!file.open(QIODevice::ReadOnly)) failure = "Palette unavailable; using last good palette or dark fallback";
    else if (file.size() > 65536) failure = "Palette exceeds 64 KiB limit";
    else if (parse(file.readAll(), candidate, failure)) omarchy_ = candidate;
    if (mode_ == "omarchy") {
        if (!omarchy_.isEmpty()) colors_ = omarchy_;
        status_ = failure.isEmpty() ? "Following Omarchy · live palette" : failure;
    }
    emit changed();
}
void Theme::setMode(QString mode) {
    if (!keys().contains(mode)) return;
    mode_ = mode;
    if (mode == "omarchy") { reload(); return; }
    colors_ = builtin(mode);
    for (const auto &t : catalog()) if (mode == t.key) status_ = QString("Built-in theme · %1").arg(t.title);
    emit changed();
}
