// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QString>

// Turns text and key names into Linux input keycodes for a VM's keyboard (libvirt's "linux" codeset).
// Text is typed as on a US keyboard layout, which is what freshly installed systems use; a guest set to
// another layout may show some symbols differently.
namespace GuestInput {
// One key press: the keys held together (modifiers first), for example {KEY_LEFTSHIFT, KEY_1} for "!".
using Chord = QList<unsigned>;
// The chords that type this text, or false (with the first character that can't be typed) for
// characters outside the US layout. Newlines press Enter and tabs press Tab.
bool chordsForText(const QString &text, QList<Chord> &chords, QString &error);
// A key combination such as "ctrl+alt+delete", "enter", "win+r" or "f2".
bool chordForKeys(const QString &keys, Chord &chord, QString &error);
// The key names chordForKeys understands, for error messages and tool descriptions.
QString keyNames();
}
