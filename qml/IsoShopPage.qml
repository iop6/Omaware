// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// The OS Shop: free, official operating systems (installers and ready-made VMs), downloaded into OmaWare's
// media folders and checked against each publisher's checksum. A spotlight up top, the catalogue by
// category, your media underneath, and downloads in a tray at the bottom.
ColumnLayout {
    id: page
    objectName: "isoShop"
    property var library: null
    property string category: "all"
    property string query: ""
    signal useIso(string path)
    signal useAppliance(string path)
    function useMedia(file) {
        if (!file)
            return;
        if (file.type === "disk")
            useAppliance(file.path);
        else
            useIso(file.path);
    }
    spacing: 14

    readonly property var categories: [
        {
            key: "all",
            label: "All"
        },
        {
            key: "desktop",
            label: "Desktop"
        },
        {
            key: "server",
            label: "Server"
        },
        {
            key: "security",
            label: "Security & networking"
        },
        {
            key: "windows",
            label: "Windows"
        },
        {
            key: "mine",
            label: "Your media"
        }
    ]
    // Names and categories never change, so filtering only rebuilds the grid when the filter does.
    property var catalogue: ({})
    function remember() {
        let map = {};
        for (const s of (library ? library.sources : []))
            map[s.id] = {
                name: s.name,
                category: s.category,
                description: s.description
            };
        catalogue = map;
    }
    readonly property var shown: {
        const ids = library ? library.sourceIds : [], q = query.trim().toLowerCase(), info = catalogue;
        return ids.filter(function (id) {
            const s = info[id] || {};
            return (category === "all" || s.category === category) && (q === "" || (s.name + " " + s.description).toLowerCase().indexOf(q) >= 0);
        });
    }
    // With everything shown, the catalogue is split into a shelf per category; otherwise one grid.
    readonly property var groups: {
        if (category !== "all" || query.trim() !== "")
            return [
                {
                    key: "",
                    label: "",
                    ids: shown
                }
            ];
        const info = catalogue, ids = shown;
        return categories.filter(function (c) {
            return c.key !== "all" && c.key !== "mine";
        }).map(function (c) {
            return {
                key: c.key,
                label: c.label,
                ids: ids.filter(function (id) {
                    return (info[id] || {}).category === c.key;
                })
            };
        }).filter(function (g) {
            return g.ids.length > 0;
        });
    }
    function countIn(key) {
        if (key === "mine")
            return library ? library.files.length : 0;
        let n = 0;
        for (const id in catalogue)
            if (key === "all" || catalogue[id].category === key)
                ++n;
        return n;
    }
    function infoFor(id) {
        for (const s of (library ? library.sources : []))
            if (s.id === id)
                return s;
        return ({});
    }
    function size(bytes) {
        const b = Number(bytes) || 0;
        return b >= 1073741824 ? (b / 1073741824).toFixed(1) + " GB" : b >= 1048576 ? Math.round(b / 1048576) + " MB" : Math.round(b / 1024) + " KB";
    }
    // A system's initials, for its badge (no trademarked logos).
    function badge(name) {
        const words = String(name).replace(/\(.*\)/g, "").replace(/[^A-Za-z0-9 ]/g, " ").split(" ").filter(function (w) {
            return w !== "";
        });
        if (words.length > 1 && /^[0-9]+$/.test(words[1]))
            return words[0][0] + words[1];   // "Windows 11" -> W11
        // An edition suffix ("pfSense CE", "Proxmox VE") isn't part of the name.
        if (words.length === 2 && /^[A-Z]{2}$/.test(words[1]))
            return words[0][0].toUpperCase() + words[0][1];
        return words.length > 1 && words[1].length > 1 && /^[A-Z]/.test(words[1]) ? words[0][0] + words[1][0] : String(words[0] || "?").slice(0, 2);
    }
    // The file of a system that "New VM" uses: that version if you have it, otherwise your newest.
    function fileFor(id, version) {
        let best = null;
        for (const f of (library ? library.files : [])) {
            if (f.source !== id)
                continue;
            if (version && f.version === version)
                return f;
            if (!best || (f.newest && !best.newest))
                best = f;
        }
        return best;
    }
    function getLabel(info) {
        const newest = (info.versions || [])[0];
        if (info.updateAvailable && info.selectedVersion === newest)
            return "Update to " + newest;
        return info.selectedVersion && info.selectedVersion !== newest ? "Download " + info.selectedVersion : "Download";
    }
    function progressText(info) {
        const rate = info.rate || 0, total = info.total || info.size || 0, received = info.received || 0;
        return size(received) + (total ? " of " + size(total) : "") + (rate ? " · " + size(rate) + "/s" : "") + (rate && total ? " · about " + Math.max(1, Math.round((total - received) / rate / 60)) + " min left" : "");
    }
    function fileLine(f) {
        const how = f.check === "sha256" ? "checked (SHA-256)" : f.check === "sha512" ? "checked (SHA-512)" : f.check === "md5" ? "MD5 only" : f.check === "unverified" ? "unverified" : "added by you";
        // A VM disk lists its whole virtual size, but only takes what's written to it.
        const virtualDisk = f.type === "disk" && f.listedSize > f.size * 1.5 ? " on disk (" + size(f.listedSize) + " virtual disk)" : "";
        return (f.type === "disk" ? "Appliance disk · " : "Installer ISO · ") + size(f.size) + virtualDisk + " · " + how + (f.kept ? " · kept" : f.newest ? "" : " · older version, you can delete it");
    }
    // ---- Deleting ----
    property var picked: ({})              // file name -> true, for "Delete selected"
    property string confirming: ""         // "older" or "selected" while asking for confirmation
    readonly property var pickedNames: (library ? library.files : []).map(function (f) {
        return f.name;
    }).filter(function (n) {
        return !!page.picked[n];
    })
    readonly property var olderNames: (library ? library.files : []).filter(function (f) {
        return !f.newest;
    }).map(function (f) {
        return f.name;
    })
    function filesOf(source) {
        return (library ? library.files : []).filter(function (f) {
            return f.source === source;
        });
    }
    function bytesOf(names) {
        let total = 0;
        for (const f of (library ? library.files : []))
            if (names.indexOf(f.name) >= 0)
                total += Number(f.size) || 0;
        return total;
    }
    // Remembered, so the shop offers the same Windows language next time.
    signal languageChosen(string id, string language)
    function chooseLanguage(id, language) {
        library.setLanguage(id, language);
        languageChosen(id, language);
    }
    function togglePick(name) {
        let next = Object.assign({}, picked);
        if (next[name])
            delete next[name];
        else
            next[name] = true;
        picked = next;
    }
    function deleteNames(names) {
        library.removeAll(names);
        let next = Object.assign({}, picked);
        for (const n of names)
            delete next[n];
        picked = next;
        confirming = "";
    }
    function openDetails(id) {
        details.show(id);
    }

    // ---- Spotlight: updates for your media first, then a few favourites ----
    property var spots: []
    property int spot: 0
    readonly property string spotId: spots.length ? spots[spot % spots.length] : ""
    readonly property var spotInfo: spotId ? infoFor(spotId) : ({})
    function refreshSpots() {
        if (!library)
            return;
        let picks = [];
        for (const s of library.sources)
            if (s.updateAvailable)
                picks.push(s.id);
        for (const id of ["kali-vm", "ubuntu-desktop", "windows-11-enterprise", "security-onion", "fedora-workstation", "windows-server"])
            if (picks.indexOf(id) < 0 && library.sourceIds.indexOf(id) >= 0)
                picks.push(id);
        picks = picks.slice(0, 5);
        if (JSON.stringify(picks) !== JSON.stringify(spots)) {
            spots = picks;
            spot = 0;
        }
    }
    onSpotChanged: if (!theme.reducedMotion)
        heroFade.restart()
    Connections {
        target: page.library
        function onChanged() {
            page.refreshSpots();
        }
    }

    component HeroChip: Rectangle {
        property string text: ""
        width: chipLabel.implicitWidth + 16
        height: 22
        radius: 11
        color: Qt.rgba(0, 0, 0, .25)
        Label {
            id: chipLabel
            anchors.centerIn: parent
            textFormat: Text.PlainText
            text: parent.text
            color: "#ffffff"
            font.pixelSize: Math.round(11 * theme.textScale)
        }
    }
    component HeroButton: AppButton {
        id: heroButton
        ink: "#ffffff"
        background: Rectangle {
            radius: 8
            color: Qt.rgba(1, 1, 1, heroButton.down ? .34 : heroButton.hovered ? .28 : .2)
            border.color: Qt.rgba(1, 1, 1, .5)
        }
    }

    onVisibleChanged: if (visible && library) {
        remember();
        refreshSpots();
        library.rescan();
        if (library.autoCheck && !library.checking)
            library.check();
    }
    Component.onCompleted: {
        remember();
        refreshSpots();
    }
    FileDialog {
        id: mediaPicker
        title: "Add installer or appliance media"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["ISO and appliances (*.iso *.ISO *.ova *.OVA *.qcow2 *.QCOW2)"]
        onAccepted: page.library.importFiles(selectedFiles)
    }
    Shortcut {
        sequence: "/"
        enabled: page.visible && !details.opened
        onActivated: {
            if (page.category === "mine")
                page.category = "all";
            search.forceActiveFocus();
        }
    }
    ShopDetails {
        id: details
        shop: page
    }

    // ---- Header ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 12
        Rectangle {
            Layout.preferredWidth: 46
            Layout.preferredHeight: 46
            radius: 13
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: Qt.lighter(theme.colors.accent, 1.2)
                }
                GradientStop {
                    position: 1
                    color: theme.colors.accent
                }
            }
            AppIcon {
                anchors.centerIn: parent
                name: "store"
                color: theme.colors.accentText
                width: 24
                height: 24
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: "OS Shop"
                font.pixelSize: Math.round(28 * theme.textScale)
                font.weight: Font.DemiBold
            }
            Label {
                objectName: "isoShopSummary"
                readonly property int updates: page.library ? page.library.sources.filter(function (s) {
                    return s.updateAvailable;
                }).length : 0
                textFormat: Text.PlainText
                text: page.countIn("all") + " systems: checked downloads, earlier versions and ready-made VMs" + (updates > 0 ? " · " + updates + (updates === 1 ? " update" : " updates") + " for your media" : "") + ". Drop in your own ISO, OVA or QCOW2."
                color: theme.colors.muted
                font.pixelSize: Math.round(12 * theme.textScale)
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }
        HelpButton {
            topic: "the-os-shop"
        }
        AppButton {
            objectName: "isoCheck"
            text: page.library && page.library.checking ? "Checking…" : "Check for updates"
            iconName: "refresh"
            enabled: !!page.library && !page.library.checking
            onClicked: page.library.check()
        }
        AppButton {
            objectName: "addMedia"
            text: "Add files…"
            iconName: "plus"
            enabled: !!page.library && !page.library.importing
            onClicked: mediaPicker.open()
        }
        AppButton {
            objectName: "isoOpenFolder"
            iconName: "folder"
            hint: page.library ? "Open " + page.library.folder : ""
            onClicked: Qt.openUrlExternally("file://" + page.library.folder)
        }
    }
    // ---- Categories and search ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Repeater {
            model: page.categories
            AppButton {
                required property var modelData
                objectName: "isoCategory_" + modelData.key
                text: modelData.label + "  " + page.countIn(modelData.key)
                tone: "quiet"
                checked: page.category === modelData.key
                ink: checked ? theme.colors.accent : theme.colors.muted
                implicitHeight: 32
                onClicked: page.category = modelData.key
            }
        }
        Item {
            Layout.fillWidth: true
        }
        AppField {
            id: search
            objectName: "isoSearch"
            visible: page.category !== "mine"
            Layout.preferredWidth: 240
            implicitHeight: 34
            leftPadding: 30
            placeholderText: "Search systems…   /"
            text: page.query
            onTextChanged: page.query = text
            Keys.onEscapePressed: {
                text = "";
                focus = false;
            }
            AppIcon {
                x: 9
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                name: "search"
                color: theme.colors.muted
            }
        }
    }

    ScrollView {
        id: scroller
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: scroller.availableWidth
            spacing: 18

            // ---- Spotlight ----
            Rectangle {
                id: hero
                objectName: "isoSpotlight"
                visible: page.category === "all" && page.query.trim() === "" && page.spotId !== ""
                readonly property color brand: page.spotInfo.color || theme.colors.accent
                readonly property bool busy: ["downloading", "unpacking", "starting"].indexOf(page.spotInfo.status) >= 0
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(200, heroContent.implicitHeight + 56)
                radius: 16
                clip: true
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop {
                        position: 0
                        color: hero.brand
                        Behavior on color {
                            enabled: !theme.reducedMotion
                            ColorAnimation {
                                duration: 420
                            }
                        }
                    }
                    GradientStop {
                        position: .65
                        color: Qt.darker(hero.brand, 1.7)
                        Behavior on color {
                            enabled: !theme.reducedMotion
                            ColorAnimation {
                                duration: 420
                            }
                        }
                    }
                    GradientStop {
                        position: 1
                        color: Qt.darker(hero.brand, 2.6)
                        Behavior on color {
                            enabled: !theme.reducedMotion
                            ColorAnimation {
                                duration: 420
                            }
                        }
                    }
                }
                HoverHandler {
                    id: heroHover
                }
                TapHandler {
                    onTapped: page.openDetails(page.spotId)
                }
                Timer {
                    interval: 7000
                    repeat: true
                    running: page.visible && hero.visible && page.spots.length > 1 && !heroHover.hovered && !theme.reducedMotion && !details.opened
                    onTriggered: page.spot = (page.spot + 1) % page.spots.length
                }
                // Rings, and the badge as a large watermark.
                Repeater {
                    model: 3
                    Rectangle {
                        required property int index
                        readonly property real d: 180 + index * 110
                        x: hero.width - d / 2 - 90
                        y: hero.height / 2 - d / 2
                        width: d
                        height: d
                        radius: d / 2
                        color: "transparent"
                        border.color: "#ffffff"
                        opacity: .09 - index * .02
                        border.width: 2
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 40
                    anchors.verticalCenter: parent.verticalCenter
                    textFormat: Text.PlainText
                    text: page.badge(page.spotInfo.name || "")
                    color: "#ffffff"
                    opacity: .16
                    font.pixelSize: 120
                    font.weight: Font.Black
                    visible: hero.width > 640
                }
                RowLayout {
                    id: heroContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: -6
                    anchors.leftMargin: 28
                    anchors.rightMargin: hero.width > 640 ? 230 : 28
                    spacing: 22
                    NumberAnimation {
                        id: heroFade
                        target: heroContent
                        property: "opacity"
                        from: 0
                        to: 1
                        duration: 320
                        easing.type: Easing.OutCubic
                    }
                    Rectangle {
                        visible: hero.width > 520
                        Layout.preferredWidth: 92
                        Layout.preferredHeight: 92
                        radius: 24
                        gradient: Gradient {
                            GradientStop {
                                position: 0
                                color: Qt.lighter(hero.brand, 1.35)
                            }
                            GradientStop {
                                position: 1
                                color: hero.brand
                            }
                        }
                        border.color: Qt.rgba(1, 1, 1, .4)
                        border.width: 2
                        Label {
                            anchors.centerIn: parent
                            textFormat: Text.PlainText
                            text: page.badge(page.spotInfo.name || "")
                            color: "#ffffff"
                            font.weight: Font.Bold
                            font.pixelSize: Math.round(32 * theme.textScale)
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            implicitWidth: overlineRow.implicitWidth + 18
                            implicitHeight: 22
                            radius: 11
                            color: page.spotInfo.updateAvailable ? theme.colors.warning : Qt.rgba(1, 1, 1, .2)
                            Row {
                                id: overlineRow
                                anchors.centerIn: parent
                                spacing: 5
                                AppIcon {
                                    name: page.spotInfo.updateAvailable ? "refresh" : "star"
                                    color: "#ffffff"
                                    width: 12
                                    height: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    text: page.spotInfo.updateAvailable ? "UPDATE FOR YOUR MEDIA" : page.spotInfo.media === "image" ? "READY TO RUN" : "SPOTLIGHT"
                                    color: "#ffffff"
                                    font.pixelSize: Math.round(10 * theme.textScale)
                                    font.weight: Font.Bold
                                    font.letterSpacing: 1.2
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                        }
                        Label {
                            objectName: "isoSpotlightName"
                            textFormat: Text.PlainText
                            text: page.spotInfo.name || ""
                            color: "#ffffff"
                            font.pixelSize: Math.round(30 * theme.textScale)
                            font.weight: Font.Bold
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            textFormat: Text.PlainText
                            text: page.spotInfo.description || ""
                            color: "#ffffff"
                            opacity: .86
                            font.pixelSize: Math.round(13 * theme.textScale)
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            HeroChip {
                                visible: !!page.spotInfo.version
                                text: page.spotInfo.version || ""
                            }
                            HeroChip {
                                visible: !!page.spotInfo.size
                                text: page.size(page.spotInfo.size)
                            }
                            HeroChip {
                                visible: !!page.spotInfo.trust
                                text: page.spotInfo.trust === "unverified" ? "⚠ Unverified" : "✓ Checked download"
                            }
                            HeroChip {
                                text: page.spotInfo.media === "image" ? "Ready-made VM" : "Installer"
                            }
                        }
                        RowLayout {
                            spacing: 8
                            Layout.topMargin: 4
                            HeroButton {
                                objectName: "isoSpotlightGet"
                                visible: !hero.busy && page.spotInfo.kind !== "page" && !page.spotInfo.selectedHave
                                text: page.getLabel(page.spotInfo)
                                iconName: "download"
                                enabled: page.spotInfo.status === "ready" && !!page.spotInfo.selectedVersion
                                onClicked: page.library.download(page.spotId)
                            }
                            HeroButton {
                                visible: !hero.busy && !!page.spotInfo.selectedHave
                                text: page.spotInfo.media === "image" ? "Import VM" : "New VM"
                                iconName: "plus"
                                onClicked: page.useMedia(page.fileFor(page.spotId, page.spotInfo.selectedVersion))
                            }
                            Label {
                                visible: hero.busy
                                textFormat: Text.PlainText
                                text: page.progressText(page.spotInfo)
                                color: "#ffffff"
                                font.pixelSize: Math.round(12 * theme.textScale)
                            }
                            HeroButton {
                                text: "Details"
                                iconName: "info"
                                onClicked: page.openDetails(page.spotId)
                            }
                        }
                    }
                }
                // Which spotlight, and a way to pick another.
                Row {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 28
                    anchors.bottomMargin: 12
                    spacing: 6
                    visible: page.spots.length > 1
                    Repeater {
                        model: page.spots.length
                        Rectangle {
                            required property int index
                            width: index === page.spot ? 22 : 8
                            height: 8
                            radius: 4
                            color: "#ffffff"
                            opacity: index === page.spot ? .95 : .4
                            Behavior on width {
                                enabled: !theme.reducedMotion
                                NumberAnimation {
                                    duration: 200
                                }
                            }
                            TapHandler {
                                onTapped: page.spot = index
                            }
                        }
                    }
                }
            }

            // ---- Catalogue ----
            Repeater {
                model: page.category === "mine" ? [] : page.groups
                ColumnLayout {
                    id: shelf
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 10
                    RowLayout {
                        visible: shelf.modelData.label !== ""
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            textFormat: Text.PlainText
                            text: shelf.modelData.label
                            font.pixelSize: Math.round(17 * theme.textScale)
                            font.weight: Font.DemiBold
                        }
                        Label {
                            textFormat: Text.PlainText
                            text: shelf.modelData.ids.length
                            color: theme.colors.muted
                            font.pixelSize: Math.round(12 * theme.textScale)
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: theme.colors.border
                        }
                        AppButton {
                            text: "Only these"
                            tone: "quiet"
                            implicitHeight: 26
                            ink: theme.colors.muted
                            onClicked: page.category = shelf.modelData.key
                        }
                    }
                    Flow {
                        id: grid
                        Layout.fillWidth: true
                        spacing: 14
                        readonly property int columns: width >= 1100 ? 4 : width >= 820 ? 3 : width >= 540 ? 2 : 1
                        readonly property real cardWidth: Math.floor((width - spacing * (columns - 1)) / columns)
                        Repeater {
                            model: shelf.modelData.ids
                            ShopCard {
                                required property string modelData
                                sourceId: modelData
                                shop: page
                                width: grid.cardWidth
                            }
                        }
                    }
                }
            }
            ColumnLayout {
                visible: page.category !== "mine" && page.shown.length === 0
                Layout.fillWidth: true
                Layout.topMargin: 30
                spacing: 8
                AppIcon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "search"
                    width: 36
                    height: 36
                    color: theme.colors.muted
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    textFormat: Text.PlainText
                    text: "Nothing matches “" + page.query + "”."
                    color: theme.colors.muted
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Got the file already? Drop it on the window to add it."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(12 * theme.textScale)
                }
            }

            // ---- Your media ----
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: page.category === "mine" || page.category === "all"
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 8
                    Label {
                        text: "Your media"
                        font.weight: Font.DemiBold
                        font.pixelSize: Math.round(17 * theme.textScale)
                    }
                    Label {
                        objectName: "isoTotals"
                        visible: !!page.library && page.library.files.length > 0
                        textFormat: Text.PlainText
                        text: page.library ? page.library.files.length + (page.library.files.length === 1 ? " file · " : " files · ") + page.size(page.bytesOf(page.library.files.map(function (f) {
                            return f.name;
                        }))) : ""
                        color: theme.colors.muted
                        font.pixelSize: Math.round(12 * theme.textScale)
                        Layout.fillWidth: true
                    }
                    AppButton {
                        objectName: "isoDeleteOlder"
                        visible: page.olderNames.length > 0 && page.confirming === ""
                        text: "Delete older versions (" + page.olderNames.length + " · " + page.size(page.bytesOf(page.olderNames)) + ")"
                        iconName: "trash"
                        tone: "quiet"
                        hint: "Remove every file that a newer download has replaced (kept files stay)"
                        onClicked: page.confirming = "older"
                    }
                    AppButton {
                        objectName: "isoSelectAll"
                        visible: !!page.library && page.library.files.length > 1 && page.confirming === ""
                        text: page.pickedNames.length === page.library.files.length ? "Clear selection" : "Select all"
                        tone: "quiet"
                        onClicked: {
                            let next = {};
                            if (page.pickedNames.length !== page.library.files.length)
                                for (const f of page.library.files)
                                    next[f.name] = true;
                            page.picked = next;
                        }
                    }
                    AppButton {
                        objectName: "isoDeleteSelected"
                        visible: page.pickedNames.length > 0 && page.confirming === ""
                        text: "Delete selected (" + page.pickedNames.length + " · " + page.size(page.bytesOf(page.pickedNames)) + ")"
                        iconName: "trash"
                        tone: "danger"
                        onClicked: page.confirming = "selected"
                    }
                }
                // How much room your media takes, next to what's left on the disk.
                ColumnLayout {
                    id: storage
                    objectName: "isoStorage"
                    visible: !!page.library && page.library.files.length > 0 && page.library.freeBytes > 0
                    readonly property real used: page.library ? page.bytesOf(page.library.files.map(function (f) {
                        return f.name;
                    })) : 0
                    readonly property real free: page.library ? page.library.freeBytes : 0
                    Layout.fillWidth: true
                    spacing: 4
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 8
                        radius: 4
                        color: theme.colors.subtle
                        clip: true
                        Rectangle {
                            width: parent.width * Math.min(1, storage.used / Math.max(1, storage.used + storage.free))
                            height: parent.height
                            radius: 4
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop {
                                    position: 0
                                    color: theme.colors.accent
                                }
                                GradientStop {
                                    position: 1
                                    color: Qt.lighter(theme.colors.accent, 1.3)
                                }
                            }
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: page.size(storage.used) + " of media · " + page.size(storage.free) + " free on this disk"
                        color: theme.colors.muted
                        font.pixelSize: Math.round(11 * theme.textScale)
                    }
                }
                // Confirmation for the bulk actions, in place.
                Rectangle {
                    id: bulkConfirm
                    visible: page.confirming !== ""
                    readonly property var names: page.confirming === "older" ? page.olderNames : page.pickedNames
                    Layout.fillWidth: true
                    implicitHeight: confirmRow.implicitHeight + 16
                    radius: 10
                    color: "transparent"
                    border.color: theme.colors.danger
                    RowLayout {
                        id: confirmRow
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 8
                        Label {
                            textFormat: Text.PlainText
                            text: "Delete " + bulkConfirm.names.length + (bulkConfirm.names.length === 1 ? " file" : " files") + " and free " + page.size(page.bytesOf(bulkConfirm.names)) + "? This can't be undone."
                            color: theme.colors.danger
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            font.pixelSize: Math.round(12 * theme.textScale)
                        }
                        AppButton {
                            objectName: "isoBulkConfirm"
                            text: "Delete"
                            tone: "danger"
                            onClicked: page.deleteNames(bulkConfirm.names)
                        }
                        AppButton {
                            text: "Keep"
                            tone: "quiet"
                            onClicked: page.confirming = ""
                        }
                    }
                }
                // Nothing yet: say what to do.
                Rectangle {
                    visible: !!page.library && page.library.files.length === 0
                    Layout.fillWidth: true
                    implicitHeight: emptyColumn.implicitHeight + 36
                    radius: 12
                    color: "transparent"
                    border.color: theme.colors.border
                    border.width: 2
                    ColumnLayout {
                        id: emptyColumn
                        anchors.centerIn: parent
                        width: parent.width - 40
                        spacing: 6
                        AppIcon {
                            Layout.alignment: Qt.AlignHCenter
                            name: "download"
                            width: 28
                            height: 28
                            color: theme.colors.muted
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            text: "None yet. Download a system above, or drop an ISO, OVA or QCOW2 appliance anywhere on the window. Appliances live in " + (page.library ? page.library.applianceFolder : "appliances/") + "."
                            color: theme.colors.muted
                            font.pixelSize: Math.round(12 * theme.textScale)
                        }
                    }
                }
                Repeater {
                    model: page.library ? page.library.files : []
                    Rectangle {
                        id: fileRow
                        required property var modelData
                        property bool confirming: false
                        readonly property color brand: fileRow.modelData.source ? (page.infoFor(fileRow.modelData.source).color || theme.colors.muted) : theme.colors.muted
                        objectName: "isoFile_" + modelData.name
                        Layout.fillWidth: true
                        implicitHeight: fileLayout.implicitHeight + 16
                        radius: 10
                        color: rowHover.hovered ? theme.colors.raised : theme.colors.surface
                        border.color: theme.colors.border
                        HoverHandler {
                            id: rowHover
                        }
                        RowLayout {
                            id: fileLayout
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 10
                            AppCheckBox {
                                objectName: "isoPick_" + fileRow.modelData.name
                                checked: !!page.picked[fileRow.modelData.name]
                                onToggled: page.togglePick(fileRow.modelData.name)
                                Accessible.name: "Select " + fileRow.modelData.name
                            }
                            Rectangle {
                                Layout.preferredWidth: 34
                                Layout.preferredHeight: 34
                                radius: 9
                                color: fileRow.modelData.source ? fileRow.brand : theme.colors.subtle
                                opacity: fileRow.modelData.newest ? 1 : .55
                                Label {
                                    visible: !!fileRow.modelData.source
                                    anchors.centerIn: parent
                                    textFormat: Text.PlainText
                                    text: page.badge(fileRow.modelData.sourceName || "")
                                    color: "#ffffff"
                                    font.weight: Font.Bold
                                    font.pixelSize: Math.round(12 * theme.textScale)
                                }
                                AppIcon {
                                    visible: !fileRow.modelData.source
                                    anchors.centerIn: parent
                                    name: "disk"
                                    width: 18
                                    height: 18
                                    color: theme.colors.muted
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    textFormat: Text.PlainText
                                    text: fileRow.modelData.sourceName ? fileRow.modelData.sourceName + " " + fileRow.modelData.version : fileRow.modelData.name
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    font.pixelSize: Math.round(13 * theme.textScale)
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    visible: !!fileRow.modelData.sourceName
                                    text: fileRow.modelData.name
                                    color: theme.colors.muted
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                    font.pixelSize: Math.round(10 * theme.textScale)
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    text: page.fileLine(fileRow.modelData)
                                    color: fileRow.modelData.newest ? theme.colors.muted : theme.colors.warning
                                    font.pixelSize: Math.round(11 * theme.textScale)
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            AppButton {
                                objectName: "isoKeep_" + fileRow.modelData.name
                                visible: !fileRow.confirming && !!fileRow.modelData.source
                                iconName: "pin"
                                tone: "quiet"
                                checked: !!fileRow.modelData.kept
                                hint: fileRow.modelData.kept ? "Kept: never offered as an older version to delete. Click to stop keeping it." : "Keep this version, so it's never offered as an older version to delete"
                                onClicked: page.library.setKept(fileRow.modelData.name, !fileRow.modelData.kept)
                            }
                            AppButton {
                                visible: !fileRow.confirming
                                text: fileRow.modelData.type === "disk" ? "Import VM" : "New VM"
                                iconName: "plus"
                                tone: "quiet"
                                hint: fileRow.modelData.type === "disk" ? "Copy appliance into an independent VM disk" : "Create a VM from this ISO"
                                onClicked: page.useMedia(fileRow.modelData)
                            }
                            AppButton {
                                visible: !fileRow.confirming
                                iconName: "trash"
                                tone: "quiet"
                                hint: "Delete this file"
                                onClicked: fileRow.confirming = true
                            }
                            AppButton {
                                visible: fileRow.confirming
                                text: "Delete"
                                tone: "danger"
                                onClicked: {
                                    fileRow.confirming = false;
                                    page.library.remove(fileRow.modelData.name);
                                }
                            }
                            AppButton {
                                visible: fileRow.confirming
                                text: "Keep"
                                tone: "quiet"
                                onClicked: fileRow.confirming = false
                            }
                        }
                    }
                }
            }
            Item {
                Layout.preferredHeight: 6
            }
        }
    }

    // ---- Downloads tray ----
    Rectangle {
        id: tray
        objectName: "isoDownloads"
        readonly property var active: page.library ? page.library.sources.filter(function (s) {
            return ["downloading", "unpacking", "starting"].indexOf(s.status) >= 0;
        }) : []
        visible: active.length > 0
        Layout.fillWidth: true
        implicitHeight: trayColumn.implicitHeight + 20
        radius: 12
        color: theme.colors.raised
        border.color: theme.colors.accent
        ColumnLayout {
            id: trayColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 14
            anchors.rightMargin: 10
            spacing: 8
            Repeater {
                model: tray.active
                RowLayout {
                    id: download
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        radius: 7
                        color: download.modelData.color || theme.colors.accent
                        Label {
                            anchors.centerIn: parent
                            textFormat: Text.PlainText
                            text: page.badge(download.modelData.name)
                            color: "#ffffff"
                            font.weight: Font.Bold
                            font.pixelSize: Math.round(10 * theme.textScale)
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: download.modelData.name + " " + (download.modelData.selectedVersion || "")
                        font.weight: Font.Medium
                        Layout.preferredWidth: 220
                        elide: Text.ElideRight
                    }
                    AppProgressBar {
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, download.modelData.total || download.modelData.size || 1)
                        value: download.modelData.received || 0
                        indeterminate: download.modelData.status !== "downloading" || !(download.modelData.total || download.modelData.size)
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: download.modelData.status === "unpacking" ? "Checked · unpacking…" : download.modelData.status === "starting" ? "Starting…" : page.progressText(download.modelData)
                        color: theme.colors.muted
                        font.pixelSize: Math.round(11 * theme.textScale)
                        Layout.preferredWidth: 250
                        elide: Text.ElideRight
                    }
                    AppButton {
                        visible: download.modelData.status !== "starting"
                        text: "Cancel"
                        tone: "quiet"
                        implicitHeight: 28
                        onClicked: page.library.cancel(download.modelData.id)
                    }
                }
            }
        }
    }
}
