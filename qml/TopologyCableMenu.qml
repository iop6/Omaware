// SPDX-License-Identifier: GPL-3.0-or-later
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right-click menu for a cable on the network map.
AppMenu {
    id: cableMenu
    objectName: "topologyCableMenu"
    property var cable: null
    readonly property var vmNode: cable ? topo.graph.byId[cable.vm] : null
    MenuItem {
        enabled: false
        contentItem: Label {
            textFormat: Text.PlainText
            text: cableMenu.cable ? (cableMenu.cable.addresses && cableMenu.cable.addresses.length ? cableMenu.cable.addresses.join(", ") + "  ·  " : "") + cableMenu.cable.mac : ""
            color: theme.colors.muted
            font.pixelSize: Math.round(11 * theme.textScale)
        }
        background: Item {}
    }
    AppMenuItem {
        objectName: "topologyToggleCable"
        text: cableMenu.cable && cableMenu.cable.up ? "Pull cable" : "Plug cable in"
        enabled: !!cableMenu.cable && cableMenu.cable.state !== "next" && !!cableMenu.vmNode && cableMenu.vmNode.owned
        onTriggered: topo.setLinks([cableMenu.cable], !cableMenu.cable.up)
    }
    AppMenu {
        id: moveMenu
        title: "Move to"
        enabled: !!cableMenu.vmNode && cableMenu.vmNode.owned && !!cableMenu.cable && cableMenu.cable.state !== "removing"
        Instantiator {
            model: cableMenu.vmNode ? topo.switchesFor(cableMenu.vmNode) : []
            AppMenuItem {
                required property var modelData
                text: modelData.label
                enabled: modelData.ok && !!cableMenu.cable && modelData.id !== cableMenu.cable.to
                onTriggered: topo.cableTo(cableMenu.cable.vm, modelData.id, cableMenu.cable.mac)
            }
            onObjectAdded: function (index, object) {
                moveMenu.insertItem(index, object);
            }
            onObjectRemoved: function (index, object) {
                moveMenu.removeItem(object);
            }
        }
    }
    AppMenuItem {
        text: "Remove adapter…"
        enabled: !!cableMenu.vmNode && cableMenu.vmNode.owned && !!cableMenu.cable && cableMenu.cable.state !== "removing"
        onTriggered: {
            removeConfirm.cable = cableMenu.cable;
            removeConfirm.open();
        }
    }
}
