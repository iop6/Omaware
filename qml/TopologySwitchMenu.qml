// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right-click menu for a network (a switch) on the network map.
AppMenu {
    id: switchMenu
    objectName: "topologySwitchMenu"
    property var node: ({})
    readonly property var net: node.network || null
    readonly property var attached: node.id ? topo.graph.cables.filter(function (c) {
        return c.to === switchMenu.node.id && c.state !== "next";
    }) : []
    readonly property int users: net ? (net.users || []).length + (net.systemUsers || []).length : 0
    MenuItem {
        enabled: false
        contentItem: Label {
            textFormat: Text.PlainText
            text: (switchMenu.node.label || "") + "  ·  " + topo.kindText(switchMenu.node).toLowerCase()
            color: theme.colors.muted
            elide: Text.ElideRight
            font.pixelSize: Math.round(11 * theme.textScale)
        }
        background: Item {}
    }
    AppMenuItem {
        visible: !!switchMenu.net && switchMenu.node.managed && switchMenu.node.running && switchMenu.node.needsPermission
        height: visible ? implicitHeight : 0
        text: "Allow VMs to join…"
        onTriggered: topo.networkAction(switchMenu.net, "authorize")
    }
    AppMenuItem {
        text: "Pull every cable on this network"
        enabled: switchMenu.attached.some(function (c) {
            return c.up;
        })
        onTriggered: topo.setLinks(switchMenu.attached, false)
    }
    AppMenuItem {
        text: "Plug every cable back in"
        enabled: switchMenu.attached.some(function (c) {
            return !c.up;
        })
        onTriggered: topo.setLinks(switchMenu.attached, true)
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        visible: !!switchMenu.net && switchMenu.node.managed
        height: visible ? implicitHeight : 0
        text: switchMenu.node.running ? "Stop network" : "Start network"
        enabled: !backend.busy && (!switchMenu.node.running || switchMenu.users === 0)
        onTriggered: topo.networkAction(switchMenu.net, switchMenu.node.running ? "stop" : "start")
    }
    AppMenuItem {
        visible: !!switchMenu.net && switchMenu.node.managed
        height: visible ? implicitHeight : 0
        text: "Edit…"
        enabled: !backend.busy && !switchMenu.node.running && switchMenu.users === 0
        onTriggered: topo.editNetwork(switchMenu.net)
    }
    AppMenuItem {
        visible: !!switchMenu.net && switchMenu.node.managed
        height: visible ? implicitHeight : 0
        text: "Remove…"
        enabled: !backend.busy && !switchMenu.node.running && switchMenu.users === 0
        onTriggered: topo.networkAction(switchMenu.net, "remove")
    }
}
