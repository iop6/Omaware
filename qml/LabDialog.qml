// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A lab an AI agent proposed: what it would build, the login for its VMs, and then the build's progress.
// Nothing is created until Build is clicked; the password is typed here, never given to the agent.
AppDialog {
    id: dialog
    objectName: "labDialog"
    property var copyHelper: null
    readonly property var proposal: agent.proposal
    readonly property var plan: proposal.plan || ({})
    readonly property var build: agent.build
    readonly property bool proposing: !!proposal.id
    readonly property bool building: build.state === "building"
    readonly property bool useSaved: savedChoice.checked && agent.logins.length > 0
    width: Math.min(640, parent ? parent.width - 40 : 640)
    height: Math.min(implicitHeight, parent ? parent.height - 40 : 760)
    headerIcon: "network"
    closePolicy: Popup.CloseOnEscape
    heading: proposing ? "Build “" + plan.name + "”?" : build.state === "ready" ? "“" + build.name + "” is ready" : build.state === "failed" ? "Building “" + build.name + "” stopped" : building ? "Building “" + build.name + "”" : "Lab"
    subtitle: proposing ? "An AI agent planned this lab. Nothing is created until you click Build." : ""
    onProposingChanged: if (proposing) {
        reset();
        open();
    }
    function reset() {
        user.text = proposal.user || "";
        password.text = "";
        reveal.checked = false;
        loginName.text = proposal.login || "";
        newChoice.checked = true;
    }
    function canBuild() {
        if (useSaved)
            return savedLogin.currentIndex >= 0;
        return /^[a-z_][a-z0-9_-]{0,31}$/.test(user.text) && password.text.length >= 8 && loginName.text.trim() !== "";
    }
    function networkLine(n) {
        const kinds = {
            internet: "Internet · VMs reach each other, this computer and the internet",
            private: "Private · VMs reach each other and this computer",
            isolated: "Isolated · VMs only reach each other"
        };
        return kinds[n.type] + (n.subnet ? " · " + n.subnet : " · addresses chosen automatically");
    }
    function vmLine(v) {
        const nets = (v.nics || []).map(function (n) {
            return n.network + (n.ip ? " (" + n.ip + ")" : "");
        }).join(", ");
        return v.os + " · " + v.cpus + " CPU · " + (v.memoryMiB >= 1024 ? (v.memoryMiB / 1024) + " GiB" : v.memoryMiB + " MiB") + " · " + v.diskGiB + " GiB disk" + (nets ? " · on " + nets : " · no network");
    }

    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth
        implicitHeight: Math.min(body.implicitHeight, 600)
        ColumnLayout {
            id: body
            width: scroller.availableWidth
            spacing: 12

            // ---- The plan ----
            ColumnLayout {
                visible: dialog.proposing
                Layout.fillWidth: true
                spacing: 8
                SectionHeading {
                    title: "Networks"
                    iconName: "switch"
                    visible: (dialog.plan.networks || []).length > 0
                }
                Repeater {
                    model: dialog.plan.networks || []
                    DetailRow {
                        required property var modelData
                        Layout.fillWidth: true
                        label: modelData.name
                        value: dialog.networkLine(modelData)
                    }
                }
                SectionHeading {
                    title: "VMs"
                    iconName: "monitor"
                }
                Repeater {
                    model: dialog.plan.vms || []
                    ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2
                        DetailRow {
                            Layout.fillWidth: true
                            label: modelData.name
                            value: dialog.vmLine(modelData)
                        }
                        Label {
                            visible: (modelData.packages || []).length > 0
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            text: "Installs: " + (modelData.packages || []).join(", ")
                            color: theme.colors.muted
                            font.pixelSize: Math.round(11 * theme.textScale)
                        }
                        // Commands that run as root inside the VM on first boot, shown in full.
                        Label {
                            visible: (modelData.setup || []).length > 0
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            wrapMode: Text.WrapAnywhere
                            textFormat: Text.PlainText
                            text: "Runs inside the VM on first boot:\n" + (modelData.setup || []).map(function (c) {
                                return "$ " + c;
                            }).join("\n")
                            color: theme.colors.muted
                            font.family: "monospace"
                            font.pixelSize: Math.round(11 * theme.textScale)
                        }
                    }
                }
                Repeater {
                    model: dialog.proposal.warnings || []
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: "⚠ " + modelData
                        color: theme.colors.warning
                        font.pixelSize: Math.round(12 * theme.textScale)
                    }
                }
                Label {
                    readonly property var missing: (dialog.proposal.images || []).filter(function (i) {
                        return !i.ready;
                    }).map(function (i) {
                        return i.os;
                    })
                    visible: missing.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: "Downloads the " + missing.join(" and ") + " cloud image" + (missing.length > 1 ? "s" : "") + " first (a few hundred MB each, once)."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(12 * theme.textScale)
                }

                // ---- Login ----
                SectionHeading {
                    title: "Login for these VMs"
                    caption: "The agent never sees this password"
                    iconName: "keyboard"
                }
                RowLayout {
                    visible: agent.logins.length > 0
                    spacing: 16
                    AppRadioButton {
                        id: newChoice
                        text: "New login"
                        checked: true
                    }
                    AppRadioButton {
                        id: savedChoice
                        objectName: "useSavedLogin"
                        text: "Use a saved login"
                    }
                }
                GridLayout {
                    visible: !dialog.useSaved
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 8
                    Label {
                        text: "User name"
                    }
                    AppField {
                        id: user
                        objectName: "labUser"
                        Layout.fillWidth: true
                        placeholderText: "for example alex"
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
                            id: password
                            objectName: "labPassword"
                            Layout.fillWidth: true
                            echoMode: reveal.checked ? TextInput.Normal : TextInput.Password
                            placeholderText: "at least 8 characters"
                        }
                        AppButton {
                            objectName: "generatePassword"
                            text: "Generate"
                            tone: "quiet"
                            onClicked: {
                                password.text = agent.generatePassword();
                                reveal.checked = true;
                            }
                        }
                    }
                    Item {
                        width: 1
                        height: 1
                    }
                    RowLayout {
                        spacing: 10
                        AppCheckBox {
                            id: reveal
                            text: "Show"
                        }
                        AppButton {
                            visible: password.text !== "" && !!dialog.copyHelper
                            text: "Copy"
                            tone: "quiet"
                            implicitHeight: 28
                            onClicked: dialog.copyHelper.copy(password.text)
                        }
                    }
                    Label {
                        text: "Save as"
                    }
                    AppField {
                        id: loginName
                        objectName: "labLoginName"
                        Layout.fillWidth: true
                        maximumLength: 64
                    }
                }
                AppSelect {
                    id: savedLogin
                    objectName: "savedLogin"
                    visible: dialog.useSaved
                    Layout.fillWidth: true
                    model: agent.logins.map(function (l) {
                        return l.name + " · " + l.user;
                    })
                }
                Label {
                    objectName: "labLoginError"
                    visible: dialog.build.state === "error" && dialog.build.id === dialog.proposal.id
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: dialog.build.message || ""
                    color: theme.colors.danger
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: "The password is kept in your system's password store and shown on each VM's Details page. Every VM gets this user, with administrator rights (sudo)." + ((dialog.plan.networks || []).length > 0 ? " Creating the networks asks for your computer's password once." : "")
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
            }

            // ---- Progress ----
            ColumnLayout {
                visible: !dialog.proposing && !!dialog.build.state
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: dialog.build.steps || []
                    RowLayout {
                        required property var modelData
                        required property int index
                        readonly property bool doneStep: dialog.build.state === "ready" || index < (dialog.build.step || 0)
                        readonly property bool current: dialog.building && index === dialog.build.step
                        spacing: 8
                        Label {
                            textFormat: Text.PlainText
                            text: parent.doneStep ? "✓" : parent.current ? "›" : "·"
                            color: parent.doneStep ? theme.colors.success : parent.current ? theme.colors.accent : theme.colors.muted
                            font.weight: Font.Bold
                            Layout.preferredWidth: 14
                        }
                        Label {
                            textFormat: Text.PlainText
                            text: modelData
                            color: parent.current ? theme.colors.foreground : theme.colors.muted
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }
                AppProgressBar {
                    visible: dialog.building
                    Layout.fillWidth: true
                    indeterminate: true
                }
                Label {
                    objectName: "labBuildMessage"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: dialog.build.message || ""
                    color: dialog.build.state === "failed" || dialog.build.state === "error" ? theme.colors.danger : theme.colors.muted
                }
            }

            RowLayout {
                Layout.topMargin: 4
                spacing: 8
                Item {
                    Layout.fillWidth: true
                }
                AppButton {
                    objectName: "declineLab"
                    visible: dialog.proposing
                    text: "Decline"
                    onClicked: {
                        agent.decline(dialog.proposal.id);
                        dialog.close();
                    }
                }
                AppButton {
                    objectName: "buildLab"
                    visible: dialog.proposing
                    text: "Build"
                    tone: "primary"
                    iconName: "play"
                    enabled: dialog.canBuild()
                    onClicked: {
                        const saved = dialog.useSaved ? agent.logins[savedLogin.currentIndex] : null;
                        agent.approve(dialog.proposal.id, saved ? saved.name : loginName.text.trim(), saved ? saved.user : user.text, saved ? "" : password.text, !!saved);
                        password.text = "";
                    }
                }
                AppButton {
                    visible: !dialog.proposing
                    text: dialog.building ? "Hide" : "Close"
                    onClicked: dialog.close()
                }
            }
        }
    }
}
