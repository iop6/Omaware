// SPDX-License-Identifier: GPL-3.0-or-later
#include "guestinput.h"
#include <QHash>
#include <QStringList>

namespace {
// Linux input event codes (linux/input-event-codes.h).
enum : unsigned {
    Esc = 1, One = 2, Zero = 11, Minus = 12, Equal = 13, Backspace = 14, Tab = 15, LeftBrace = 26, RightBrace = 27, Enter = 28,
    LeftCtrl = 29, Semicolon = 39, Apostrophe = 40, Grave = 41, LeftShift = 42, Backslash = 43, Comma = 51, Dot = 52, Slash = 53,
    LeftAlt = 56, Space = 57, CapsLock = 58, F1 = 59, F11 = 87, F12 = 88, SysRq = 99, RightAlt = 100, Home = 102, Up = 103,
    PageUp = 104, Left = 105, Right = 106, End = 107, Down = 108, PageDown = 109, Insert = 110, Delete = 111, Pause = 119,
    LeftMeta = 125, Menu = 127
};
// Letter keys in alphabetical order.
const unsigned letters[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
// Characters of the US layout: the key, and whether Shift is held.
const QHash<QChar, QPair<unsigned, bool>> &symbols() {
    static const QHash<QChar, QPair<unsigned, bool>> map = [] {
        QHash<QChar, QPair<unsigned, bool>> m;
        const QString digits = "1234567890", shifted = "!@#$%^&*()";
        for (int i = 0; i < 10; ++i) { m[digits[i]] = {One + unsigned(i), false}; m[shifted[i]] = {One + unsigned(i), true}; }
        const QList<std::tuple<QChar, QChar, unsigned>> pairs{{'-', '_', Minus}, {'=', '+', Equal}, {'[', '{', LeftBrace}, {']', '}', RightBrace},
            {';', ':', Semicolon}, {'\'', '"', Apostrophe}, {'`', '~', Grave}, {'\\', '|', Backslash}, {',', '<', Comma}, {'.', '>', Dot}, {'/', '?', Slash}};
        for (const auto &[plain, shift, key] : pairs) { m[plain] = {key, false}; m[shift] = {key, true}; }
        m[' '] = {Space, false}; m['\n'] = {Enter, false}; m['\t'] = {Tab, false};
        return m;
    }();
    return map;
}
const QHash<QString, unsigned> &named() {
    static const QHash<QString, unsigned> map = [] {
        QHash<QString, unsigned> m{{"ctrl", LeftCtrl}, {"control", LeftCtrl}, {"shift", LeftShift}, {"alt", LeftAlt}, {"altgr", RightAlt},
            {"win", LeftMeta}, {"super", LeftMeta}, {"meta", LeftMeta}, {"cmd", LeftMeta}, {"enter", Enter}, {"return", Enter},
            {"esc", Esc}, {"escape", Esc}, {"tab", Tab}, {"space", Space}, {"backspace", Backspace}, {"delete", Delete}, {"del", Delete},
            {"insert", Insert}, {"home", Home}, {"end", End}, {"pageup", PageUp}, {"pagedown", PageDown}, {"up", Up}, {"down", Down},
            {"left", Left}, {"right", Right}, {"capslock", CapsLock}, {"printscreen", SysRq}, {"pause", Pause}, {"menu", Menu},
            {"f11", F11}, {"f12", F12}};
        for (unsigned i = 1; i <= 10; ++i) m["f" + QString::number(i)] = F1 + i - 1;
        return m;
    }();
    return map;
}
}

bool GuestInput::chordsForText(const QString &text, QList<Chord> &chords, QString &error) {
    chords.clear();
    for (const QChar c : text) {
        if (c == '\r') continue;
        if (c >= 'a' && c <= 'z') { chords.append(Chord{letters[c.unicode() - 'a']}); continue; }
        if (c >= 'A' && c <= 'Z') { chords.append(Chord{LeftShift, letters[c.unicode() - 'A']}); continue; }
        const auto found = symbols().find(c);
        if (found == symbols().end()) { error = QString("“%1” can't be typed on a US keyboard layout.").arg(c); chords.clear(); return false; }
        chords.append(found->second ? Chord{LeftShift, found->first} : Chord{found->first});
    }
    return true;
}
bool GuestInput::chordForKeys(const QString &keys, Chord &chord, QString &error) {
    chord.clear();
    const auto parts = keys.toLower().remove(' ').split('+', Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 5) { error = "Give one to five keys joined with +, for example ctrl+alt+t."; return false; }
    for (const auto &part : parts) {
        unsigned code = named().value(part, 0);
        if (!code && part.size() == 1) {
            const QChar c = part[0];
            if (c >= 'a' && c <= 'z') code = letters[c.unicode() - 'a'];
            else if (auto found = symbols().find(c); found != symbols().end() && !found->second) code = found->first;
        }
        if (!code) { error = "Unknown key “" + part + "”. Use letters, digits or: " + keyNames(); chord.clear(); return false; }
        if (!chord.contains(code)) chord.append(code);
    }
    return true;
}
QString GuestInput::keyNames() {
    return "ctrl, shift, alt, altgr, win, enter, esc, tab, space, backspace, delete, insert, home, end, pageup, pagedown, up, down, left, right, f1–f12, capslock, printscreen, pause, menu";
}
