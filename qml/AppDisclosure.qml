// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: section
    default property alias details: body.data
    property string title: "More details"
    property bool expanded: false
    property string resetKey: ""
    onResetKeyChanged: expanded = false
    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight
    implicitWidth: layout.implicitWidth
    data: ColumnLayout {
        id: layout
        width: parent.width
        spacing: section.expanded ? 8 : 0
        AppButton {
            objectName: section.objectName + "Toggle"
            Layout.fillWidth: true
            implicitHeight: 34
            leading: true
            text: section.title
            iconName: section.expanded ? "chevron" : "next"
            tone: "quiet"
            ink: section.expanded ? theme.colors.accent : theme.colors.muted
            Accessible.description: section.expanded ? "Expanded. Activate to hide details." : "Collapsed. Activate to show details."
            onClicked: section.expanded = !section.expanded
        }
        ColumnLayout {
            id: body
            objectName: section.objectName + "Body"
            visible: section.expanded
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            spacing: 10
        }
    }
}
