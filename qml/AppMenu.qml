// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
Menu {
    function openBelow(anchor) { popup(anchor, anchor.width - width, anchor.height + 4) }
    padding: 6
    implicitWidth: 270 * theme.textScale
    delegate: AppMenuItem {}
    background: Rectangle { color: theme.colors.raised; border.color: theme.colors.border; radius: 4 }
}
