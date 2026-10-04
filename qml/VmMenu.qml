// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right-click menu for a VM in the library or monitor. Power and organize actions target that VM
// directly; workspace actions (console, details, snapshots) select it first.
AppMenu {
    id: vmMenu
    objectName: "vmContextMenu"
    property var vm: ({})
    readonly property bool owned: !!vm.owned
    readonly property bool ready: backend.connected && !backend.busy && owned
    readonly property int code: vm.stateCode || 0
    readonly property var meta: vm.uuid ? root.vmMeta(vm.uuid) : ({})
    implicitWidth: 262 * theme.textScale
    MenuItem {
        enabled: false
        contentItem: Label {
            textFormat: Text.PlainText
            text: vmMenu.vm.uuid ? root.vmLabel(vmMenu.vm) + "  ·  " + String(vmMenu.vm.state || "").toLowerCase() : ""
            color: theme.colors.muted
            elide: Text.ElideRight
            font.pixelSize: Math.round(11 * theme.textScale)
        }
        background: Item {}
    }
    AppMenuItem {
        objectName: "vmMenuConsole"
        text: "Open console"
        onTriggered: {
            root.selectVm(vmMenu.vm.uuid);
            root.detailsOpen = false;
        }
    }
    AppMenuItem {
        objectName: "vmMenuDetails"
        text: "Details"
        onTriggered: {
            root.selectVm(vmMenu.vm.uuid);
            if (vmDetails.page === 3)
                vmDetails.page = 0;
            root.detailsOpen = true;
        }
    }
    AppMenuItem {
        objectName: "vmMenuSnapshots"
        text: "Snapshots"
        onTriggered: {
            root.selectVm(vmMenu.vm.uuid);
            vmDetails.page = 3;
            root.detailsOpen = true;
        }
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        objectName: "vmMenuPower"
        text: vmMenu.code === 5 ? "Start" : vmMenu.code === 3 ? "Resume" : "Pause"
        enabled: vmMenu.ready && (vmMenu.code === 1 || vmMenu.code === 3 || vmMenu.code === 5)
        onTriggered: backend.action(vmMenu.vm.uuid, vmMenu.code === 5 ? "start" : vmMenu.code === 3 ? "resume" : "pause")
    }
    AppMenuItem {
        objectName: "vmMenuShutdown"
        text: "Shut down guest"
        enabled: vmMenu.ready && vmMenu.code === 1
        onTriggered: backend.action(vmMenu.vm.uuid, "shutdown")
    }
    AppMenuItem {
        objectName: "vmMenuForceOff"
        text: "Force power off…"
        enabled: vmMenu.ready && vmMenu.code !== 5
        onTriggered: root.confirmFor(false, vmMenu.vm)
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        objectName: "vmMenuSnapshot"
        text: "Take snapshot…"
        enabled: vmMenu.ready && !vmMenu.vm.diskless
        onTriggered: root.queueVmAction(vmMenu.vm.uuid, "snapshot")
    }
    AppMenuItem {
        objectName: "vmMenuRevert"
        text: "Revert to current snapshot…"
        enabled: vmMenu.ready && !vmMenu.vm.diskless
        onTriggered: root.queueVmAction(vmMenu.vm.uuid, "revert")
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        objectName: "vmMenuFavorite"
        text: vmMenu.meta.favorite ? "Remove from favorites" : "Add to favorites"
        onTriggered: root.toggleFavorite(vmMenu.vm)
    }
    AppMenuItem {
        objectName: "vmMenuOrganize"
        text: "Rename & organize…"
        onTriggered: organizeDialog.openFor(vmMenu.vm)
    }
    AppMenuItem {
        objectName: "vmMenuNetworkMap"
        text: "Show on network map"
        onTriggered: {
            root.navigation = "networks";
            networksPage.showVm(vmMenu.vm.uuid);
        }
    }
    AppMenuItem {
        objectName: "vmMenuContain"
        text: vmMenu.vm.contained ? "Review containment…" : "Contain VM…"
        enabled: vmMenu.owned
        onTriggered: {
            root.selectVm(vmMenu.vm.uuid);
            vmDetails.page = 2;
            root.detailsOpen = true;
        }
    }
    AppMenuItem {
        objectName: "vmMenuCopyName"
        text: "Copy name"
        onTriggered: preferences.copy(vmMenu.vm.name)
    }
    AppMenuItem {
        objectName: "vmMenuCopyUuid"
        text: "Copy UUID"
        onTriggered: preferences.copy(vmMenu.vm.uuid)
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        objectName: "vmMenuRemove"
        text: "Remove definition…"
        enabled: vmMenu.ready && vmMenu.code === 5
        onTriggered: root.confirmFor(true, vmMenu.vm)
    }
    AppMenuItem {
        objectName: "vmMenuDelete"
        text: "Delete VM…"
        enabled: vmMenu.ready
        onTriggered: deleteDialog.openFor(vmMenu.vm)
    }
}
