// SPDX-License-Identifier: GPL-3.0-or-later
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right-click menu for a VM on the network map.
AppMenu {
    id: vmMenu
    objectName: "topologyVmMenu"
    property var node: ({
            interfaces: []
        })
    readonly property var mine: node.id ? topo.graph.cables.filter(function (c) {
        return c.vm === vmMenu.node.id && c.state !== "next";
    }) : []
    MenuItem {
        enabled: false
        contentItem: Label {
            textFormat: Text.PlainText
            text: vmMenu.node.label ? vmMenu.node.label + "  ·  " + (topo.reachInfo[vmMenu.node.reach] || {
                    label: ""
                }).label.toLowerCase() : ""
            color: theme.colors.muted
            elide: Text.ElideRight
            font.pixelSize: Math.round(11 * theme.textScale)
        }
        background: Item {}
    }
    AppMenuItem {
        text: "Open console"
        onTriggered: topo.openVm(vmMenu.node.uuid)
    }
    AppMenuItem {
        objectName: "topologyPullAll"
        text: "Disconnect from everything"
        enabled: !!vmMenu.node.owned && vmMenu.mine.some(function (c) {
            return c.up;
        })
        onTriggered: topo.setLinks(vmMenu.mine, false)
    }
    AppMenuItem {
        text: "Reconnect all cables"
        enabled: !!vmMenu.node.owned && vmMenu.mine.some(function (c) {
            return !c.up;
        })
        onTriggered: topo.setLinks(vmMenu.mine, true)
    }
    AppMenu {
        id: addMenu
        objectName: "topologyAddCable"
        title: "Connect to"
        enabled: !!vmMenu.node.owned
        Instantiator {
            model: vmMenu.node.id ? topo.switchesFor(vmMenu.node) : []
            AppMenuItem {
                required property var modelData
                text: modelData.label
                enabled: modelData.ok
                onTriggered: topo.cableTo(vmMenu.node.id, modelData.id, "")
            }
            onObjectAdded: function (index, object) {
                addMenu.insertItem(index, object);
            }
            onObjectRemoved: function (index, object) {
                addMenu.removeItem(object);
            }
        }
    }
    AppMenuItem {
        text: "New network with this VM…"
        enabled: !!vmMenu.node.owned
        onTriggered: topo.createNetwork("nat", [vmMenu.node.uuid])
    }
    AppMenu {
        id: pingMenu
        objectName: "topologyPingMenu"
        title: "Trace a ping to"
        AppMenuItem {
            text: "The internet"
            onTriggered: topo.startTrace(vmMenu.node.id, "internet")
        }
        AppMenuItem {
            text: "This computer"
            onTriggered: topo.startTrace(vmMenu.node.id, "host")
        }
        Instantiator {
            model: vmMenu.node.id ? topo.graph.order.filter(function (id) {
                return topo.graph.byId[id].kind === "vm" && id !== vmMenu.node.id;
            }) : []
            AppMenuItem {
                required property var modelData
                text: topo.graph.byId[modelData] ? topo.graph.byId[modelData].label : ""
                onTriggered: topo.startTrace(vmMenu.node.id, modelData)
            }
            onObjectAdded: function (index, object) {
                pingMenu.insertItem(index + 2, object);
            }
            onObjectRemoved: function (index, object) {
                pingMenu.removeItem(object);
            }
        }
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        text: "More VM actions…"
        onTriggered: {
            const it = topo.items[vmMenu.node.id];
            topo.vmActions(vmMenu.node.vm, it, it ? it.width / 2 : 0, it ? it.height / 2 : 0);
        }
    }
}
