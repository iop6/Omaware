// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
Item {
    id: indicator
    property bool running: true
    implicitWidth: 24; implicitHeight: 24
    Accessible.role: Accessible.Indicator
    Accessible.name: "Working"
    BusyIndicator { anchors.fill: parent; running: indicator.running && !theme.reducedMotion }
    AppIcon { anchors.fill: parent; visible: indicator.running && theme.reducedMotion; name: "history"; color: theme.colors.accent }
}
