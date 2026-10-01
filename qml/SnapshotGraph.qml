// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: graph
    property var snapshots: []
    property string selectedId: "__working__"
    property string currentId: ""
    property string vmState: ""
    property bool fit: false
    property string renamingId: ""
    property string renameDraft: ""
    property bool editable: false
    readonly property real unit: theme.textScale
    readonly property real cardWidth: 230 * unit
    readonly property real cardHeight: 96 * unit
    readonly property real zoom: fit ? Math.max(0.20, Math.min(1, (width - 12) / layout.width, (height - 12) / layout.height)) : 1
    signal picked(string snapshotId)
    signal previous()
    signal next()
    signal deleteRequested()
    signal renameRequested(string snapshotId)
    signal renameSubmitted(string snapshotId, string name)
    signal previewRequested(var snapshot)
    signal contextRequested(string snapshotId, var anchor, real x, real y)
    radius: 4; color: theme.colors.background; border.color: theme.colors.border; clip: true
    Keys.onLeftPressed: previous()
    Keys.onRightPressed: next()
    Keys.onDeletePressed: if (!renamingId) deleteRequested()
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_F2 && editable) { renameRequested(selectedId); event.accepted = true }
        else if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            const index = layout.nodes.findIndex(function(n) { return n.id === selectedId }), card = cards.itemAt(index)
            if (card) contextRequested(selectedId, card, card.width / 2, card.height / 2)
            event.accepted = true
        }
    }
    function sizeLabel(snapshot) { return snapshot.kind !== "copy" ? "Size unavailable" : snapshot.bytes >= 1073741824 ? (snapshot.bytes / 1073741824).toFixed(1) + " GiB stored" : (Number(snapshot.bytes || 0) / 1048576).toFixed(1) + " MiB stored" }
    function beginRename(id, name) { fit = false; renamingId = id; renameDraft = name; Qt.callLater(focusSelection) }
    readonly property var layout: {
        const nodes = snapshots.map(function(s) { return {id: s.id || "internal:" + s.name, snapshot: s, parentId: s.parentId || "", working: false, onPath: false, children: []} })
        nodes.push({id: "__working__", snapshot: {name: "You are here"}, parentId: currentId, working: true, onPath: true, children: []})
        const byId = {}, roots = [], visible = []
        nodes.forEach(function(n) { byId[n.id] = n })
        nodes.forEach(function(n) {
            let parent = byId[n.parentId], cursor = parent
            const seen = {}; seen[n.id] = true
            while (cursor) { if (seen[cursor.id]) { parent = null; break }; seen[cursor.id] = true; cursor = byId[cursor.parentId] }
            if (parent) parent.children.push(n)
            else { n.parentId = ""; roots.push(n) }
        })
        let cursor = byId[currentId], path = {}
        while (cursor && !path[cursor.id]) { path[cursor.id] = true; cursor.onPath = true; cursor = byId[cursor.parentId] }
        let row = 0, color = 0, maxDepth = 0
        function place(node, depth, branch) {
            node.branch = branch; node.x = 24 + depth * (graph.cardWidth + 64); maxDepth = Math.max(maxDepth, depth)
            visible.push(node)
            if (node.children.length) {
                // Align the current path so the working state and parent stay clear.
                node.children.forEach(function(child, index) { place(child, depth + 1, child.working || index === 0 ? branch : ++color) })
                const pathChild = node.children.find(function(child) { return child.onPath })
                node.y = pathChild ? pathChild.y : node.children[0].y
            } else node.y = 24 + row++ * (graph.cardHeight + 30)
        }
        roots.forEach(function(root, index) { place(root, 0, index === 0 ? 0 : ++color) })
        return {nodes: visible, width: 48 + graph.cardWidth + maxDepth * (graph.cardWidth + 64), height: Math.max(144, row * (graph.cardHeight + 30) + 24)}
    }
    function focusSelection() {
        const node = layout.nodes.find(function(n) { return n.id === selectedId })
        if (!node) return
        const parent = node.working ? layout.nodes.find(function(n) { return n.id === node.parentId }) : null
        const left = parent && (node.x + cardWidth - parent.x + 24) * zoom <= viewport.width ? parent.x : node.x
        if (left * zoom < viewport.contentX + 8) viewport.contentX = Math.max(0, (left - 12) * zoom)
        if ((node.x + cardWidth + 12) * zoom > viewport.contentX + viewport.width) viewport.contentX = Math.max(0, (node.x + cardWidth + 12) * zoom - viewport.width)
        if (node.y * zoom < viewport.contentY + 8) viewport.contentY = Math.max(0, (node.y - 16) * zoom)
        if ((node.y + cardHeight + 10) * zoom > viewport.contentY + viewport.height) viewport.contentY = Math.max(0, (node.y + cardHeight + 10) * zoom - viewport.height)
        viewport.returnToBounds()
    }
    onSelectedIdChanged: { renamingId = ""; Qt.callLater(focusSelection); lines.requestPaint() }
    onCurrentIdChanged: Qt.callLater(focusSelection)
    Component.onCompleted: Qt.callLater(focusSelection)
    onLayoutChanged: lines.requestPaint()
    onZoomChanged: { lines.requestPaint(); Qt.callLater(focusSelection) }
    onVisibleChanged: {
        if (!visible) { hoverPreview.close(); renamingId = "" }
        else Qt.callLater(function() { lines.requestPaint(); graph.focusSelection() })
    }
    Connections { target: theme; function onChanged() { lines.requestPaint() } }
    Canvas {
        id: lines; anchors.fill: parent
        onAvailableChanged: if (available) requestPaint()
        onPaint: {
            const ctx = getContext("2d"); ctx.reset(); ctx.fillStyle = theme.colors.border
            for (let x = 12 - viewport.contentX % 24; x < width; x += 24) for (let y = 12 - viewport.contentY % 24; y < height; y += 24) ctx.fillRect(x, y, 1, 1)
            ctx.translate(-viewport.contentX, -viewport.contentY); ctx.scale(graph.zoom, graph.zoom)
            const byId = {}; graph.layout.nodes.forEach(function(n) { byId[n.id] = n })
            graph.layout.nodes.forEach(function(n) {
                const parent = byId[n.parentId]; if (!parent) return
                const x1 = parent.x + graph.cardWidth, y1 = parent.y + graph.cardHeight / 2, x2 = n.x, y2 = n.y + graph.cardHeight / 2
                ctx.strokeStyle = n.onPath ? theme.colors.accent : theme.colors.muted
                ctx.lineCap = n.working ? "round" : "butt"
                ctx.setLineDash(n.working ? [1, 7] : [])
                const siblings = parent.children.length, index = parent.children.indexOf(n)
                const turn = x1 + 16 + 32 * (index + 1) / (siblings + 1)
                function edge() { ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(turn, y1); ctx.lineTo(turn, y2); ctx.lineTo(x2, y2); ctx.stroke() }
                ctx.globalAlpha = n.onPath ? 1 : 0.5; ctx.lineWidth = n.working ? 3 : 2; edge()
            })
        }
    }
    Flickable {
        id: viewport; objectName: "branchMapViewport"; anchors.fill: parent; anchors.margins: 1
        contentWidth: Math.max(width, graph.layout.width * graph.zoom); contentHeight: Math.max(height, graph.layout.height * graph.zoom)
        boundsBehavior: Flickable.StopAtBounds; clip: true
        onContentXChanged: { lines.requestPaint(); hoverPreview.close() }
        onContentYChanged: { lines.requestPaint(); hoverPreview.close() }
        ScrollBar.horizontal: AppScrollBar {}
        ScrollBar.vertical: AppScrollBar {}
        Item {
            width: graph.layout.width; height: graph.layout.height; scale: graph.zoom; transformOrigin: Item.TopLeft
            Repeater {
                id: cards
                model: graph.layout.nodes
                Button {
                    id: node
                    required property var modelData
                    objectName: "snapshotNode_" + modelData.id
                    readonly property bool chosen: modelData.id === graph.selectedId
                    readonly property bool currentSaved: !modelData.working && modelData.id === graph.currentId
                    readonly property color ink: currentSaved || modelData.working ? theme.colors.accent : theme.colors.muted
                    x: modelData.x; y: modelData.y; width: graph.cardWidth; height: graph.cardHeight; padding: 0; hoverEnabled: true
                    Accessible.name: modelData.working ? "You are here, unsaved VM state" : modelData.snapshot.name + ", " + graph.sizeLabel(modelData.snapshot) + (currentSaved ? ", current snapshot" : "")
                    Accessible.description: "Right-click or press Shift+F10 for actions. F2 renames a saved snapshot."
                    background: Rectangle {
                        radius: node.modelData.working ? 22 : 10
                        color: node.modelData.working ? theme.colors.accentSoft : node.chosen ? theme.colors.raised : theme.colors.surface
                        border.color: node.currentSaved || node.modelData.working ? theme.colors.accent : node.chosen || node.hovered || node.visualFocus ? theme.colors.foreground : theme.colors.border
                        border.width: node.currentSaved || node.modelData.working || node.visualFocus ? 2 : 1
                    }
                    contentItem: Item {
                        Rectangle { visible: node.currentSaved; x: 0; y: 12; width: 3; height: parent.height - 24; color: node.ink }
                        Rectangle { x: -4; y: parent.height / 2 - 4; width: 8; height: 8; radius: 4; color: node.ink }
                        Rectangle {
                            x: 12; y: 14; width: 42 * graph.unit; height: 44 * graph.unit; radius: 4; color: theme.colors.background; clip: true
                            Image { anchors.fill: parent; source: node.modelData.snapshot.previewUrl || ""; sourceSize.width: 120; sourceSize.height: 90; fillMode: Image.PreserveAspectFit }
                            AppIcon { anchors.centerIn: parent; visible: !node.modelData.snapshot.previewUrl; name: node.modelData.working ? "console" : "snapshot"; color: node.ink; width: 20; height: 20 }
                        }
                        Column {
                            x: 64 * graph.unit; y: 12; width: parent.width - x - 12; spacing: 4
                            Label { width: parent.width; visible: graph.renamingId !== node.modelData.id; text: node.modelData.snapshot.name; elide: Text.ElideRight; font.pixelSize: Math.round((12) * theme.textScale); font.weight: Font.DemiBold }
                            Label { width: parent.width; text: node.modelData.working ? "Not saved yet" : graph.sizeLabel(node.modelData.snapshot); font.pixelSize: Math.round((11) * theme.textScale); color: node.modelData.working ? theme.colors.accent : theme.colors.muted; elide: Text.ElideRight }
                            Label { width: parent.width; text: node.modelData.working ? graph.vmState || "Current VM state" : (node.modelData.snapshot.memory ? "Memory · " : "Disk · ") + Qt.formatDateTime(new Date(node.modelData.snapshot.time * 1000), "MMM d · HH:mm"); font.pixelSize: Math.round((10) * theme.textScale); color: theme.colors.muted; elide: Text.ElideRight }
                        }
                        Label { objectName: "snapshotState_" + node.modelData.id; x: 12; y: parent.height - height - 9; text: node.modelData.working ? "WORKING VM" : node.currentSaved ? "CURRENT SNAPSHOT" : node.modelData.snapshot.healthy === false ? "! Needs attention" : node.modelData.snapshot.safety ? "RECOVERY POINT" : ""; font.pixelSize: Math.round((9) * theme.textScale); font.weight: Font.DemiBold; font.letterSpacing: 0.5; color: node.ink }
                        AppField {
                            id: renameField; objectName: "renameSnapshot_" + node.modelData.id
                            visible: graph.renamingId === node.modelData.id
                            x: 5; y: 6; width: parent.width - 10; implicitHeight: 38
                            text: graph.renameDraft; enabled: graph.editable; Accessible.name: "Snapshot name. Enter saves, Escape cancels."
                            onTextEdited: graph.renameDraft = text
                            onVisibleChanged: if (visible) Qt.callLater(function() { forceActiveFocus(); selectAll() })
                            onAccepted: graph.renameSubmitted(node.modelData.id, text)
                            Keys.onEscapePressed: { graph.renamingId = ""; graph.forceActiveFocus() }
                        }
                    }
                    Timer { id: previewDelay; interval: 550; onTriggered: if (node.hovered && !node.modelData.working && !graph.renamingId) hoverPreview.showFor(node, node.modelData.snapshot) }
                    onHoveredChanged: { if (hovered && modelData.snapshot.previewUrl) previewDelay.restart(); else { previewDelay.stop(); hoverPreview.close() } }
                    onClicked: { hoverPreview.close(); graph.picked(modelData.id); graph.forceActiveFocus() }
                    onDoubleClicked: if (graph.editable && modelData.snapshot.kind === "copy") graph.renameRequested(modelData.id)
                    MouseArea {
                        anchors.fill: parent; acceptedButtons: Qt.RightButton
                        onClicked: function(mouse) { previewDelay.stop(); hoverPreview.close(); graph.forceActiveFocus(); graph.contextRequested(node.modelData.id, node, mouse.x, mouse.y) }
                    }
                }
            }
        }
    }
    Popup {
        id: hoverPreview; objectName: "snapshotHoverPreview"; parent: Overlay.overlay
        property var snapshot: ({})
        width: 320; padding: 12; modal: false; focus: false; closePolicy: Popup.NoAutoClose
        function showFor(anchor, data) {
            snapshot = data; const point = anchor.mapToItem(parent, 0, 0)
            x = Math.max(8, Math.min(parent.width - width - 8, point.x))
            y = Math.max(8, point.y - implicitHeight - 8); open()
        }
        background: Rectangle { radius: 4; color: theme.colors.raised; border.color: theme.colors.accent }
        contentItem: ColumnLayout { spacing: 8
            Image { Layout.fillWidth: true; Layout.preferredHeight: 166; source: hoverPreview.snapshot.previewUrl || ""; fillMode: Image.PreserveAspectFit }
            Label { text: hoverPreview.snapshot.name || ""; Layout.fillWidth: true; elide: Text.ElideRight; font.weight: Font.DemiBold }
            Label { text: graph.sizeLabel(hoverPreview.snapshot); color: theme.colors.muted }
        }
    }
}
