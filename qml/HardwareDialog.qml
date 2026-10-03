// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
EditorDialog {
    id: dialog
    objectName: "hardwareDialog"
    property var workspace
    property var info: ({})
    property bool reviewing: false
    property int configurationPage: 0
    onConfigurationPageChanged: Qt.callLater(resetScroll)
    property bool isoChanged: false
    heading: reviewing ? "Review hardware changes" : "Edit hardware"
    headerIcon: "cpu"
    subtitle: reviewing ? "Review what will change" : "Resources, boot media and guest integration"
    actionText: reviewing ? (info.active ? "Save for next start" : "Save settings") : "Review changes"
    function openFor(details) {
        cpuAdvanced.expanded = false; info = JSON.parse(JSON.stringify(details)); reviewing = false; configurationPage = 0
        cpus.text = String(details.vcpus); memory.text = String(details.currentMemoryMiB)
        cpuMode.model = [details.cpuMode, "host-model", "host-passthrough"].filter(function(v, i, a) { return a.indexOf(v) === i }); cpuMode.currentIndex = 0
        boot.currentIndex = 0
        const cd = details.disks.find(function(d) { return d.device === "cdrom" })
        iso.text = cd ? cd.source : ""; isoChanged = false; clipboard.checked = !!details.clipboardConfigured
        open()
    }
    function values() {
        let result = {uuid: info.uuid, revision: info.revision, cpus: Number(cpus.text), memoryMiB: Number(memory.text), cpuMode: cpuMode.currentText}
        if (boot.currentIndex > 0) result.boot = ["", "hd", "cdrom,hd", "hd,cdrom", "network,hd"][boot.currentIndex]
        if (isoChanged) result.iso = iso.text
        if (clipboard.checked !== !!info.clipboardConfigured) result.clipboard = clipboard.checked
        return result
    }
    onSubmitted: { if (!reviewing) reviewing = true; else execute("hardware.save", values()) }
    Label { textFormat: Text.PlainText; text: dialog.info.name || ""; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    RowLayout {
        visible: !dialog.reviewing; Layout.fillWidth: true; spacing: 4
        Repeater {
            model: ["Compute", "Boot & media", "Integration"]
            AppButton { required property int index; required property string modelData
                objectName: "hardwareSection" + index
                text: modelData; tone: "tab"; checked: dialog.configurationPage === index
                onClicked: dialog.configurationPage = index
            }
        }
        Item { Layout.fillWidth: true }
    }
    ColumnLayout {
        visible: !dialog.reviewing && dialog.configurationPage === 0; Layout.fillWidth: true; spacing: 12
        SectionHeading { title: "Compute"; iconName: "cpu" }
        Label { text: "Processors" }
        AppField { id: cpus; objectName: "hardwareCpus"; Layout.fillWidth: true; validator: IntValidator { bottom: 1; top: 256 } }
        Label { text: "Memory (MiB)" }
        AppField { id: memory; objectName: "hardwareMemory"; Layout.fillWidth: true; validator: IntValidator { bottom: 256; top: 1048576 } }
        AppDisclosure {
            id: cpuAdvanced; objectName: "cpuAdvanced"; title: "Advanced CPU settings"
            Label { text: "CPU mode" }
            AppSelect { id: cpuMode; objectName: "hardwareCpuMode"; Layout.fillWidth: true }
        }
    }
    ColumnLayout {
        visible: !dialog.reviewing && dialog.configurationPage === 1; Layout.fillWidth: true; spacing: 12
        SectionHeading { title: "Boot & installation"; iconName: "disk" }
        Label { text: "Boot order" }
        AppSelect { id: boot; Layout.fillWidth: true; model: ["Keep existing boot order", "Disk", "ISO, then disk", "Disk, then ISO", "Network, then disk"] }
        Label { text: "Installation ISO" }
        RowLayout { Layout.fillWidth: true
            AppField { id: iso; Layout.fillWidth: true; placeholderText: "No media inserted"; onTextEdited: dialog.isoChanged = true }
            AppButton { text: "Browse…"; onClicked: picker.open() }
            AppButton { text: "Eject"; onClicked: { iso.text = ""; dialog.isoChanged = true } }
        }
    }
    ColumnLayout {
        visible: !dialog.reviewing && dialog.configurationPage === 2; Layout.fillWidth: true; spacing: 12
        SectionHeading { title: "Guest integration"; iconName: "clipboard" }
        AppCheckBox { id: clipboard; text: "Enable guest clipboard channel" }
        Label { text: "The guest also needs spice-vdagent."; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
    }
    ColumnLayout {
        visible: dialog.reviewing; Layout.fillWidth: true; spacing: 8
        DetailRow { Layout.fillWidth: true; label: "Processors"; value: dialog.info.vcpus + " → " + cpus.text }
        DetailRow { Layout.fillWidth: true; label: "Memory"; value: dialog.info.currentMemoryMiB + " → " + memory.text + " MiB" }
        DetailRow { visible: cpuMode.currentText !== dialog.info.cpuMode; Layout.fillWidth: true; label: "CPU mode"; value: cpuMode.currentText }
        DetailRow { Layout.fillWidth: true; label: "Boot order"; value: boot.currentText }
        DetailRow { Layout.fillWidth: true; label: "ISO"; value: dialog.isoChanged ? iso.text || "Eject media" : "Keep current media" }
        DetailRow { Layout.fillWidth: true; label: "Clipboard"; value: clipboard.checked ? "Guest channel enabled" : "Guest channel disabled" }
        AppButton { text: "Back to settings"; onClicked: dialog.reviewing = false }
    }
    Label { text: dialog.info.active ? "The running VM keeps its current hardware. Changes apply after a full shutdown and start and can be reviewed or discarded from Pending changes." : "These settings will be used at the VM's next start."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    FileDialog { id: picker; title: "Choose installation ISO"; nameFilters: ["ISO images (*.iso)", "All files (*)"]; onAccepted: { iso.text = dialog.workspace.localPath(selectedFile.toString()); dialog.isoChanged = true } }
}
