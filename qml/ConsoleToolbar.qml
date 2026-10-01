// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: toolbar
    required property var workspace
    required property var guestDisplay
    required property var history
    property bool showLabels: false
    readonly property bool keyboardNavigation: { for (let i = 0; i < actions.children.length; ++i) if (actions.children[i].visualFocus === true) return true; return false }
    readonly property bool clipboardConfigured: backend.details.uuid === workspace.selectedUuid && !!backend.details.liveClipboardConfigured
    readonly property string clipboardStatus: !clipboardConfigured ? "Needs setup" : !guestDisplay.connected ? "Disconnected" : guestDisplay.clipboardMode === "off" ? "Off" : guestDisplay.clipboardMode === "both" ? "Two-way" : "To guest"
    readonly property bool popupOpen: toolsMenu.visible || keysMenu.visible || sizeMenu.visible || connectionMenu.visible || clipboardPopup.visible || consoleNotice.visible
    implicitWidth: actions.implicitWidth
    implicitHeight: actions.implicitHeight
    function closePopups() { if (toolsMenu) toolsMenu.close(); if (keysMenu) keysMenu.close(); if (sizeMenu) sizeMenu.close(); if (connectionMenu) connectionMenu.close(); if (clipboardPopup) clipboardPopup.close(); if (consoleNotice) consoleNotice.close() }
    function openTools() { guestDisplay.releaseInput(); toolsMenu.openBelow(toolsButton) }
    onParentChanged: closePopups()
    Connections { target: workspace; function onSelectedUuidChanged() { toolbar.closePopups() } function onDetailsOpenChanged() { toolbar.closePopups() } }

    Row {
        id: actions
        spacing: 4
        AppButton { objectName: "consoleSnapshot"; text: toolbar.showLabels ? "Snapshot" : ""; iconName: "snapshot"; tone: "quiet"; hint: "Take snapshot"; enabled: toolbar.workspace.permitted && toolbar.history.canCapture; onClicked: { toolbar.guestDisplay.releaseInput(); toolbar.history.requestCapture(toolbar.Overlay.overlay) } }
        AppButton { objectName: "consoleRevert"; text: toolbar.showLabels ? "Revert" : ""; iconName: "history"; tone: "quiet"; hint: toolbar.history.currentSnapshot.name ? "Revert to " + toolbar.history.currentSnapshot.name : "Take a snapshot first"; enabled: toolbar.workspace.permitted && toolbar.history.ready && !!toolbar.history.currentSnapshot.name && !toolbar.history.snapshotData.restoreBlocker; onClicked: { toolbar.guestDisplay.releaseInput(); toolbar.history.revertCurrent(toolbar.Overlay.overlay) } }
        Rectangle { width: 1; height: 20; anchors.verticalCenter: parent.verticalCenter; color: theme.colors.border }
        AppButton { objectName: "consoleInput"; text: toolbar.showLabels && toolbar.guestDisplay.captured ? "Ctrl+Alt to release" : ""; iconName: "keyboard"; tone: "quiet"; checked: toolbar.guestDisplay.captured; hint: toolbar.guestDisplay.captured ? "Keyboard captured · Ctrl+Alt to release" : "Click the display to use the guest keyboard and mouse"; onClicked: toolbar.guestDisplay.releaseInput()
            Rectangle { visible: toolbar.guestDisplay.captured; width: 5; height: 5; radius: 3; color: theme.colors.accent; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 5 }
        }
        AppButton { id: clipboardButton; objectName: "consoleClipboard"; iconName: "clipboard"; tone: "quiet"; checked: toolbar.guestDisplay.clipboardMode !== "off"; hint: "Clipboard · " + toolbar.clipboardStatus; enabled: !!toolbar.workspace.selected; onClicked: { toolbar.guestDisplay.releaseInput(); clipboardPopup.open() } }
        AppButton { objectName: "consoleFullscreen"; iconName: toolbar.workspace.consoleFullScreen ? "collapse" : "expand"; tone: "quiet"; hint: toolbar.workspace.consoleFullScreen ? "Leave fullscreen" : "Fullscreen"; enabled: !!toolbar.workspace.selected; onClicked: toolbar.workspace.toggleConsoleFullscreen() }
        AppButton { id: toolsButton; objectName: "consoleToolsMenu"; iconName: "more"; tone: "quiet"; hint: "Console tools"; enabled: !!toolbar.workspace.selected; onClicked: toolbar.openTools() }
    }
    // Only the toolbar handles this shortcut; guest right-clicks remain guest input.
    TapHandler { acceptedButtons: Qt.RightButton; onTapped: toolbar.openTools() }
    Keys.onPressed: function(event) { if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) { toolbar.openTools(); event.accepted = true } }

    AppMenu {
        id: toolsMenu; objectName: "consoleMenu"; title: "Console tools"
        Action { objectName: "consoleFocusAction"; text: toolbar.workspace.consoleExpanded ? "Leave focus view" : "Focus view"; enabled: !toolbar.workspace.consoleDetached && !toolbar.workspace.consoleFullScreen; onTriggered: toolbar.workspace.consoleExpanded = !toolbar.workspace.consoleExpanded }
        Action { objectName: "consoleDetachAction"; text: toolbar.workspace.consoleDetached ? "Dock console" : "Detach console"; onTriggered: toolbar.workspace.consoleDetached = !toolbar.workspace.consoleDetached }
        MenuSeparator {}
        AppMenu { id: keysMenu; objectName: "consoleKeysMenu"; title: "Send keys"
            Action { text: "Ctrl+Alt+Delete"; enabled: toolbar.guestDisplay.connected; onTriggered: toolbar.guestDisplay.sendSpecial("ctrlaltdel") }
            Action { text: "Alt+F4"; enabled: toolbar.guestDisplay.connected; onTriggered: toolbar.guestDisplay.sendSpecial("altf4") }
            Action { text: "Super key"; enabled: toolbar.guestDisplay.connected; onTriggered: toolbar.guestDisplay.sendSpecial("super") }
        }
        AppMenu { id: sizeMenu; objectName: "consoleSizeMenu"; title: "Display size"
            Action { text: "Request 1280 × 800 display"; enabled: toolbar.guestDisplay.connected; onTriggered: toolbar.guestDisplay.resizeGuest(1280, 800) }
            Action { text: "Request 1920 × 1080 display"; enabled: toolbar.guestDisplay.connected; onTriggered: toolbar.guestDisplay.resizeGuest(1920, 1080) }
        }
        AppMenu { id: connectionMenu; objectName: "consoleConnectionMenu"; title: "Connection"
            Action { text: "Reconnect console"; enabled: toolbar.workspace.permitted && toolbar.workspace.displayAvailable; onTriggered: toolbar.workspace.reopenConsole() }
            Action { text: "Close console"; enabled: toolbar.guestDisplay.connected; onTriggered: toolbar.workspace.closeConsole() }
        }
        MenuSeparator {}
        Action { text: "Console details…"; onTriggered: consoleNotice.open() }
    }
    Popup {
        id: clipboardPopup; objectName: "consoleClipboardPopup"
        parent: toolbar
        x: toolbar.width - width; y: toolbar.height + 6
        width: Math.min(330 * theme.textScale, toolbar.Window.window ? toolbar.Window.window.width - 24 : 330)
        padding: 18
        background: Rectangle { radius: 4; color: theme.colors.raised; border.color: theme.colors.border }
        contentItem: ColumnLayout {
            spacing: 8
            RowLayout { Layout.fillWidth: true
                Label { text: "Clipboard"; font.weight: Font.DemiBold; Layout.fillWidth: true }
                Label { objectName: "clipboardStatus"; text: toolbar.clipboardStatus; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) }
            }
            Label { visible: !toolbar.clipboardConfigured; text: "Enable the clipboard channel in Details → Hardware, then fully shut down and start the VM. Install spice-vdagent inside the guest."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.muted }
            Label { visible: toolbar.clipboardConfigured; text: "Sharing requires spice-vdagent running inside the guest."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) }
            ButtonGroup { id: clipboardDirection }
            AppRadioButton { objectName: "clipboardOff"; text: "Off"; checked: toolbar.guestDisplay.clipboardMode === "off"; ButtonGroup.group: clipboardDirection; onClicked: toolbar.guestDisplay.clipboardMode = "off" }
            AppRadioButton { objectName: "clipboardToGuest"; text: "To guest"; checked: toolbar.guestDisplay.clipboardMode === "toGuest"; enabled: toolbar.clipboardConfigured && toolbar.guestDisplay.connected; ButtonGroup.group: clipboardDirection; onClicked: toolbar.guestDisplay.clipboardMode = "toGuest" }
            AppRadioButton { objectName: "clipboardBoth"; text: "Both directions"; checked: toolbar.guestDisplay.clipboardMode === "both"; enabled: toolbar.clipboardConfigured && toolbar.guestDisplay.connected; ButtonGroup.group: clipboardDirection; onClicked: toolbar.guestDisplay.clipboardMode = "both" }
            AppButton { objectName: "consolePaste"; text: "Paste clipboard"; iconName: "clipboard"; Layout.fillWidth: true; enabled: toolbar.clipboardConfigured && toolbar.guestDisplay.connected && toolbar.guestDisplay.clipboardMode !== "off"; onClicked: { toolbar.guestDisplay.pasteClipboard(); clipboardPopup.close() } }
        }
    }
    AppDialog {
        id: consoleNotice; objectName: "consoleNotice"
        parent: toolbar.Overlay.overlay
        heading: "Console details"; headerIcon: "info"
        width: Math.min(480, parent ? parent.width - 40 : 480)
        contentItem: ColumnLayout { spacing: 14
            Label { text: toolbar.guestDisplay.status; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Label { text: "Ctrl+Alt releases keyboard and mouse input.\n\nDisplay size requests require a compatible guest display driver. Closing OmaWare keeps your VMs running."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
    }
}
