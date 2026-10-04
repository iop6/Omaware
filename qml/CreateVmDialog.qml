// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

EditorDialog {
    id: dialog
    objectName: "createVmDialog"
    property var workspace
    property var isoLibrary: null
    signal getIsos
    property bool reviewing: false
    property string selectedNetwork: "user"
    property string isoFolder: ""
    property bool scanning: false
    property var mediaResponse: backend.management["media.list"] || ({})
    readonly property var library: mediaResponse.folder === isoFolder ? mediaResponse : ({})
    readonly property var images: (library.items || []).concat(isoLibrary ? isoLibrary.files.filter(function (f) {
        return f.type === "disk";
    }) : [])
    property var caps: backend.management["capabilities"] || ({})
    property var networks: [
        {
            id: "none",
            label: "No network",
            available: true
        },
        {
            id: "user",
            label: "Per-VM NAT · internet access",
            available: true
        }
    ].concat((caps.networks || []).filter(function (n) {
        return n.id !== "user";
    }))
    // An OS picked from a shop ISO that isn't in the short list is added to it.
    property var extraPresets: []
    property var presets: (caps.presets || [
            {
                id: "generic",
                label: "Generic OS"
            }
        ]).concat(extraPresets)
    // The closest available OS preset for a downloaded ISO, e.g. Ubuntu 24.04.3 → "ubuntu24.04".
    function presetFor(path) {
        if (!isoLibrary)
            return "";
        const info = isoLibrary.identify(String(path).split("/").pop());
        const wanted = info.preset || "";
        if (!wanted)
            return "";
        if (presets.some(function (p) {
            return p.id === wanted;
        }))
            return wanted;
        if ((caps.osinfo || []).indexOf(wanted) < 0)
            return "";
        extraPresets = [
            {
                id: wanted,
                label: info.sourceName + " " + String(info.version || "").split(".").slice(0, 2).join(".")
            }
        ];
        return wanted;
    }
    // Set when an ISO was chosen before this computer's OS list arrived.
    property string pendingPresetPath: ""
    onCapsChanged: if (pendingPresetPath !== "" && caps.presets) {
        const path = pendingPresetPath;
        pendingPresetPath = "";
        pickPresetFor(path);
    }
    function pickPresetFor(path) {
        if (!isoLibrary)
            return;
        const info = isoLibrary.identify(String(path).split("/").pop());
        // Suggest a name like "Debian 13.1" or "Rocky Linux 10.2" (VM names allow letters, digits, spaces, dots, - and _).
        if (name.text.trim() === "" && info.sourceName)
            name.text = (info.sourceName + " " + String(info.version || "").split(".").slice(0, 2).join(".")).replace(/[^A-Za-z0-9 ._-]/g, "").replace(/ +/g, " ").trim().slice(0, 48);
        if (!caps.presets) {
            pendingPresetPath = path;
            return;
        }
        const id = presetFor(path);
        if (!id)
            return;
        Qt.callLater(function () {
            preset.currentIndex = Math.max(0, dialog.presets.findIndex(function (p) {
                return p.id === id;
            }));
            dialog.applyPreset();
        });
    }
    property string selectedPreset: "generic"
    // Windows: a TPM, which Windows 11 requires.
    readonly property bool windowsPreset: selectedPreset.indexOf("win") === 0
    readonly property bool needsTpm: selectedPreset === "win11"
    // "Set it up for me": the chosen ISO's installer can run without questions ("windows" or "subiquity").
    readonly property var mediaInfo: isoLibrary && sourceMode.currentIndex === 0 && source.text !== "" ? isoLibrary.identify(source.text.split("/").pop()) : ({})
    readonly property string setupKind: mediaInfo.setup || ""
    readonly property bool settingUp: setupKind !== "" && unattended.checked
    readonly property bool setupValid: !settingUp || (/^[a-z_][a-z0-9_-]{0,31}$/.test(setupUser.text) && setupPassword.text.length > 0)
    onPresetsChanged: Qt.callLater(function () {
        preset.currentIndex = Math.max(0, dialog.presets.findIndex(function (p) {
            return p.id === dialog.selectedPreset;
        }));
    })
    onNetworksChanged: Qt.callLater(function () {
        network.currentIndex = Math.max(0, dialog.networks.findIndex(function (n) {
            return n.id === dialog.selectedNetwork;
        }));
    })
    heading: reviewing ? "Review your new VM" : "Create a virtual machine"
    headerIcon: "plus"
    subtitle: reviewing ? "Check the settings, then create" : "Choose your system. Fine-tune whenever you need."
    actionText: reviewing ? "Create VM" : "Review configuration"
    ready: name.text.trim() !== "" && source.text !== "" && !!caps.virtInstall && setupValid
    function scanLibrary() {
        if (isoFolder)
            scanning = backend.request("media.list", {
                folder: isoFolder
            });
    }
    function setIsoFolder(folder) {
        isoFolder = folder;
        workspace.set("isoFolder", folder);
        scanLibrary();
    }
    function begin() {
        createAdvanced.expanded = false;
        createPaths.expanded = false;
        reviewing = false;
        failure = "";
        extraPresets = [];
        pendingPresetPath = "";
        name.text = "";
        unattended.checked = true;
        setupPassword.text = "";
        showPassword.checked = false;
        // ISOs live in OmaWare's own folder unless you chose another one.
        isoFolder = String(workspace.get("isoFolder", "")) || (isoLibrary ? isoLibrary.folder : "");
        backend.request("capabilities", {});
        scanLibrary();
        open();
    }
    // Opens the dialog with an ISO already chosen (from Get ISOs).
    function beginWith(path) {
        begin();
        useMedia(path);
        Qt.callLater(function () {
            dialog.pickPresetFor(path);
        });
    }
    // Uses an ISO in the open dialog (for example one dropped onto the window).
    function useMedia(path, type) {
        sourceMode.currentIndex = /\.iso$/i.test(String(path)) ? 0 : type === "disk" || /\.(qcow2|ova|raw|img)$/i.test(String(path)) ? 1 : 0;
        source.text = path;
        scanLibrary();
        pickPresetFor(path);
    }
    function useIso(path) {
        useMedia(path);
    }
    Connections {
        target: dialog.isoLibrary
        function onFinished(id, ok, message) {
            if (ok && dialog.visible)
                dialog.scanLibrary();
        }
    }
    function chooseMedia(index) {
        if (index >= 0 && index < images.length)
            useMedia(images[index].path, images[index].type);
    }
    function applyPreset() {
        selectedPreset = (presets[preset.currentIndex] || {}).id || "generic";
        const windows = String((presets[preset.currentIndex] || {}).id).indexOf("win") === 0;
        cpus.text = windows ? "4" : "2";
        memory.text = windows ? "8192" : "4096";
        disk.text = windows ? "64" : "32";
        // Windows 11 only installs on UEFI firmware with a TPM.
        if (selectedPreset === "win11")
            firmware.currentIndex = 1;
        tpm.checked = selectedPreset === "win11" && !!caps.tpm;
    }
    function recoverFailure(action) {
        reviewing = false;
        if (action === "source")
            sourcePicker.open();
        else if (action === "storage") {
            createAdvanced.expanded = true;
            directoryPicker.open();
        } else if (action === "networks" || action === "connection") {
            close();
            backend.showRecovery(action, "");
        } else {
            createAdvanced.expanded = true;
            resetScroll();
        }
    }
    Connections {
        target: backend
        function onCommandFinished(op, ok, result) {
            if (op === "media.list")
                dialog.scanning = false;
        }
    }
    onSubmitted: {
        if (!reviewing) {
            reviewing = true;
            return;
        }
        let input = {
            name: name.text,
            sourceMode: sourceMode.currentIndex === 0 ? "iso" : "disk",
            source: source.text,
            preset: (presets[preset.currentIndex] || {}).id || "generic",
            cpus: Number(cpus.text),
            memoryMiB: Number(memory.text),
            diskGiB: Number(disk.text),
            location: location.text || caps.storage,
            firmware: firmware.currentIndex === 0 ? "bios" : "uefi",
            networkId: networks[network.currentIndex].id,
            tpm: windowsPreset && tpm.checked && !!caps.tpm
        };
        if (settingUp)
            input.unattended = {
                user: setupUser.text,
                password: setupPassword.text
            };
        execute("vm.create", input);
    }
    ColumnLayout {
        visible: !dialog.reviewing
        Layout.fillWidth: true
        spacing: 10
        Label {
            text: "VM name"
        }
        AppField {
            id: name
            objectName: "newVmName"
            Accessible.name: "Virtual machine name"
            placeholderText: "Ubuntu development"
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            AppSelect {
                id: sourceMode
                objectName: "newVmSourceMode"
                Accessible.name: "Installation source type"
                Layout.fillWidth: true
                model: ["Install from an ISO", "Copy an existing disk image"]
                onActivated: source.text = ""
            }
            AppButton {
                text: "Browse file…"
                objectName: "browseMedia"
                onClicked: sourcePicker.open()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            AppSelect {
                id: mediaPicker
                objectName: "isoLibraryPicker"
                Accessible.name: "ISO library"
                Layout.fillWidth: true
                model: dialog.images
                textRole: "name"
                currentIndex: -1
                displayText: currentIndex >= 0 ? currentText : dialog.scanning ? "Reading ISO library…" : dialog.images.length ? "Choose an ISO" : "No ISOs yet"
                enabled: dialog.images.length > 0 && !dialog.scanning
                onActivated: dialog.chooseMedia(currentIndex)
                // Shows the chosen file whichever way it was chosen (list, Browse, OS Shop or a drop).
                function sync() {
                    let at = -1;
                    for (let i = 0; i < dialog.images.length; ++i)
                        if (dialog.images[i].path === source.text)
                            at = i;
                    currentIndex = at;
                }
                Connections {
                    target: source
                    function onTextChanged() {
                        mediaPicker.sync();
                    }
                }
                Connections {
                    target: dialog
                    function onImagesChanged() {
                        mediaPicker.sync();
                    }
                }
            }
            AppButton {
                objectName: "chooseIsoFolder"
                iconName: "folder"
                hint: dialog.isoFolder ? "Change ISO folder: " + dialog.isoFolder : "Choose ISO folder"
                onClicked: libraryPicker.open()
            }
            AppButton {
                visible: dialog.isoFolder !== ""
                iconName: "refresh"
                hint: "Refresh ISO library"
                enabled: !dialog.scanning
                onClicked: dialog.scanLibrary()
            }
            AppButton {
                objectName: "getIsos"
                text: "OS Shop"
                iconName: "store"
                hint: "Download Ubuntu, Fedora, Debian, Kali, Windows and more"
                onClicked: dialog.getIsos()
            }
        }
        Label {
            textFormat: Text.PlainText
            visible: sourceMode.currentIndex === 0 && dialog.isoFolder !== "" && !dialog.scanning && (dialog.images.length === 0 || !!dialog.library.error || !!dialog.library.notice)
            text: dialog.library.error || dialog.library.notice || "No ISOs yet. Get one from the OS Shop, or browse to a file."
            color: dialog.library.error ? theme.colors.warning : theme.colors.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Math.round((12) * theme.textScale)
        }
        AppField {
            id: source
            objectName: "newVmSource"
            Accessible.name: sourceMode.currentIndex === 0 ? "ISO path" : "Disk image path"
            placeholderText: sourceMode.currentIndex === 0 ? "Choose an ISO above, or paste its path" : "Choose an OVA, raw or qcow2 image, or paste its path"
            Layout.fillWidth: true
        }
        Label {
            text: "Operating system"
        }
        AppSelect {
            id: preset
            objectName: "newVmPreset"
            Accessible.name: "Operating system"
            Layout.fillWidth: true
            model: dialog.presets
            textRole: "label"
            onActivated: dialog.applyPreset()
        }
        ColumnLayout {
            objectName: "windowsOptions"
            visible: dialog.windowsPreset
            Layout.fillWidth: true
            spacing: 6
            AppCheckBox {
                id: tpm
                objectName: "newVmTpm"
                text: "Add a TPM 2.0 chip"
                enabled: !!dialog.caps.tpm
                Layout.fillWidth: true
            }
            Label {
                objectName: "tpmNote"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Math.round(11 * theme.textScale)
                color: dialog.needsTpm && !dialog.caps.tpm ? theme.colors.warning : theme.colors.muted
                textFormat: Text.PlainText
                text: !dialog.caps.tpm ? (dialog.needsTpm ? "Windows 11 needs a TPM, and this computer can't provide one yet: install the swtpm package, then reopen this window." : "Install the swtpm package to give VMs a TPM.") : dialog.needsTpm ? "Windows 11 needs it. Snapshots keep its contents too." : "Optional for this version of Windows."
            }
        }
        // Set it up for me.
        Rectangle {
            objectName: "setupOptions"
            visible: dialog.setupKind !== ""
            Layout.fillWidth: true
            implicitHeight: setupColumn.implicitHeight + 24
            radius: 10
            color: Qt.rgba(theme.colors.accent.r, theme.colors.accent.g, theme.colors.accent.b, .07)
            border.color: Qt.rgba(theme.colors.accent.r, theme.colors.accent.g, theme.colors.accent.b, .35)
            ColumnLayout {
                id: setupColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 8
                AppCheckBox {
                    id: unattended
                    objectName: "newVmUnattended"
                    text: "Set it up for me"
                    checked: true
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "setupNote"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: theme.colors.muted
                    font.pixelSize: Math.round(12 * theme.textScale)
                    textFormat: Text.PlainText
                    text: !unattended.checked ? "The installer asks its usual questions; answer them on the VM's screen." : dialog.setupKind === "windows" ? "Windows installs by itself, with this account as an administrator (no Microsoft account needed). It takes 20–40 minutes and restarts a few times." + (dialog.mediaInfo.source === "windows-11" ? " Windows 11 Pro is installed without a product key: activate it with your own license." : "") + (dialog.mediaInfo.source === "windows-server" ? " Windows Server's Administrator gets the same password, which must be complex (upper and lower case, digits). Sign in before shutting it down: Windows Server ignores Shut down at its sign-in screen." : "") + " When you first shut it down afterwards, OmaWare takes out the installation media." : "Installs Ubuntu Server with this account as an administrator (sudo), OpenSSH and the QEMU guest agent. About 10 minutes; then OmaWare takes out the installation media and starts the new system."
                }
                GridLayout {
                    visible: unattended.checked
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 6
                    Layout.fillWidth: true
                    Label {
                        text: "User name"
                    }
                    AppField {
                        id: setupUser
                        objectName: "newVmSetupUser"
                        Accessible.name: "User name for the new system"
                        placeholderText: "e.g. alex"
                        Layout.fillWidth: true
                        validator: RegularExpressionValidator {
                            regularExpression: /[a-z_][a-z0-9_-]{0,31}/
                        }
                    }
                    Label {
                        text: "Password"
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        AppField {
                            id: setupPassword
                            objectName: "newVmSetupPassword"
                            Accessible.name: "Password for the new system"
                            echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password
                            maximumLength: 127
                            Layout.fillWidth: true
                        }
                        AppButton {
                            id: showPassword
                            checkable: true
                            text: checked ? "Hide" : "Show"
                            tone: "quiet"
                        }
                    }
                }
                Label {
                    visible: unattended.checked
                    text: "The password goes into the answers for the installer, kept in the VM's private folder until setup is done. OmaWare doesn't save it anywhere else."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }
        Label {
            textFormat: Text.PlainText
            text: cpus.text + " processors · " + Number(Number(memory.text) / 1024).toFixed(1) + " GiB memory · " + (sourceMode.currentIndex === 0 ? disk.text + " GiB disk" : "Source disk capacity")
            color: theme.colors.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Math.round((12) * theme.textScale)
        }
        AppDisclosure {
            id: createAdvanced
            objectName: "createAdvanced"
            title: "Advanced setup"
            GridLayout {
                columns: 3
                Layout.fillWidth: true
                columnSpacing: 12
                Label {
                    text: "Processors"
                }
                Label {
                    text: "RAM (MiB)"
                }
                Label {
                    text: "Disk (GiB)"
                }
                AppField {
                    id: cpus
                    objectName: "newVmCpus"
                    Accessible.name: "Processors"
                    text: "2"
                    validator: IntValidator {
                        bottom: 1
                        top: 256
                    }
                    Layout.fillWidth: true
                }
                AppField {
                    id: memory
                    objectName: "newVmMemory"
                    Accessible.name: "Memory in MiB"
                    text: "4096"
                    validator: IntValidator {
                        bottom: 256
                        top: 1048576
                    }
                    Layout.fillWidth: true
                }
                AppField {
                    id: disk
                    Accessible.name: "Disk capacity in GiB"
                    text: "32"
                    enabled: sourceMode.currentIndex === 0
                    validator: IntValidator {
                        bottom: 1
                        top: 2048
                    }
                    Layout.fillWidth: true
                }
            }
            Label {
                textFormat: Text.PlainText
                text: "Host: " + (dialog.caps.cpus || "…") + " CPUs · " + (dialog.caps.memoryMiB ? Math.round(dialog.caps.memoryMiB / 1024) + " GiB RAM" : "Reading capacity…")
                color: theme.colors.muted
                font.pixelSize: Math.round((11) * theme.textScale)
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                text: "Network"
            }
            AppSelect {
                id: network
                objectName: "newVmNetwork"
                Accessible.name: "Network"
                Layout.fillWidth: true
                model: dialog.networks
                textRole: "label"
                onActivated: dialog.selectedNetwork = dialog.networks[currentIndex].id
            }
            Label {
                textFormat: Text.PlainText
                text: (dialog.networks[network.currentIndex] || {}).reason || ""
                visible: text !== ""
                color: theme.colors.warning
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                text: "Storage location"
            }
            RowLayout {
                Layout.fillWidth: true
                AppField {
                    id: location
                    objectName: "newVmLocation"
                    Accessible.name: "Storage location"
                    placeholderText: dialog.caps.storage || "Default VM storage directory"
                    Layout.fillWidth: true
                }
                AppButton {
                    text: "Browse…"
                    onClicked: directoryPicker.open()
                }
            }
            Label {
                text: "Firmware"
            }
            AppSelect {
                id: firmware
                objectName: "newVmFirmware"
                Accessible.name: "Firmware"
                Layout.fillWidth: true
                model: ["BIOS", "UEFI (requires installed host firmware)"]
            }
        }
    }
    ColumnLayout {
        visible: dialog.reviewing
        Layout.fillWidth: true
        spacing: 10
        DetailRow {
            label: "Name"
            value: name.text
            Layout.fillWidth: true
        }
        DetailRow {
            label: "Source"
            value: source.text.split("/").pop()
            Layout.fillWidth: true
        }
        DetailRow {
            label: "Operating system"
            value: preset.currentText
            Layout.fillWidth: true
        }
        DetailRow {
            label: "Compute"
            value: cpus.text + " CPUs · " + memory.text + " MiB RAM"
            Layout.fillWidth: true
        }
        DetailRow {
            label: "Storage"
            value: sourceMode.currentIndex === 0 ? "New " + disk.text + " GiB qcow2 disk" : "Independent copy of the source disk"
            Layout.fillWidth: true
        }
        DetailRow {
            label: "Firmware"
            value: firmware.currentText + (dialog.windowsPreset && firmware.currentIndex === 1 ? " · Secure Boot" : "")
            Layout.fillWidth: true
        }
        DetailRow {
            visible: dialog.windowsPreset
            label: "Windows extras"
            value: tpm.checked && dialog.caps.tpm ? "TPM 2.0" : "No TPM"
            Layout.fillWidth: true
        }
        DetailRow {
            label: "Network"
            value: network.currentText
            Layout.fillWidth: true
        }
        DetailRow {
            objectName: "reviewSetup"
            visible: dialog.setupKind !== ""
            label: "Setup"
            value: dialog.settingUp ? "Unattended, as " + setupUser.text : "You answer the installer's questions"
            Layout.fillWidth: true
        }
        AppDisclosure {
            id: createPaths
            objectName: "createPaths"
            title: "File locations"
            DetailRow {
                label: "Source"
                value: source.text
                Layout.fillWidth: true
            }
            DetailRow {
                label: "Destination"
                value: location.text || dialog.caps.storage || ""
                Layout.fillWidth: true
            }
        }
        Label {
            textFormat: Text.PlainText
            text: dialog.settingUp ? "The VM starts stopped. Start it and the installer runs by itself." : "The VM starts stopped. Imported disks are copied."
            color: theme.colors.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        AppButton {
            objectName: "backToSetup"
            text: "Back to settings"
            onClicked: dialog.reviewing = false
        }
    }
    Label {
        visible: dialog.caps.virtInstall === false
        text: "Creation needs virt-install and libosinfo on this host. Install these dependencies, then reopen the wizard."
        color: theme.colors.warning
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
    FileDialog {
        id: sourcePicker
        title: "Choose installation media or appliance"
        nameFilters: ["Supported media (*.iso *.ISO *.ova *.OVA *.qcow2 *.QCOW2 *.raw *.img)", "All files (*)"]
        onAccepted: dialog.useMedia(dialog.workspace.localPath(selectedFile.toString()), sourceMode.currentIndex === 1 ? "disk" : "")
    }
    FolderDialog {
        id: libraryPicker
        title: "Choose your ISO library folder"
        onAccepted: dialog.setIsoFolder(dialog.workspace.localPath(selectedFolder.toString()))
    }
    FolderDialog {
        id: directoryPicker
        title: "Choose VM storage directory"
        onAccepted: location.text = dialog.workspace.localPath(selectedFolder.toString())
    }
}
