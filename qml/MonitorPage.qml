// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// htop-style view of the host and every VM, fed by FleetStats.
FocusScope {
    id: page
    property var fleet: null
    property var domains: []
    property string selectedUuid: ""
    property var labelFor: function (vm) {
        return vm.name;
    }
    property int sampleInterval: 2000
    property string sortKey: "cpu"
    property bool descending: true
    signal select(string uuid)
    signal openConsole(string uuid)
    signal openDetails(string uuid)
    signal menuRequested(string uuid)

    FontMetrics {
        id: mono
        font.family: "monospace"
        font.pixelSize: Math.round(12 * theme.textScale)
    }
    readonly property real ch: mono.averageCharacterWidth
    readonly property bool showIo: list.width > ch * 104
    readonly property bool showSpark: list.width > ch * 86
    readonly property int meterCells: list.width > ch * 120 ? 16 : 10

    function mem(mib) {
        return mib === null || mib === undefined ? "--" : mib >= 1024 ? (mib / 1024).toFixed(1) + "G" : Math.round(mib) + "M";
    }
    function rate(b) {
        if (b === null || b === undefined)
            return "--";
        const units = ["B", "K", "M", "G"];
        let i = 0;
        while (b >= 1024 && i < units.length - 1) {
            b /= 1024;
            ++i;
        }
        return (i === 0 ? Math.round(b) : b < 10 ? b.toFixed(1) : Math.round(b)) + units[i];
    }
    function uptime(sec) {
        if (sec === undefined || sec === null || sec < 0)
            return "--";
        const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60);
        return d > 0 ? d + "d" + h + "h" : h > 0 ? h + "h" + (m < 10 ? "0" : "") + m + "m" : m + "m" + Math.floor(sec % 60) + "s";
    }
    function live(uuid) {
        return fleet && fleet.current[uuid] ? fleet.current[uuid] : null;
    }
    function sortValue(vm) {
        const l = live(vm.uuid);
        switch (sortKey) {
        case "name":
            return labelFor(vm).toLowerCase();
        case "cpu":
            return l && l.cpu !== null ? l.cpu : -1;
        case "mem":
            return l ? (l.memUsedMiB !== null ? l.memUsedMiB : l.rssMiB || 0) : -1;
        case "io":
            return l ? (l.rd || 0) + (l.wr || 0) : -1;
        case "net":
            return l ? (l.rx || 0) + (l.tx || 0) : -1;
        case "up":
            return l && l.uptime !== undefined ? l.uptime : -1;
        }
        return 0;
    }
    readonly property var rows: {
        const revision = fleet ? fleet.times.length : 0;
        return domains.slice().sort(function (a, b) {
            const activeA = a.stateCode === 1 || a.stateCode === 3, activeB = b.stateCode === 1 || b.stateCode === 3;
            if (activeA !== activeB)
                return activeA ? -1 : 1;
            const x = page.sortValue(a), y = page.sortValue(b);
            const order = x < y ? -1 : x > y ? 1 : 0;
            return page.descending ? -order : order;
        });
    }
    function sortBy(key) {
        if (sortKey === key)
            descending = !descending;
        else {
            sortKey = key;
            descending = key !== "name";
        }
    }
    function move(step) {
        if (rows.length === 0)
            return;
        const index = rows.findIndex(function (r) {
            return r.uuid === selectedUuid;
        });
        select(rows[Math.max(0, Math.min(rows.length - 1, index + step))].uuid);
    }
    onVisibleChanged: if (visible)
        list.forceActiveFocus()

    ColumnLayout {
        anchors.fill: parent
        spacing: 14
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: "// monitor"
                color: theme.colors.accent
                font.family: "monospace"
                font.pixelSize: Math.round(11 * theme.textScale)
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "System monitor"
                    font.pixelSize: Math.round(24 * theme.textScale)
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                }
                Rectangle {
                    width: 7
                    height: 7
                    color: page.fleet && page.fleet.ready ? theme.colors.success : theme.colors.muted
                    SequentialAnimation on opacity {
                        running: page.visible && !theme.reducedMotion
                        loops: Animation.Infinite
                        NumberAnimation {
                            to: .2
                            duration: 700
                        }
                        NumberAnimation {
                            to: 1
                            duration: 700
                        }
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    text: "sampling every " + page.sampleInterval / 1000 + " s"
                    color: theme.colors.muted
                    font.family: "monospace"
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
            }
        }
        // Host summary, htop header style.
        Rectangle {
            objectName: "monitorHost"
            Layout.fillWidth: true
            implicitHeight: hostGrid.implicitHeight + 24
            color: theme.colors.surface
            border.color: theme.colors.border
            radius: 4
            GridLayout {
                id: hostGrid
                anchors.fill: parent
                anchors.margins: 12
                columns: 3
                columnSpacing: 12
                rowSpacing: 6
                Label {
                    text: "cpu"
                    color: theme.colors.accent
                    font.family: "monospace"
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(12 * theme.textScale)
                }
                TextMeter {
                    cells: page.meterCells + 14
                    fraction: page.fleet && page.fleet.host.cpu !== null && page.fleet.host.cpu !== undefined ? page.fleet.host.cpu / 100 : -1
                    suffix: fraction < 0 ? "measuring…" : (fraction * 100).toFixed(1) + "%  · " + (page.fleet.host.cpus || "?") + " threads"
                }
                Spark {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 18
                    values: page.fleet ? page.fleet.hostCpu : []
                    capacity: page.fleet ? page.fleet.capacity : 60
                }
                Label {
                    text: "mem"
                    color: theme.colors.accent
                    font.family: "monospace"
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(12 * theme.textScale)
                }
                TextMeter {
                    cells: page.meterCells + 14
                    fraction: page.fleet && page.fleet.host.memUsedMiB !== null && page.fleet.host.memTotalMiB ? page.fleet.host.memUsedMiB / page.fleet.host.memTotalMiB : -1
                    suffix: fraction < 0 ? "--" : page.mem(page.fleet.host.memUsedMiB) + " / " + page.mem(page.fleet.host.memTotalMiB)
                }
                Spark {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 18
                    values: page.fleet ? page.fleet.hostMem : []
                    capacity: page.fleet ? page.fleet.capacity : 60
                    ceiling: page.fleet ? page.fleet.host.memTotalMiB || 1 : 1
                }
                Label {
                    text: "vms"
                    color: theme.colors.accent
                    font.family: "monospace"
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(12 * theme.textScale)
                }
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    readonly property var active: page.domains.filter(function (d) {
                        return d.stateCode === 1 || d.stateCode === 3;
                    })
                    readonly property int threads: active.reduce(function (a, d) {
                        return a + (d.cpus || 0);
                    }, 0)
                    readonly property real ram: active.reduce(function (a, d) {
                        return a + (d.memoryMiB || 0);
                    }, 0)
                    textFormat: Text.PlainText
                    text: active.length + " running / " + page.domains.length + " defined  ·  " + threads + " vCPU" + (page.fleet && page.fleet.host.cpus ? " on " + page.fleet.host.cpus + " threads" : "") + "  ·  " + page.mem(ram) + " assigned" + (page.fleet && page.fleet.host.memTotalMiB ? " (" + Math.round(ram / page.fleet.host.memTotalMiB * 100) + "% of host)" : "")
                    color: theme.colors.foreground
                    font.family: "monospace"
                    font.pixelSize: Math.round(12 * theme.textScale)
                }
            }
        }
        // Process table.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: theme.colors.surface
            border.color: theme.colors.border
            radius: 4
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    color: theme.colors.accentSoft
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        spacing: page.ch * 2
                        Repeater {
                            model: [
                                {
                                    key: "name",
                                    label: "NAME",
                                    width: 22,
                                    show: true
                                },
                                {
                                    key: "cpu",
                                    label: "CPU%",
                                    width: page.meterCells + 9 + (page.showSpark ? 9 : 0),
                                    show: true
                                },
                                {
                                    key: "mem",
                                    label: "MEM",
                                    width: page.meterCells + 9,
                                    show: true
                                },
                                {
                                    key: "io",
                                    label: "DISK r/w",
                                    width: 14,
                                    show: page.showIo
                                },
                                {
                                    key: "net",
                                    label: "NET rx/tx",
                                    width: 14,
                                    show: page.showIo
                                },
                                {
                                    key: "up",
                                    label: "UPTIME",
                                    width: 8,
                                    show: true
                                }
                            ]
                            Label {
                                required property var modelData
                                visible: modelData.show
                                width: page.ch * modelData.width + (modelData.key === "name" ? page.ch * 2 : 0)
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                textFormat: Text.PlainText
                                text: modelData.label + (page.sortKey === modelData.key ? (page.descending ? " ▾" : " ▴") : "")
                                color: page.sortKey === modelData.key ? theme.colors.accent : theme.colors.muted
                                font.family: "monospace"
                                font.weight: Font.DemiBold
                                font.pixelSize: Math.round(11 * theme.textScale)
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: page.sortBy(parent.modelData.key)
                                }
                            }
                        }
                    }
                }
                ListView {
                    id: list
                    objectName: "monitorList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    focus: true
                    model: page.rows
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: AppScrollBar {}
                    Keys.onPressed: function (event) {
                        if (event.key === Qt.Key_J || event.key === Qt.Key_Down) {
                            page.move(1);
                            event.accepted = true;
                        } else if (event.key === Qt.Key_K || event.key === Qt.Key_Up) {
                            page.move(-1);
                            event.accepted = true;
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            if (page.selectedUuid)
                                page.openConsole(page.selectedUuid);
                            event.accepted = true;
                        } else if (event.key === Qt.Key_D) {
                            if (page.selectedUuid)
                                page.openDetails(page.selectedUuid);
                            event.accepted = true;
                        }
                    }
                    delegate: Rectangle {
                        id: rowItem
                        required property var modelData
                        required property int index
                        readonly property var l: page.live(modelData.uuid)
                        readonly property bool active: modelData.stateCode === 1 || modelData.stateCode === 3
                        readonly property bool chosen: modelData.uuid === page.selectedUuid
                        width: ListView.view.width
                        height: 32
                        color: chosen ? theme.colors.accentSoft : rowHover.hovered ? theme.colors.subtle : index % 2 ? theme.colors.field : "transparent"
                        Rectangle {
                            visible: rowItem.chosen
                            width: 2
                            height: parent.height
                            color: theme.colors.accent
                        }
                        HoverHandler {
                            id: rowHover
                        }
                        TapHandler {
                            onTapped: {
                                page.select(rowItem.modelData.uuid);
                                list.forceActiveFocus();
                            }
                            onDoubleTapped: page.openConsole(rowItem.modelData.uuid)
                        }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: {
                                page.select(rowItem.modelData.uuid);
                                page.menuRequested(rowItem.modelData.uuid);
                            }
                        }
                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            spacing: page.ch * 2
                            opacity: rowItem.active ? 1 : .55
                            Row {
                                width: page.ch * 24
                                height: parent.height
                                spacing: page.ch
                                Label {
                                    textFormat: Text.PlainText
                                    text: rowItem.modelData.stateCode === 1 ? "●" : rowItem.modelData.stateCode === 3 ? "‖" : "○"
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: rowItem.modelData.stateCode === 1 ? theme.colors.success : rowItem.modelData.stateCode === 3 ? theme.colors.warning : theme.colors.muted
                                    font.family: "monospace"
                                    font.pixelSize: Math.round(12 * theme.textScale)
                                }
                                Label {
                                    width: page.ch * 22
                                    anchors.verticalCenter: parent.verticalCenter
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    text: page.labelFor(rowItem.modelData)
                                    color: rowItem.chosen ? theme.colors.accent : theme.colors.foreground
                                    font.family: "monospace"
                                    font.weight: rowItem.chosen ? Font.DemiBold : Font.Normal
                                    font.pixelSize: Math.round(12 * theme.textScale)
                                }
                            }
                            Row {
                                width: page.ch * (page.meterCells + 9 + (page.showSpark ? 9 : 0))
                                height: parent.height
                                spacing: page.ch
                                TextMeter {
                                    anchors.verticalCenter: parent.verticalCenter
                                    cells: page.meterCells
                                    fraction: rowItem.l && rowItem.l.cpu !== null ? rowItem.l.cpu / 100 : -1
                                    suffix: fraction < 0 ? (rowItem.active ? "  ..." : "   --") : ("     " + (fraction * 100).toFixed(1)).slice(-5)
                                }
                                Spark {
                                    visible: page.showSpark && rowItem.active
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: page.ch * 8
                                    height: 14
                                    values: page.fleet && page.fleet.history[rowItem.modelData.uuid] ? page.fleet.history[rowItem.modelData.uuid].cpu : []
                                    capacity: page.fleet ? page.fleet.capacity : 60
                                }
                            }
                            TextMeter {
                                width: page.ch * (page.meterCells + 9)
                                anchors.verticalCenter: parent.verticalCenter
                                cells: page.meterCells
                                readonly property real used: rowItem.l ? (rowItem.l.memUsedMiB !== null ? rowItem.l.memUsedMiB : rowItem.l.rssMiB || 0) : 0
                                fraction: rowItem.l && used > 0 ? used / Math.max(1, rowItem.l.memTotalMiB || rowItem.modelData.memoryMiB) : -1
                                suffix: fraction < 0 ? "   --" : ("      " + page.mem(used)).slice(-6)
                            }
                            Label {
                                visible: page.showIo
                                width: page.ch * 14
                                anchors.verticalCenter: parent.verticalCenter
                                textFormat: Text.PlainText
                                text: rowItem.l ? page.rate(rowItem.l.rd) + "/" + page.rate(rowItem.l.wr) : "--"
                                color: theme.colors.foreground
                                font.family: "monospace"
                                font.pixelSize: Math.round(12 * theme.textScale)
                            }
                            Label {
                                visible: page.showIo
                                width: page.ch * 14
                                anchors.verticalCenter: parent.verticalCenter
                                textFormat: Text.PlainText
                                text: rowItem.l ? page.rate(rowItem.l.rx) + "/" + page.rate(rowItem.l.tx) : "--"
                                color: theme.colors.foreground
                                font.family: "monospace"
                                font.pixelSize: Math.round(12 * theme.textScale)
                            }
                            Label {
                                width: page.ch * 8
                                anchors.verticalCenter: parent.verticalCenter
                                textFormat: Text.PlainText
                                text: rowItem.l ? page.uptime(rowItem.l.uptime) : "--"
                                color: theme.colors.muted
                                font.family: "monospace"
                                font.pixelSize: Math.round(12 * theme.textScale)
                            }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: list.count === 0
                        text: "no virtual machines defined"
                        color: theme.colors.muted
                        font.family: "monospace"
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: theme.colors.border
                }
                Label {
                    Layout.fillWidth: true
                    Layout.margins: 8
                    Layout.leftMargin: 12
                    text: "j/k move  ·  enter console  ·  d details  ·  click a column to sort  ·  double-click to open"
                    color: theme.colors.muted
                    elide: Text.ElideRight
                    font.family: "monospace"
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
            }
        }
    }
}
