// SPDX-License-Identifier: GPL-3.0-or-later
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks what a VM dropped onto a network should do: add a new connection, or move one of its
// existing connections there.
AppMenu {
    id: dropMenu
    objectName: "topologyDropMenu"
    property string vmId: ""
    property string targetId: ""
    readonly property var vmNode: vmId ? topo.graph.byId[vmId] : null
    MenuItem {
        enabled: false
        contentItem: Label {
            textFormat: Text.PlainText
            text: "Connect to " + (dropMenu.targetId === "host" ? "a private internet connection" : dropMenu.targetId && topo.graph.byId[dropMenu.targetId] ? topo.graph.byId[dropMenu.targetId].label : "")
            color: theme.colors.muted
            font.pixelSize: Math.round(11 * theme.textScale)
        }
        background: Item {}
    }
    AppMenuItem {
        objectName: "dropNewAdapter"
        text: "Add a new connection"
        onTriggered: topo.cableTo(dropMenu.vmId, dropMenu.targetId, "")
    }
    Instantiator {
        model: dropMenu.vmNode ? dropMenu.vmNode.interfaces : []
        AppMenuItem {
            required property var modelData
            text: "Move the connection to " + topo.targetName(modelData.networkId) + " here"
            enabled: modelData.networkId !== (dropMenu.targetId === "host" ? "user" : dropMenu.targetId)
            onTriggered: topo.cableTo(dropMenu.vmId, dropMenu.targetId, modelData.mac)
        }
        onObjectAdded: function (index, object) {
            dropMenu.insertItem(index + 2, object);
        }
        onObjectRemoved: function (index, object) {
            dropMenu.removeItem(object);
        }
    }
}
