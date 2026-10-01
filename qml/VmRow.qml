// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One VM in the sidebar library, with a collapsible group header above the first row of its group.
Item {
    id: row
    required property var modelData
    required property int index
    property var shell: null          // the main window: selection, grouping and the VM menu
    property var stats: null
    property bool rail: false
    property bool compact: false
    property bool listFocused: false
    readonly property var vm: modelData
    readonly property var meta: shell.vmMeta(vm.uuid)
    readonly property string group: shell.groupOf(vm)
    readonly property bool firstInGroup: index === 0 || shell.groupOf(shell.filteredDomains[index - 1]) !== group
    readonly property bool showHeader: !rail && shell.grouped && firstInGroup
    readonly property bool folded: !rail && shell.grouped && !!shell.collapsedGroups[group]
    readonly property bool selected: shell.selectedUuid === vm.uuid
    readonly property bool marked: shell.marked.indexOf(vm.uuid) >= 0
    readonly property bool marking: !rail && shell.marked.length > 0
    readonly property var live: stats && stats.current[vm.uuid] ? stats.current[vm.uuid] : null
    readonly property bool running: vm.stateCode === 1
    readonly property color stateInk: vm.stateCode === 1 ? theme.colors.success : vm.stateCode === 3 ? theme.colors.warning : vm.stateCode === 6 ? theme.colors.danger : theme.colors.muted
    readonly property var chips: {
        let list = []
        if (shell.exitPaused.indexOf(vm.uuid) >= 0 && vm.stateCode === 3) list.push({text: "auto-paused", ink: theme.colors.warning})
        if (vm.contained) list.push({text: "contained", ink: theme.colors.danger})
        if (!vm.owned) list.push({text: "read-only", ink: theme.colors.muted})
        if (vm.diskless) list.push({text: "test", ink: theme.colors.muted})
        for (const tag of String(meta.tags || "").split(",").map(function(t) { return t.trim() }).filter(function(t) { return t !== "" }).slice(0, 2))
            list.push({text: tag, ink: theme.colors.accent})
        return list
    }
    width: ListView.view ? ListView.view.width : 0
    height: (showHeader ? header.height : 0) + (folded ? 0 : body.height + 4)
    visible: height > 0

    Item {
        id: header
        objectName: "libraryGroup_" + (row.group || "unsorted")
        visible: row.showHeader
        width: parent.width; height: 28
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 6; spacing: 6
            AppIcon { name: row.folded ? "next" : "chevron"; width: 12; height: 12; color: theme.colors.muted }
            Label {
                text: row.group === "favorites" ? "★ favorites" : row.group === "" ? "unsorted" : row.group
                color: row.group === "favorites" ? theme.colors.warning : theme.colors.muted
                font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true
            }
            Label { text: row.shell.groupCount(row.group); color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale) }
        }
        TapHandler { onTapped: row.shell.toggleGroup(row.group) }
        Accessible.role: Accessible.Button
        Accessible.name: (row.folded ? "Expand " : "Collapse ") + (row.group || "unsorted")
    }

    // A plain item with explicit handlers: left-click selects, Ctrl/Shift-click picks several,
    // double-click opens the console, right-click only opens the menu and never changes the selection.
    Item {
        id: body
        objectName: "vmRow_" + row.vm.uuid
        readonly property bool hovered: hover.hovered
        readonly property bool highlighted: row.marking ? row.marked : row.selected
        readonly property int padding: row.rail ? 0 : row.compact ? 8 : 10
        visible: !row.folded
        y: row.showHeader ? header.height : 0
        width: parent.width
        height: row.rail ? 46 : row.compact ? 40 : 60
        Accessible.role: Accessible.Button
        Accessible.name: row.vm.name + ", " + row.vm.state + (row.vm.contained ? ", contained" : "") + (row.marked ? ", selected" : "")
        ToolTip.visible: hover.hovered && (row.rail || nameLabel.truncated)
        ToolTip.delay: row.rail ? 250 : 800
        ToolTip.text: row.shell.vmLabel(row.vm) + " · " + row.vm.state + (row.vm.contained ? " · contained" : "")
        HoverHandler { id: hover }
        TapHandler {
            id: leftTap
            acceptedButtons: Qt.LeftButton
            onTapped: row.shell.clickVm(row.vm.uuid, leftTap.point.modifiers)
            onDoubleTapped: { row.shell.selectVm(row.vm.uuid); row.shell.detailsOpen = false }
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: function(point) { row.shell.openVmMenu(row.vm, body, point.position.x, point.position.y) }
        }
        Rectangle {
            anchors.fill: parent
            radius: 3
            color: body.highlighted ? theme.colors.accentSoft : body.hovered ? theme.colors.subtle : "transparent"
            border.color: row.listFocused && body.highlighted ? theme.colors.accent : "transparent"
            border.width: row.listFocused && body.highlighted ? 1 : 0
            Rectangle { visible: body.highlighted; width: 2; height: parent.height - 12; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; color: theme.colors.accent }
        }
        Item {
            anchors.fill: parent; anchors.margins: body.padding
            // Icon rail: icon, state dot and a containment mark.
            Item {
                visible: row.rail
                anchors.fill: parent
                AppIcon { anchors.centerIn: parent; name: "monitor"; width: 20; height: 20; color: row.selected ? theme.colors.accent : theme.colors.foreground }
                Rectangle { x: parent.width / 2 + 5; y: parent.height / 2 + 4; width: 8; height: 8; radius: 4; color: row.stateInk; border.width: 2; border.color: theme.colors.sidebar }
                Rectangle { visible: !!row.vm.contained; x: parent.width / 2 + 5; y: parent.height / 2 - 12; width: 8; height: 8; color: theme.colors.danger; border.width: 2; border.color: theme.colors.sidebar }
            }
            ColumnLayout {
                visible: !row.rail
                anchors.fill: parent
                spacing: 5
                RowLayout {
                    Layout.fillWidth: true; spacing: 7
                    // Check box while several VMs are being picked.
                    Rectangle {
                        objectName: "vmRowCheck"
                        visible: row.marking
                        Layout.preferredWidth: 13; Layout.preferredHeight: 13; radius: 2
                        color: row.marked ? theme.colors.accent : "transparent"
                        border.color: row.marked ? theme.colors.accent : theme.colors.muted
                        Label { anchors.centerIn: parent; visible: row.marked; text: "✓"; color: theme.colors.accentText; font.pixelSize: Math.round(10 * theme.textScale); font.weight: Font.Bold }
                        TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: row.shell.clickVm(row.vm.uuid, Qt.ControlModifier) }
                    }
                    Label { text: row.vm.stateCode === 1 ? "●" : row.vm.stateCode === 3 ? "‖" : row.vm.stateCode === 6 ? "✕" : "○"; color: row.stateInk; font.pixelSize: Math.round(11 * theme.textScale) }
                    Label {
                        id: nameLabel
                        textFormat: Text.PlainText
                        text: row.shell.vmName(row.vm) // the favorites group header already marks favorites
                        color: row.selected ? theme.colors.accent : theme.colors.foreground
                        font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale)
                        elide: Text.ElideRight; Layout.fillWidth: true
                    }
                    Label {
                        visible: !body.hovered && !menuButton.activeFocus
                        text: row.running && row.live && row.live.cpu !== null ? Math.round(row.live.cpu) + "%" : row.vm.owned ? row.shell.memoryLabel(row.vm.memoryMiB) : ""
                        color: row.running && row.live && row.live.cpu !== null ? theme.colors.foreground : theme.colors.muted
                        font.pixelSize: Math.round(11 * theme.textScale)
                    }
                    AppButton {
                        id: menuButton
                        objectName: "vmRowMenu"
                        visible: body.hovered || activeFocus
                        iconName: "more"; tone: "quiet"; hint: "VM actions · right-click"
                        implicitWidth: 24; implicitHeight: 22; leftPadding: 3; rightPadding: 3; topPadding: 2; bottomPadding: 2
                        onClicked: row.shell.openVmMenu(row.vm, menuButton, 0, menuButton.height)
                    }
                }
                RowLayout {
                    visible: !row.compact
                    Layout.fillWidth: true; spacing: 4
                    Label { visible: row.chips.length === 0; text: row.vm.state.toLowerCase(); color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale) }
                    Repeater {
                        model: row.chips
                        Label {
                            id: chip
                            required property var modelData
                            readonly property color ink: modelData.ink
                            text: modelData.text; color: ink
                            font.pixelSize: Math.round(9 * theme.textScale); font.letterSpacing: .5; font.weight: Font.DemiBold
                            leftPadding: 4; rightPadding: 4; topPadding: 1; bottomPadding: 1
                            Layout.maximumWidth: 90; elide: Text.ElideRight
                            background: Rectangle { color: "transparent"; radius: 2; border.color: Qt.rgba(chip.ink.r, chip.ink.g, chip.ink.b, .55) }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Spark {
                        readonly property var trend: row.stats ? row.stats.history[row.vm.uuid] : null
                        visible: !!trend && row.running
                        Layout.preferredWidth: 44; Layout.preferredHeight: 12
                        values: trend ? trend.cpu : []; capacity: row.stats ? row.stats.capacity : 60
                    }
                }
            }
        }
    }
}
