// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown instead of the main window when another OmaWare is already open.
ApplicationWindow {
    id: window
    visible: true
    title: "OmaWare"
    width: 440; height: Math.max(190, content.implicitHeight + 48)
    minimumWidth: 360
    color: theme.colors.background
    ColumnLayout {
        id: content
        anchors.fill: parent; anchors.margins: 24
        spacing: 12
        Label { text: "OmaWare is already open"; font.pixelSize: Math.round(18 * theme.textScale); font.weight: Font.DemiBold; color: theme.colors.foreground; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Label {
            objectName: "alreadyRunningReason"
            textFormat: Text.PlainText
            text: blocker + " Switch to that window, or close it before opening this one. Two open copies would both manage the same VMs."
            color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true
        }
        Item { Layout.fillHeight: true }
        AppButton { objectName: "alreadyRunningOk"; text: "OK"; tone: "primary"; Layout.alignment: Qt.AlignRight; onClicked: Qt.quit() }
    }
}
