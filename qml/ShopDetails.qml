// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A system's details, sliding in from the right: pick a version and language, see how the download is
// checked, and manage the copies you have (keep an older one, make a VM from it).
Popup {
    id: panel
    objectName: "isoDetails"
    property var shop: null
    property string sourceId: ""
    readonly property var info: shop && sourceId ? shop.infoFor(sourceId) : ({})
    readonly property color brand: info.color || theme.colors.accent
    readonly property bool busy: info.status === "downloading" || info.status === "unpacking" || info.status === "starting"
    readonly property bool website: info.kind === "page"
    readonly property var mine: shop && sourceId ? shop.filesOf(sourceId) : []
    // Versions change rarely; keep the lists steady while download progress updates arrive.
    property var versions: []
    property var languages: []
    onInfoChanged: {
        if (JSON.stringify(info.versions || []) !== JSON.stringify(versions))
            versions = info.versions || [];
        if (JSON.stringify(info.languages || []) !== JSON.stringify(languages))
            languages = info.languages || [];
    }
    function show(id) {
        sourceId = id;
        open();
    }
    component Fact: Label {
        color: theme.colors.muted
        font.pixelSize: Math.round(12 * theme.textScale)
    }
    component Value: Label {
        textFormat: Text.PlainText
        font.pixelSize: Math.round(12 * theme.textScale)
        Layout.fillWidth: true
        elide: Text.ElideMiddle
    }
    parent: Overlay.overlay
    x: parent ? parent.width - width : 0
    y: 0
    width: parent ? Math.min(500, parent.width) : 500
    height: parent ? parent.height : 600
    modal: true
    dim: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: Transition {
        NumberAnimation {
            property: "x"
            from: panel.parent ? panel.parent.width : 0
            to: panel.parent ? panel.parent.width - panel.width : 0
            duration: theme.reducedMotion ? 0 : 220
            easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "x"
            to: panel.parent ? panel.parent.width : 0
            duration: theme.reducedMotion ? 0 : 160
            easing.type: Easing.InCubic
        }
    }
    background: Rectangle {
        color: theme.colors.background
        border.color: theme.colors.border
    }
    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, .45)
    }

    contentItem: ColumnLayout {
        spacing: 0
        // ---- Banner ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: bannerColumn.implicitHeight + 40
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0
                    color: panel.brand
                }
                GradientStop {
                    position: 1
                    color: Qt.darker(panel.brand, 2.1)
                }
            }
            clip: true
            Text {
                anchors.right: parent.right
                anchors.rightMargin: -10
                anchors.bottom: parent.bottom
                anchors.bottomMargin: -36
                textFormat: Text.PlainText
                text: panel.shop ? panel.shop.badge(panel.info.name || "") : ""
                color: "#ffffff"
                opacity: .12
                font.pixelSize: 150
                font.weight: Font.Black
            }
            AppButton {
                objectName: "isoDetailsClose"
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                iconName: "close"
                tone: "quiet"
                ink: "#ffffff"
                hint: "Close"
                onClicked: panel.close()
            }
            ColumnLayout {
                id: bannerColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 22
                anchors.rightMargin: 56
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: 68
                    Layout.preferredHeight: 68
                    radius: 18
                    gradient: Gradient {
                        GradientStop {
                            position: 0
                            color: Qt.lighter(panel.brand, 1.3)
                        }
                        GradientStop {
                            position: 1
                            color: panel.brand
                        }
                    }
                    border.color: Qt.rgba(1, 1, 1, .35)
                    border.width: 2
                    Label {
                        anchors.centerIn: parent
                        textFormat: Text.PlainText
                        text: panel.shop ? panel.shop.badge(panel.info.name || "") : ""
                        color: "#ffffff"
                        font.weight: Font.Bold
                        font.pixelSize: Math.round(24 * theme.textScale)
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    text: panel.info.name || ""
                    color: "#ffffff"
                    font.pixelSize: Math.round(24 * theme.textScale)
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                Label {
                    textFormat: Text.PlainText
                    text: panel.info.description || ""
                    color: "#ffffff"
                    opacity: .85
                    font.pixelSize: Math.round(13 * theme.textScale)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
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
                spacing: 16
                Item {
                    Layout.preferredHeight: 4
                }
                // ---- How it's checked ----
                Rectangle {
                    id: trustBox
                    visible: !panel.website && !!panel.info.trust
                    readonly property bool unverified: panel.info.trust === "unverified"
                    readonly property color ink: unverified ? theme.colors.warning : theme.colors.success
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    implicitHeight: trustRow.implicitHeight + 24
                    radius: 10
                    color: Qt.rgba(ink.r, ink.g, ink.b, .09)
                    border.color: Qt.rgba(ink.r, ink.g, ink.b, .35)
                    RowLayout {
                        id: trustRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 12
                        spacing: 12
                        AppIcon {
                            name: trustBox.unverified ? "warning" : "shield"
                            color: trustBox.ink
                            width: 24
                            height: 24
                            Layout.alignment: Qt.AlignTop
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Label {
                                objectName: "isoTrustTitle"
                                textFormat: Text.PlainText
                                text: trustBox.unverified ? "Unverified download" : "Checked against the publisher's " + (panel.info.algorithm === "sha512" ? "SHA-512" : "SHA-256") + " checksum"
                                font.weight: Font.DemiBold
                                color: trustBox.ink
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            Label {
                                textFormat: Text.PlainText
                                text: trustBox.unverified ? (panel.info.trustNote || "The publisher gives no checksum that proves this file is theirs.") : "OmaWare throws the download away if it doesn't match, so you get exactly the file the publisher released."
                                color: theme.colors.muted
                                font.pixelSize: Math.round(12 * theme.textScale)
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
                Label {
                    visible: !!panel.info.note
                    textFormat: Text.PlainText
                    text: panel.info.note || ""
                    color: theme.colors.warning
                    font.pixelSize: Math.round(12 * theme.textScale)
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    wrapMode: Text.WordWrap
                }
                Label {
                    textFormat: Text.PlainText
                    visible: !!panel.info.error
                    text: panel.info.error || ""
                    color: theme.colors.danger
                    font.pixelSize: Math.round(12 * theme.textScale)
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    wrapMode: Text.WordWrap
                }

                // ---- Version and language ----
                GridLayout {
                    visible: !panel.website
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    columns: 2
                    columnSpacing: 14
                    rowSpacing: 10
                    Label {
                        visible: panel.versions.length > 0
                        text: "Version"
                        color: theme.colors.muted
                    }
                    AppSelect {
                        objectName: "isoVersion_" + panel.sourceId
                        visible: panel.versions.length > 0
                        Accessible.name: "Version"
                        Layout.fillWidth: true
                        implicitHeight: 36
                        enabled: !panel.busy && panel.versions.length > 1
                        model: panel.versions.map(function (v, i) {
                            return v + (i === 0 ? " (newest)" : "") + ((panel.info.haveVersions || []).indexOf(v) >= 0 ? " · in your media" : "");
                        })
                        currentIndex: Math.max(0, panel.versions.indexOf(panel.info.selectedVersion))
                        onActivated: function (index) {
                            panel.shop.library.setVersion(panel.sourceId, index === 0 ? "" : panel.versions[index]);
                        }
                    }
                    Label {
                        visible: panel.languages.length > 0
                        text: "Language"
                        color: theme.colors.muted
                    }
                    AppSelect {
                        objectName: "isoLanguage_" + panel.sourceId
                        visible: panel.languages.length > 0
                        Accessible.name: "Language"
                        Layout.fillWidth: true
                        implicitHeight: 36
                        enabled: !panel.busy
                        model: panel.languages
                        currentIndex: Math.max(0, panel.languages.indexOf(panel.info.language))
                        onActivated: function (index) {
                            panel.shop.chooseLanguage(panel.sourceId, panel.languages[index]);
                        }
                    }
                }
                Label {
                    visible: !panel.website && panel.versions.length > 1 && panel.info.selectedVersion !== panel.versions[0]
                    text: "An earlier version you download is kept: it isn't offered for deletion as an older version."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    wrapMode: Text.WordWrap
                }

                // ---- Facts ----
                GridLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    columns: 2
                    columnSpacing: 14
                    rowSpacing: 6
                    Fact {
                        visible: !panel.website
                        text: "Kind"
                    }
                    Value {
                        visible: !panel.website
                        text: panel.info.media === "image" ? "Ready-made VM disk, imported as a new VM" : "Installer ISO"
                    }
                    Fact {
                        visible: !!panel.info.selectedSize
                        text: "Size"
                    }
                    Value {
                        visible: !!panel.info.selectedSize
                        text: panel.shop ? panel.shop.size(panel.info.selectedSize) : ""
                    }
                    Fact {
                        visible: !!panel.info.selectedFile
                        text: "File"
                    }
                    Value {
                        visible: !!panel.info.selectedFile
                        text: panel.info.selectedFile || ""
                    }
                    Fact {
                        visible: !!panel.info.checksum
                        text: (panel.info.algorithm || "").toUpperCase().replace("SHA", "SHA-")
                    }
                    Value {
                        objectName: "isoChecksum"
                        visible: !!panel.info.checksum
                        text: panel.info.checksum || ""
                        font.family: "monospace"
                    }
                    Fact {
                        visible: !!panel.info.page
                        text: "Website"
                    }
                    Label {
                        visible: !!panel.info.page
                        textFormat: Text.StyledText
                        linkColor: theme.colors.accent
                        text: "<a href=\"" + (panel.info.page || "") + "\">" + (panel.info.page || "").replace(/^https:\/\//, "").replace(/\/.*$/, "") + "</a>"
                        font.pixelSize: Math.round(12 * theme.textScale)
                        onLinkActivated: function (link) {
                            Qt.openUrlExternally(link);
                        }
                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }

                // ---- Progress ----
                ColumnLayout {
                    visible: panel.busy
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    spacing: 4
                    AppProgressBar {
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, panel.info.total || panel.info.size || 1)
                        value: panel.info.received || 0
                        indeterminate: panel.info.status !== "downloading" || !(panel.info.total || panel.info.size)
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: panel.shop ? panel.shop.progressText(panel.info) : ""
                        color: theme.colors.muted
                        font.pixelSize: Math.round(11 * theme.textScale)
                    }
                }

                // ---- Your copies ----
                ColumnLayout {
                    visible: panel.mine.length > 0
                    Layout.fillWidth: true
                    Layout.leftMargin: 22
                    Layout.rightMargin: 22
                    spacing: 6
                    Label {
                        text: "Your copies"
                        font.weight: Font.DemiBold
                        font.pixelSize: Math.round(14 * theme.textScale)
                    }
                    Repeater {
                        model: panel.mine
                        Rectangle {
                            id: copy
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: copyRow.implicitHeight + 14
                            radius: 8
                            color: theme.colors.surface
                            border.color: theme.colors.border
                            RowLayout {
                                id: copyRow
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 10
                                anchors.rightMargin: 6
                                spacing: 6
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Label {
                                        textFormat: Text.PlainText
                                        text: copy.modelData.version || copy.modelData.name
                                        font.weight: Font.Medium
                                        elide: Text.ElideMiddle
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        textFormat: Text.PlainText
                                        text: panel.shop ? panel.shop.fileLine(copy.modelData) : ""
                                        color: copy.modelData.newest ? theme.colors.muted : theme.colors.warning
                                        font.pixelSize: Math.round(11 * theme.textScale)
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }
                                AppButton {
                                    objectName: "isoKeepDetails_" + copy.modelData.name
                                    iconName: "pin"
                                    tone: "quiet"
                                    implicitHeight: 30
                                    checked: !!copy.modelData.kept
                                    hint: copy.modelData.kept ? "Kept: never offered as an older version to delete. Click to stop keeping it." : "Keep this version, so it's never offered as an older version to delete"
                                    onClicked: panel.shop.library.setKept(copy.modelData.name, !copy.modelData.kept)
                                }
                                AppButton {
                                    text: copy.modelData.type === "disk" ? "Import VM" : "New VM"
                                    iconName: "plus"
                                    tone: "quiet"
                                    implicitHeight: 30
                                    onClicked: {
                                        panel.close();
                                        panel.shop.useMedia(copy.modelData);
                                    }
                                }
                            }
                        }
                    }
                }
                Item {
                    Layout.preferredHeight: 8
                }
            }
        }

        // ---- Actions ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.colors.border
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 8
            AppButton {
                visible: !!panel.info.page && !panel.website
                text: "Website"
                iconName: "globe"
                tone: "quiet"
                onClicked: Qt.openUrlExternally(panel.info.page)
            }
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                visible: panel.busy && panel.info.status !== "starting"
                text: "Cancel download"
                tone: "quiet"
                onClicked: panel.shop.library.cancel(panel.sourceId)
            }
            AppButton {
                objectName: "isoDetailsGet"
                visible: !panel.busy && (panel.website || !panel.info.selectedHave)
                text: panel.website ? "Get from website" : panel.shop ? panel.shop.getLabel(panel.info) : "Download"
                iconName: panel.website ? "globe" : "download"
                tone: "primary"
                enabled: panel.website || (panel.info.status === "ready" && !!panel.info.selectedVersion)
                onClicked: panel.website ? Qt.openUrlExternally(panel.info.page) : panel.shop.library.download(panel.sourceId)
            }
            AppButton {
                visible: !panel.busy && !panel.website && !!panel.info.selectedHave
                text: panel.info.media === "image" ? "Import VM" : "New VM"
                iconName: "plus"
                tone: "primary"
                onClicked: {
                    const f = panel.shop.fileFor(panel.sourceId, panel.info.selectedVersion);
                    if (f) {
                        panel.close();
                        panel.shop.useMedia(f);
                    }
                }
            }
        }
    }
}
