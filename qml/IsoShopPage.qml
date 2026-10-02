// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// The ISO Shop: free, official installation images, downloaded into OmaWare's ISO folder and
// verified against each publisher's checksum. Your ISOs are listed underneath.
ColumnLayout {
    id: page
    objectName: "isoShop"
    property var library: null
    property string category: "all"
    property string query: ""
    signal useIso(string path)
    signal useAppliance(string path)
    function useMedia(file) { if (file.type === "disk") useAppliance(file.path); else useIso(file.path) }
    spacing: 14

    readonly property var categories: [
        {key: "all", label: "All"}, {key: "desktop", label: "Desktop"}, {key: "server", label: "Server"},
        {key: "security", label: "Security & networking"}, {key: "windows", label: "Windows"}, {key: "mine", label: "Your media"}]
    // Names and categories never change, so filtering only rebuilds the grid when the filter does.
    property var catalogue: ({})
    function remember() {
        let map = {}
        for (const s of (library ? library.sources : [])) map[s.id] = {name: s.name, category: s.category, description: s.description}
        catalogue = map
    }
    readonly property var shown: {
        const ids = library ? library.sourceIds : [], q = query.trim().toLowerCase(), info = catalogue
        return ids.filter(function(id) {
            const s = info[id] || {}
            return (category === "all" || s.category === category) && (q === "" || (s.name + " " + s.description).toLowerCase().indexOf(q) >= 0)
        })
    }
    function infoFor(id) {
        for (const s of (library ? library.sources : [])) if (s.id === id) return s
        return ({})
    }
    function size(bytes) {
        const b = Number(bytes) || 0
        return b >= 1073741824 ? (b / 1073741824).toFixed(1) + " GB" : b >= 1048576 ? Math.round(b / 1048576) + " MB" : Math.round(b / 1024) + " KB"
    }
    // ---- Deleting ----
    property var picked: ({})              // file name -> true, for "Delete selected"
    property string confirming: ""         // "older" or "selected" while asking for confirmation
    readonly property var pickedNames: (library ? library.files : []).map(function(f) { return f.name }).filter(function(n) { return !!page.picked[n] })
    readonly property var olderNames: (library ? library.files : []).filter(function(f) { return !f.newest }).map(function(f) { return f.name })
    function filesOf(source) { return (library ? library.files : []).filter(function(f) { return f.source === source }) }
    function bytesOf(names) {
        let total = 0
        for (const f of (library ? library.files : [])) if (names.indexOf(f.name) >= 0) total += Number(f.size) || 0
        return total
    }
    // Remembered, so the shop offers the same Windows language next time.
    signal languageChosen(string id, string language)
    function chooseLanguage(id, language) { library.setLanguage(id, language); languageChosen(id, language) }
    function togglePick(name) { let next = Object.assign({}, picked); if (next[name]) delete next[name]; else next[name] = true; picked = next }
    function deleteNames(names) {
        library.removeAll(names)
        let next = Object.assign({}, picked)
        for (const n of names) delete next[n]
        picked = next; confirming = ""
    }
    function badge(name) {
        const words = String(name).replace(/[^A-Za-z0-9 ]/g, " ").split(" ").filter(function(w) { return w !== "" })
        return words.length > 1 && words[1].length > 1 && /^[A-Z]/.test(words[1]) ? words[0][0] + words[1][0] : String(words[0] || "?").slice(0, 2)
    }
    onVisibleChanged: if (visible && library) { remember(); library.rescan(); if (library.autoCheck && !library.checking) library.check() }
    Component.onCompleted: remember()
    FileDialog { id: mediaPicker; title: "Add installer or appliance media"; fileMode: FileDialog.OpenFiles; nameFilters: ["ISO and appliances (*.iso *.ISO *.ova *.OVA *.qcow2 *.QCOW2)"]; onAccepted: page.library.importFiles(selectedFiles) }

    // ---- Header ----
    RowLayout {
        Layout.fillWidth: true; spacing: 10
        ColumnLayout {
            Layout.fillWidth: true; spacing: 4
            Label { text: "ISO Shop"; font.pixelSize: Math.round(28 * theme.textScale); font.weight: Font.DemiBold }
            Label {
                text: "Official installer images and appliance links. Automatic ISO downloads are checksum-verified. Add your own ISO, OVA or QCOW2 with Add files or drag and drop; verify appliances with their publisher first."
                color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap
            }
        }
        AppButton { objectName: "isoCheck"; text: page.library && page.library.checking ? "Checking…" : "Check for updates"; iconName: "refresh"; enabled: !!page.library && !page.library.checking; onClicked: page.library.check() }
        AppButton { objectName: "addMedia"; text: "Add files…"; enabled: !!page.library && !page.library.importing; onClicked: mediaPicker.open() }
        AppButton { objectName: "isoOpenFolder"; text: "Open folder"; iconName: "folder"; hint: page.library ? page.library.folder : ""; onClicked: Qt.openUrlExternally("file://" + page.library.folder) }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 6
        Repeater {
            model: page.categories
            AppButton {
                required property var modelData
                objectName: "isoCategory_" + modelData.key
                text: modelData.label + (modelData.key === "mine" && page.library ? " " + page.library.files.length : "")
                tone: "quiet"; checked: page.category === modelData.key
                ink: checked ? theme.colors.accent : theme.colors.muted
                implicitHeight: 32
                onClicked: page.category = modelData.key
            }
        }
        Item { Layout.fillWidth: true }
        AppField {
            id: search
            objectName: "isoSearch"
            visible: page.category !== "mine"
            Layout.preferredWidth: 220; implicitHeight: 34; leftPadding: 30
            placeholderText: "Search systems…"
            text: page.query; onTextChanged: page.query = text
            AppIcon { x: 9; anchors.verticalCenter: parent.verticalCenter; width: 14; height: 14; name: "search"; color: theme.colors.muted }
        }
    }

    ScrollView {
        id: scroller
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: scroller.availableWidth
            spacing: 14
            // ---- Catalogue ----
            Flow {
                id: grid
                visible: page.category !== "mine"
                Layout.fillWidth: true
                spacing: 12
                readonly property int columns: width >= 1050 ? 3 : width >= 640 ? 2 : 1
                readonly property real cardWidth: Math.floor((width - spacing * (columns - 1)) / columns)
                Repeater {
                    model: page.shown
                    Rectangle {
                        id: card
                        required property string modelData
                        readonly property var info: page.infoFor(modelData)
                        readonly property bool busy: info.status === "downloading" || info.status === "unpacking" || info.status === "starting"
                        // Languages change rarely; keep the list steady while progress updates arrive.
                        property var languages: []
                        onInfoChanged: if (JSON.stringify(info.languages || []) !== JSON.stringify(languages)) languages = info.languages || []
                        readonly property color brand: info.color || theme.colors.accent
                        property bool removing: false
                        objectName: "isoSource_" + modelData
                        width: grid.cardWidth; height: cardColumn.implicitHeight + 28
                        radius: 12; color: theme.colors.surface
                        border.color: cardHover.hovered ? Qt.rgba(card.brand.r, card.brand.g, card.brand.b, .7) : info.updateAvailable ? theme.colors.warning : theme.colors.border
                        Behavior on border.color { enabled: !theme.reducedMotion; ColorAnimation { duration: 120 } }
                        HoverHandler { id: cardHover }
                        ColumnLayout {
                            id: cardColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
                            spacing: 9
                            RowLayout {
                                Layout.fillWidth: true; spacing: 12
                                // A colored badge with the system's initials (no trademarked logos).
                                Rectangle {
                                    Layout.preferredWidth: 46; Layout.preferredHeight: 46; radius: 12
                                    color: card.brand
                                    Label { anchors.centerIn: parent; text: page.badge(card.info.name || ""); color: "#ffffff"; font.weight: Font.Bold; font.pixelSize: Math.round(16 * theme.textScale) }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 2
                                    Label { textFormat: Text.PlainText; text: card.info.name || ""; font.weight: Font.DemiBold; font.pixelSize: Math.round(15 * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true }
                                    Label {
                                        objectName: "isoStatus_" + card.modelData
                                        Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: Math.round(11 * theme.textScale)
                                        color: card.info.error ? theme.colors.danger : card.info.updateAvailable ? theme.colors.warning : card.info.upToDate ? theme.colors.success : theme.colors.muted
                                        text: card.info.error ? "Couldn't check"
                                            : card.info.status === "unpacking" ? "Unpacking…"
                                            : card.info.status === "starting" ? "Asking " + (card.modelData === "windows-11" ? "Microsoft" : "the publisher") + " for a download link…"
                                            : card.busy ? "Downloading…"
                                            : card.info.kind === "page" ? (card.info.have ? "In your library: " + card.info.have : "From the publisher's website")
                                            : card.info.status === "checking" ? "Checking…"
                                            : card.info.upToDate ? "✓ Up to date · " + card.info.version
                                            : card.info.updateAvailable ? "Update: " + card.info.version + " (you have " + card.info.have + ")"
                                            : card.info.version ? card.info.version + (card.info.size ? " · " + page.size(card.info.size) : "")
                                            : card.info.have ? "In your library: " + card.info.have
                                            : "Not checked yet"
                                    }
                                }
                            }
                            Label {
                                text: card.info.description || ""; color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale)
                                Layout.fillWidth: true; wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
                                Layout.preferredHeight: Math.ceil(3 * (font.pixelSize * 1.35))
                            }
                            RowLayout {
                                visible: card.languages.length > 0
                                Layout.fillWidth: true; spacing: 8
                                Label { text: "Language"; color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale) }
                                AppSelect {
                                    objectName: "isoLanguage_" + card.modelData
                                    Accessible.name: "Language"
                                    Layout.fillWidth: true; implicitHeight: 32
                                    enabled: !card.busy
                                    model: card.languages
                                    currentIndex: Math.max(0, card.languages.indexOf(card.info.language))
                                    onActivated: function(index) { page.chooseLanguage(card.modelData, card.languages[index]) }
                                }
                            }
                            Label { visible: !!card.info.note; text: card.info.note || ""; color: theme.colors.warning; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            Label { textFormat: Text.PlainText; visible: !!card.info.error; text: card.info.error || ""; color: theme.colors.danger; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            ColumnLayout {
                                visible: card.busy
                                Layout.fillWidth: true; spacing: 4
                                AppProgressBar { Layout.fillWidth: true; from: 0; to: Math.max(1, card.info.total || card.info.size || 1); value: card.info.received || 0; indeterminate: card.info.status === "unpacking" || card.info.status === "starting" || !(card.info.total || card.info.size) }
                                Label {
                                    text: card.info.status === "starting" ? "This takes a few seconds."
                                        : card.info.status === "unpacking" ? "Download verified, unpacking the image…"
                                        : page.size(card.info.received) + (card.info.total ? " of " + page.size(card.info.total) : "") + (card.info.rate ? " · " + page.size(card.info.rate) + "/s" : "")
                                          + (card.info.rate && card.info.total ? " · about " + Math.max(1, Math.round((card.info.total - card.info.received) / card.info.rate / 60)) + " min left" : "")
                                    color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale)
                                }
                            }
                            // Deleting this system's ISOs, confirmed in place.
                            RowLayout {
                                visible: card.removing
                                Layout.fillWidth: true; spacing: 6
                                Label {
                                    readonly property var mine: page.filesOf(card.modelData)
                                    text: "Delete " + (mine.length === 1 ? "this ISO" : mine.length + " ISOs") + " (" + page.size(page.bytesOf(mine.map(function(f) { return f.name }))) + ")?"
                                    color: theme.colors.danger; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: Math.round(12 * theme.textScale)
                                }
                                AppButton { objectName: "isoDeleteConfirm_" + card.modelData; text: "Delete"; tone: "danger"; onClicked: { card.removing = false; page.deleteNames(page.filesOf(card.modelData).map(function(f) { return f.name })) } }
                                AppButton { text: "Keep"; tone: "quiet"; onClicked: card.removing = false }
                            }
                            RowLayout {
                                visible: !card.removing
                                Layout.fillWidth: true; spacing: 6
                                AppButton {
                                    objectName: "isoDelete_" + card.modelData
                                    visible: !!card.info.have && !card.busy
                                    iconName: "trash"; tone: "quiet"
                                    hint: "Delete your " + (card.info.name || "") + " ISO" + (page.filesOf(card.modelData).length > 1 ? "s" : "")
                                    onClicked: card.removing = true
                                }
                                Item { Layout.fillWidth: true }
                                AppButton {
                                    visible: (card.info.upToDate || (card.info.kind === "page" && !!card.info.have))
                                    text: "New VM"; tone: "quiet"; iconName: "plus"
                                    hint: "Create a VM from this ISO"
                                    onClicked: { for (const f of page.library.files) if (f.source === card.modelData && f.newest) { page.useMedia(f); break } }
                                }
                                AppButton {
                                    // When a publisher refuses an automated download, its website still works.
                                    visible: !card.busy && card.info.kind !== "page" && !!card.info.page && !!card.info.error
                                    text: "Website"; iconName: "globe"; tone: "quiet"; hint: card.info.page
                                    onClicked: Qt.openUrlExternally(card.info.page)
                                }
                                AppButton { visible: card.busy && card.info.status !== "starting"; text: "Cancel"; tone: "quiet"; onClicked: page.library.cancel(card.modelData) }
                                AppButton {
                                    objectName: "isoGet_" + card.modelData
                                    visible: !card.busy && !card.info.upToDate
                                    text: card.info.kind === "page" ? "Get from website" : card.info.updateAvailable ? "Update" : "Download"
                                    iconName: card.info.kind === "page" ? "globe" : "next"
                                    tone: "primary"
                                    enabled: card.info.kind === "page" || (card.info.status === "ready" && !!card.info.version)
                                    hint: card.info.kind === "page" ? card.info.page : card.info.file || ""
                                    onClicked: card.info.kind === "page" ? Qt.openUrlExternally(card.info.page) : page.library.download(card.modelData)
                                }
                            }
                        }
                    }
                }
            }
            Label { visible: page.category !== "mine" && page.shown.length === 0; text: "Nothing matches “" + page.query + "”."; color: theme.colors.muted }

            // ---- Your ISOs ----
            ColumnLayout {
                Layout.fillWidth: true; spacing: 8
                visible: page.category === "mine" || page.category === "all"
                RowLayout {
                    Layout.fillWidth: true; Layout.topMargin: 6; spacing: 8
                    Label { text: "Your ISOs"; font.weight: Font.DemiBold; font.pixelSize: Math.round(16 * theme.textScale) }
                    Label {
                        objectName: "isoTotals"
                        visible: !!page.library && page.library.files.length > 0
                        text: page.library ? page.library.files.length + (page.library.files.length === 1 ? " file · " : " files · ") + page.size(page.bytesOf(page.library.files.map(function(f) { return f.name }))) : ""
                        color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale); Layout.fillWidth: true
                    }
                    AppButton {
                        objectName: "isoDeleteOlder"
                        visible: page.olderNames.length > 0 && page.confirming === ""
                        text: "Delete older versions (" + page.olderNames.length + " · " + page.size(page.bytesOf(page.olderNames)) + ")"
                        iconName: "trash"; tone: "quiet"
                        hint: "Remove every ISO that a newer download has replaced"
                        onClicked: page.confirming = "older"
                    }
                    AppButton {
                        objectName: "isoSelectAll"
                        visible: !!page.library && page.library.files.length > 1 && page.confirming === ""
                        text: page.pickedNames.length === page.library.files.length ? "Clear selection" : "Select all"; tone: "quiet"
                        onClicked: {
                            let next = {}
                            if (page.pickedNames.length !== page.library.files.length) for (const f of page.library.files) next[f.name] = true
                            page.picked = next
                        }
                    }
                    AppButton {
                        objectName: "isoDeleteSelected"
                        visible: page.pickedNames.length > 0 && page.confirming === ""
                        text: "Delete selected (" + page.pickedNames.length + " · " + page.size(page.bytesOf(page.pickedNames)) + ")"
                        iconName: "trash"; tone: "danger"
                        onClicked: page.confirming = "selected"
                    }
                }
                // Confirmation for the bulk actions, in place.
                Rectangle {
                    id: bulkConfirm
                    visible: page.confirming !== ""
                    readonly property var names: page.confirming === "older" ? page.olderNames : page.pickedNames
                    Layout.fillWidth: true; implicitHeight: confirmRow.implicitHeight + 16; radius: 10
                    color: "transparent"; border.color: theme.colors.danger
                    RowLayout {
                        id: confirmRow; anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                        Label {
                            text: "Delete " + bulkConfirm.names.length + (bulkConfirm.names.length === 1 ? " ISO" : " ISOs") + " and free " + page.size(page.bytesOf(bulkConfirm.names)) + "? This can't be undone."
                            color: theme.colors.danger; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * theme.textScale)
                        }
                        AppButton { objectName: "isoBulkConfirm"; text: "Delete"; tone: "danger"; onClicked: page.deleteNames(bulkConfirm.names) }
                        AppButton { text: "Keep"; tone: "quiet"; onClicked: page.confirming = "" }
                    }
                }
                Label {
                    visible: !!page.library && page.library.files.length === 0
                    text: "None yet. Download an ISO above, or drop an ISO, OVA or QCOW2 appliance here. Appliances live in " + (page.library ? page.library.applianceFolder : "appliances/") + "."
                    color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * theme.textScale)
                }
                Repeater {
                    model: page.library ? page.library.files : []
                    Rectangle {
                        id: fileRow
                        required property var modelData
                        property bool confirming: false
                        objectName: "isoFile_" + modelData.name
                        Layout.fillWidth: true; implicitHeight: fileLayout.implicitHeight + 18; radius: 10
                        color: theme.colors.surface; border.color: theme.colors.border
                        RowLayout {
                            id: fileLayout
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 10
                            AppCheckBox {
                                objectName: "isoPick_" + fileRow.modelData.name
                                checked: !!page.picked[fileRow.modelData.name]
                                onToggled: page.togglePick(fileRow.modelData.name)
                                Accessible.name: "Select " + fileRow.modelData.name
                            }
                            AppIcon { name: "disk"; width: 18; height: 18; color: fileRow.modelData.newest ? theme.colors.accent : theme.colors.muted }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 1
                                Label { textFormat: Text.PlainText; text: fileRow.modelData.name; elide: Text.ElideMiddle; Layout.fillWidth: true; font.pixelSize: Math.round(12 * theme.textScale) }
                                Label {
                                    text: (fileRow.modelData.type === "disk" ? "Appliance disk · " : "Installer ISO · ") + page.size(fileRow.modelData.size) + (fileRow.modelData.sourceName ? " · " + fileRow.modelData.sourceName + " " + fileRow.modelData.version : "") + (fileRow.modelData.newest ? "" : " · older version, you can delete it")
                                    color: fileRow.modelData.newest ? theme.colors.muted : theme.colors.warning; font.pixelSize: Math.round(10 * theme.textScale)
                                }
                            }
                            AppButton { visible: !fileRow.confirming; text: fileRow.modelData.type === "disk" ? "Import VM" : "New VM"; iconName: "plus"; tone: "quiet"; hint: fileRow.modelData.type === "disk" ? "Copy appliance into an independent VM disk" : "Create a VM from this ISO"; onClicked: page.useMedia(fileRow.modelData) }
                            AppButton { visible: !fileRow.confirming; iconName: "trash"; tone: "quiet"; hint: "Delete this ISO"; onClicked: fileRow.confirming = true }
                            AppButton { visible: fileRow.confirming; text: "Delete"; tone: "danger"; onClicked: { fileRow.confirming = false; page.library.remove(fileRow.modelData.name) } }
                            AppButton { visible: fileRow.confirming; text: "Keep"; tone: "quiet"; onClicked: fileRow.confirming = false }
                        }
                    }
                }
            }
        }
    }
}
