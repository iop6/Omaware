// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Confirms removing a VM's network adapter (a cable) from the map.
AppDialog {
    id: removeConfirm
    objectName: "topologyRemoveAdapter"
    property var cable: null
    anchors.centerIn: parent
    width: Math.min(440, topo.width - 40)
    modal: true
    padding: 22
    heading: "Remove this connection?"
    headerIcon: "network"
    contentItem: Label {
        textFormat: Text.PlainText
        text: removeConfirm.cable ? "The VM's network adapter " + removeConfirm.cable.mac + " is removed" + (removeConfirm.cable.live ? ", on the running VM too if its OS allows it." : ".") : ""
        wrapMode: Text.WordWrap
        color: theme.colors.muted
    }
    footer: RowLayout {
        spacing: 8
        Item {
            Layout.fillWidth: true
        }
        AppButton {
            text: "Cancel"
            onClicked: removeConfirm.close()
            Layout.bottomMargin: 16
        }
        AppButton {
            text: "Remove connection"
            tone: "danger"
            Layout.rightMargin: 16
            Layout.bottomMargin: 16
            onClicked: {
                topo.removeAdapter(removeConfirm.cable);
                removeConfirm.close();
            }
        }
    }
}
