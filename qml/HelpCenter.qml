// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Help: the user guide and the networks guide, as searchable topics. The text comes straight from
// docs/, so the app and the guide on GitHub never disagree.
Popup {
    id: help
    objectName: "helpCenter"
    property var guide: null           // something with guide(name), the Workspace
    property string current: ""
    property string query: ""
    readonly property var topics: load()
    readonly property var shown: filter(topics, query)
    readonly property var topic: topics.find(function(t) { return t.id === help.current }) || topics[0] || null
    modal: true; focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(1040, (parent ? parent.width : 1040) - 60)
    height: Math.min(820, (parent ? parent.height : 820) - 60)
    padding: 0
    background: Rectangle { color: theme.colors.raised; border.color: theme.colors.border; radius: 8 }

    // Opens Help at a topic: a heading's id such as "snapshots", or "networks/containment".
    function show(id) { query = ""; search.text = ""; if (id) current = resolve(id); open() }
    function slug(title) { return title.toLowerCase().replace(/[^\w\s-]/g, "").trim().replace(/\s+/g, "-") }
    function resolve(id) {
        id = String(id || "")
        if (topics.some(function(t) { return t.id === id })) return id
        // A subsection's anchor opens the topic that contains it.
        const owner = topics.find(function(t) { return t.anchors.indexOf(id.split("/").pop()) >= 0 && (id.indexOf("/") < 0 || t.id.indexOf(id.split("/")[0] + "/") === 0) })
        return owner ? owner.id : current
    }
    // Each "## " heading of a guide is a topic; its "### " headings stay inside it.
    function load() {
        if (!guide) return []
        let out = []
        for (const source of [{file: "USER-GUIDE.md", prefix: "", group: "Using OmaWare"}, {file: "NETWORKS.md", prefix: "networks/", group: "Networks"}]) {
            const text = String(guide.guide(source.file) || "")
            let topic = null
            for (const line of text.split("\n")) {
                const h2 = /^## (.+)$/.exec(line), h3 = /^### (.+)$/.exec(line)
                if (h2) { topic = {id: source.prefix + slug(h2[1]), title: h2[1], group: source.group, prefix: source.prefix, body: [], anchors: [slug(h2[1])]}; out.push(topic); continue }
                if (!topic) continue
                if (h3) topic.anchors.push(slug(h3[1]))
                topic.body.push(line)
            }
        }
        for (const t of out) { t.text = t.body.join("\n").trim(); delete t.body }
        return out
    }
    function filter(list, q) {
        const words = q.toLowerCase().split(/\s+/).filter(function(w) { return w.length > 0 })
        if (!words.length) return list
        return list.filter(function(t) { const hay = (t.title + " " + t.text).toLowerCase(); return words.every(function(w) { return hay.indexOf(w) >= 0 }) })
    }
    // The line of a topic that first matches the search, to show under its title.
    function snippet(t) {
        const words = query.toLowerCase().split(/\s+/).filter(function(w) { return w.length > 0 })
        if (!words.length) return ""
        const line = t.text.split("\n").find(function(l) { return words.some(function(w) { return l.toLowerCase().indexOf(w) >= 0 }) }) || ""
        return line.replace(/[*`#>|]/g, "").replace(/\[([^\]]+)\]\([^)]+\)/g, "$1").replace(/^\s*-\s*/, "").trim()
    }
    // Links inside the guide: other topics open here, everything else in the browser.
    function follow(link) {
        const m = /^(?:(USER-GUIDE|NETWORKS)\.md)?#(.+)$/.exec(link)
        const file = /^(USER-GUIDE|NETWORKS)\.md$/.exec(link)
        if (m) { const prefix = m[1] === "NETWORKS" ? "networks/" : m[1] === "USER-GUIDE" ? "" : (topic ? topic.prefix : ""); current = resolve(prefix + m[2]); return }
        if (file) { const first = topics.find(function(t) { return t.prefix === (file[1] === "NETWORKS" ? "networks/" : "") }); if (first) current = first.id; return }
        if (/^https?:/.test(link)) { Qt.openUrlExternally(link); return }
        Qt.openUrlExternally("https://github.com/iop6/Omaware/blob/main/docs/" + link)
    }
    onCurrentChanged: reader.contentY = 0
    onOpened: search.forceActiveFocus()

    contentItem: RowLayout {
        spacing: 0
        // Topics and search.
        Rectangle {
            Layout.preferredWidth: 280; Layout.fillHeight: true
            color: theme.colors.sidebar; radius: 8
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: theme.colors.border }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 10
                RowLayout {
                    spacing: 8
                    AppIcon { name: "help"; width: 20; height: 20; color: theme.colors.accent }
                    Label { text: "Help"; font.pixelSize: Math.round(18 * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true }
                }
                AppField {
                    id: search; objectName: "helpSearch"
                    Layout.fillWidth: true; placeholderText: "Search help"
                    onTextChanged: help.query = text
                    Keys.onReturnPressed: if (help.shown.length) help.current = help.shown[0].id
                    Keys.onDownPressed: if (help.shown.length) { list.forceActiveFocus(); list.currentIndex = 0 }
                }
                ListView {
                    id: list; objectName: "helpTopics"
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: help.shown
                    spacing: 1
                    ScrollBar.vertical: AppScrollBar {}
                    Keys.onReturnPressed: if (currentIndex >= 0) help.current = help.shown[currentIndex].id
                    delegate: Item {
                        id: row
                        required property var modelData
                        required property int index
                        readonly property bool picked: help.topic && help.topic.id === modelData.id
                        readonly property string hit: help.snippet(modelData)
                        // The group's name above its first topic.
                        readonly property bool first: index === 0 || help.shown[index - 1].group !== modelData.group
                        width: ListView.view.width; height: (first ? groupLabel.implicitHeight : 0) + rowBox.height
                        Label { id: groupLabel; visible: row.first; text: row.modelData.group.toUpperCase(); color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale); font.weight: Font.Bold; font.letterSpacing: .6; topPadding: row.index === 0 ? 2 : 12; bottomPadding: 4 }
                        Rectangle {
                        id: rowBox
                        y: row.first ? groupLabel.implicitHeight : 0
                        width: parent.width; height: rowColumn.implicitHeight + 12; radius: 5
                        color: row.picked ? theme.colors.accentSoft : rowHover.hovered || row.ListView.isCurrentItem && list.activeFocus ? theme.colors.subtle : "transparent"
                        ColumnLayout {
                            id: rowColumn; anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 9; anchors.rightMargin: 9; spacing: 1
                            Label { text: row.modelData.title; color: row.picked ? theme.colors.accent : theme.colors.foreground; font.weight: row.picked ? Font.DemiBold : Font.Normal; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label { visible: row.hit !== ""; text: row.hit; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                        HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: help.current = row.modelData.id }
                        }
                    }
                }
                Label { visible: help.shown.length === 0; text: "Nothing matches “" + help.query + "”."; color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }
        // The topic.
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
            RowLayout {
                Layout.fillWidth: true; Layout.margins: 14; Layout.bottomMargin: 0
                Label { text: help.topic ? help.topic.group : ""; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true }
                AppButton { iconName: "close"; tone: "quiet"; implicitWidth: 30; implicitHeight: 30; leftPadding: 6; rightPadding: 6; hint: "Close · Esc"; onClicked: help.close() }
            }
            ScrollView {
                id: scroller
                Layout.fillWidth: true; Layout.fillHeight: true
                contentWidth: availableWidth; clip: true
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                Flickable {
                    id: reader
                    contentWidth: width; contentHeight: page.implicitHeight + 40
                    boundsBehavior: Flickable.StopAtBounds
                    TextEdit {
                        id: page; objectName: "helpPage"
                        x: 28; y: 4; width: reader.width - 56
                        readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                        textFormat: TextEdit.MarkdownText
                        text: help.topic ? "# " + help.topic.title + "\n\n" + help.topic.text : ""
                        color: theme.colors.foreground; selectionColor: theme.colors.accentSoft; selectedTextColor: theme.colors.foreground
                        font.pixelSize: Math.round(13 * theme.textScale)
                        onLinkActivated: function(link) { help.follow(link) }
                        HoverHandler { cursorShape: page.hoveredLink ? Qt.PointingHandCursor : Qt.IBeamCursor }
                    }
                }
            }
        }
    }
}
