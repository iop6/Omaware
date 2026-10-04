// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One system in the OS Shop: a banner in the system's color with its badge, what you'd get (installer or
// ready-made VM, and how the download is checked), and the action that fits: download, update, new VM.
// Clicking anywhere else on the card opens its details.
Rectangle {
    id: card
    required property string sourceId
    property var shop: null
    readonly property var info: shop ? shop.infoFor(sourceId) : ({})
    readonly property bool busy: info.status === "downloading" || info.status === "unpacking" || info.status === "starting"
    readonly property color brand: info.color || theme.colors.accent
    readonly property bool image: info.media === "image"
    readonly property bool website: info.kind === "page"
    readonly property real progress: busy && info.total ? info.received / info.total : 0
    // The file "New VM" uses: the picked version if you have it, otherwise your newest.
    readonly property var file: shop ? shop.fileFor(sourceId, info.selectedVersion) : null
    property bool removing: false
    objectName: "isoSource_" + sourceId
    implicitHeight: body.y + body.implicitHeight + 16
    radius: 14
    color: theme.colors.surface
    border.color: hover.hovered ? Qt.rgba(brand.r, brand.g, brand.b, .75) : info.updateAvailable ? theme.colors.warning : theme.colors.border
    Behavior on border.color { enabled: !theme.reducedMotion; ColorAnimation { duration: 140 } }
    transform: Translate { y: hover.hovered && !theme.reducedMotion ? -3 : 0; Behavior on y { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } } }
    HoverHandler { id: hover }
    TapHandler { onTapped: if (card.shop) card.shop.openDetails(card.sourceId) }
    Accessible.role: Accessible.Grouping
    Accessible.name: info.name || sourceId

    // A soft shadow that grows when the card lifts.
    Rectangle {
        z: -1; anchors.fill: parent; anchors.topMargin: 6; anchors.bottomMargin: -5; anchors.leftMargin: 3; anchors.rightMargin: 3
        radius: parent.radius; color: "#000000"; opacity: hover.hovered ? .22 : .07
        Behavior on opacity { enabled: !theme.reducedMotion; NumberAnimation { duration: 140 } }
    }

    // ---- Banner ----
    Rectangle {
        id: banner
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 1
        height: 76; radius: card.radius - 1; clip: true
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: card.brand }
            GradientStop { position: 1; color: Qt.darker(card.brand, 1.9) }
        }
        // Squares off the bottom corners, where the banner meets the card.
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: parent.radius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: card.brand }
                GradientStop { position: 1; color: Qt.darker(card.brand, 1.9) }
            }
        }
        // The badge again, large and faint, as a watermark.
        Text {
            anchors.right: parent.right; anchors.rightMargin: -6; anchors.bottom: parent.bottom; anchors.bottomMargin: -22
            text: card.shop ? card.shop.badge(card.info.name || "") : ""
            color: "#ffffff"; opacity: .13; font.pixelSize: 84; font.weight: Font.Black
        }
        // Download progress fills the banner.
        Rectangle {
            visible: card.busy; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: parent.width * card.progress; color: "#ffffff"; opacity: .14
            Behavior on width { enabled: !theme.reducedMotion; NumberAnimation { duration: 400 } }
        }
        // What's going on, top right.
        Rectangle {
            id: stateChip
            readonly property string text: card.busy ? (card.info.status === "unpacking" ? "Unpacking" : card.info.status === "starting" ? "Starting" : card.progress > 0 ? Math.round(card.progress * 100) + "%" : "Downloading")
                : card.info.updateAvailable ? "Update" : card.info.have ? "In your media" : ""
            visible: text !== ""
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10
            width: stateText.implicitWidth + 16; height: 22; radius: 11
            color: card.info.updateAvailable && !card.busy ? theme.colors.warning : Qt.rgba(0, 0, 0, .32)
            Label { id: stateText; anchors.centerIn: parent; text: stateChip.text; color: "#ffffff"; font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.DemiBold }
        }
    }
    // The badge: the system's initials (no trademarked logos), overlapping the banner.
    Rectangle {
        id: badge
        x: 16; y: banner.y + banner.height - height / 2 - 4
        width: 54; height: 54; radius: 15
        border.width: 3; border.color: theme.colors.surface
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(card.brand, 1.25) }
            GradientStop { position: 1; color: card.brand }
        }
        Label { anchors.centerIn: parent; text: card.shop ? card.shop.badge(card.info.name || "") : ""; color: "#ffffff"; font.weight: Font.Bold; font.pixelSize: Math.round(18 * theme.textScale) }
    }

    // ---- Body ----
    ColumnLayout {
        id: body
        anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 16
        y: badge.y + badge.height + 8
        spacing: 8
        ColumnLayout {
            Layout.fillWidth: true; spacing: 2
            Label { textFormat: Text.PlainText; text: card.info.name || ""; font.weight: Font.DemiBold; font.pixelSize: Math.round(16 * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true }
            Label {
                objectName: "isoStatus_" + card.sourceId
                Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: Math.round(11 * theme.textScale)
                color: card.info.error ? theme.colors.danger : card.info.updateAvailable ? theme.colors.warning : card.info.upToDate ? theme.colors.success : theme.colors.muted
                text: card.info.error ? "Couldn't check"
                    : card.info.status === "unpacking" ? (card.image ? "Download checked, unpacking the VM disk…" : "Download checked, unpacking…")
                    : card.info.status === "starting" ? "Asking " + (card.sourceId === "windows-11" ? "Microsoft" : "the publisher") + " for a download link…"
                    : card.busy ? "Downloading " + (card.info.selectedVersion || "") + "…"
                    : card.website ? (card.info.have ? "In your media: " + card.info.have : "From the publisher's website")
                    : card.info.status === "checking" ? "Checking…"
                    : card.info.upToDate ? "✓ Up to date · " + card.info.version
                    : card.info.updateAvailable ? "Update: " + card.info.version + " (you have " + card.info.have + ")"
                    : card.info.version ? card.info.version + (card.info.size ? " · " + card.shop.size(card.info.size) : "")
                    : card.info.have ? "In your media: " + card.info.have
                    : "Not checked yet"
            }
        }
        Label {
            text: card.info.description || ""; color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale)
            Layout.fillWidth: true; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
            Layout.preferredHeight: Math.ceil(2 * (font.pixelSize * 1.4))
        }
        // What you get, and how it's checked.
        Flow {
            Layout.fillWidth: true; spacing: 6
            ShopChip { text: card.website ? "Publisher's website" : card.image ? "Ready-made VM" : "Installer"; iconName: card.website ? "globe" : card.image ? "disk" : "download" }
            ShopChip {
                visible: !card.website && !!card.info.trust
                objectName: "isoTrust_" + card.sourceId
                text: card.info.trust === "unverified" ? "Unverified" : "Checked"
                iconName: card.info.trust === "unverified" ? "warning" : "shield"
                ink: card.info.trust === "unverified" ? theme.colors.warning : theme.colors.success
                hint: card.info.trust === "unverified" ? card.info.trustNote || "The publisher gives no checksum that proves this file is theirs." : "Checked against the publisher's own checksum; thrown away if it doesn't match."
            }
            ShopChip { visible: (card.info.versions || []).length > 1; text: (card.info.versions || []).length + " versions"; iconName: "history" }
        }
        // Progress while downloading.
        ColumnLayout {
            visible: card.busy
            Layout.fillWidth: true; spacing: 4
            AppProgressBar { Layout.fillWidth: true; from: 0; to: Math.max(1, card.info.total || card.info.size || 1); value: card.info.received || 0; indeterminate: card.info.status === "unpacking" || card.info.status === "starting" || !(card.info.total || card.info.size) }
            Label {
                text: card.info.status === "starting" ? "This takes a few seconds."
                    : card.info.status === "unpacking" ? "Almost there."
                    : card.shop ? card.shop.progressText(card.info) : ""
                color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true
            }
        }
        // Deleting this system's files, confirmed in place.
        RowLayout {
            visible: card.removing
            Layout.fillWidth: true; spacing: 6
            Label {
                readonly property var mine: card.shop ? card.shop.filesOf(card.sourceId) : []
                text: "Delete " + (mine.length === 1 ? "this file" : mine.length + " files") + " (" + (card.shop ? card.shop.size(card.shop.bytesOf(mine.map(function(f) { return f.name }))) : "") + ")?"
                color: theme.colors.danger; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: Math.round(12 * theme.textScale)
            }
            AppButton { objectName: "isoDeleteConfirm_" + card.sourceId; text: "Delete"; tone: "danger"; implicitHeight: 32; onClicked: { card.removing = false; card.shop.deleteNames(card.shop.filesOf(card.sourceId).map(function(f) { return f.name })) } }
            AppButton { text: "Keep"; tone: "quiet"; implicitHeight: 32; onClicked: card.removing = false }
        }
        RowLayout {
            visible: !card.removing
            Layout.fillWidth: true; spacing: 4
            AppButton {
                objectName: "isoDelete_" + card.sourceId
                visible: !!card.info.have && !card.busy
                iconName: "trash"; tone: "quiet"; implicitHeight: 34
                hint: "Delete your " + (card.info.name || "") + " files"
                onClicked: card.removing = true
            }
            AppButton { objectName: "isoDetails_" + card.sourceId; iconName: "info"; tone: "quiet"; implicitHeight: 34; hint: "Versions, checksum and your copies"; onClicked: card.shop.openDetails(card.sourceId) }
            Item { Layout.fillWidth: true }
            AppButton {
                visible: !card.busy && !!card.file && (card.website || !!card.info.selectedHave || !card.info.selectedVersion)
                text: card.image || (card.file && card.file.type === "disk") ? "Import VM" : "New VM"; iconName: "plus"; implicitHeight: 34
                // The main action once you have it.
                tone: card.website || !card.info.selectedHave ? "quiet" : "primary"
                hint: card.image ? "Make a new VM from this disk" : "Create a VM from this ISO"
                onClicked: card.shop.useMedia(card.file)
            }
            AppButton {
                // When a publisher refuses an automated download, its website still works.
                visible: !card.busy && !card.website && !!card.info.page && !!card.info.error
                text: "Website"; iconName: "globe"; tone: "quiet"; implicitHeight: 34; hint: card.info.page
                onClicked: Qt.openUrlExternally(card.info.page)
            }
            AppButton { visible: card.busy && card.info.status !== "starting"; text: "Cancel"; tone: "quiet"; implicitHeight: 34; onClicked: card.shop.library.cancel(card.sourceId) }
            AppButton {
                objectName: "isoGet_" + card.sourceId
                visible: !card.busy && (card.website || !card.info.selectedHave)
                text: card.website ? "Get from website" : card.shop ? card.shop.getLabel(card.info) : "Download"
                iconName: card.website ? "globe" : "download"
                tone: "primary"; implicitHeight: 34
                enabled: card.website || (card.info.status === "ready" && !!card.info.selectedVersion)
                hint: card.website ? card.info.page : card.info.selectedFile || ""
                onClicked: card.website ? Qt.openUrlExternally(card.info.page) : card.shop.library.download(card.sourceId)
            }
        }
    }
}
