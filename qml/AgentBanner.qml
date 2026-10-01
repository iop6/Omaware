// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shows when an AI agent is working: using a VM's screen (for 20 seconds after its last action) or building
// a lab. Stop turns agent access off at once.
Rectangle {
    id: banner
    objectName: "agentBanner"
    signal showVm(string uuid)
    signal showLab()
    property double now: Date.now()
    readonly property bool usingScreen: agent.enabled && !!agent.screen.uuid && now - agent.screen.at < 20000
    readonly property bool building: agent.build.state === "building"
    visible: usingScreen || building
    implicitHeight: visible ? row.implicitHeight + 14 : 0
    color: theme.colors.accentSoft
    border.color: theme.colors.accent
    radius: 3
    Timer { interval: 1000; repeat: true; running: !!agent.screen.uuid || banner.building; onTriggered: banner.now = Date.now() }
    RowLayout {
        id: row
        anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.margins: 10
        spacing: 10
        AppIcon { name: banner.building ? "network" : "monitor"; color: theme.colors.accent; width: 18; height: 18 }
        Label {
            objectName: "agentBannerText"
            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            font.pixelSize: Math.round(12 * theme.textScale)
            text: banner.building ? "An AI agent's lab “" + agent.build.name + "” is being built: " + (agent.build.message || "")
                : "An AI agent is using " + agent.screen.name + "'s screen."
        }
        AppButton { visible: banner.usingScreen; text: "Watch"; tone: "quiet"; implicitHeight: 28; onClicked: banner.showVm(agent.screen.uuid) }
        AppButton { visible: banner.building; text: "Details"; tone: "quiet"; implicitHeight: 28; onClicked: banner.showLab() }
        AppButton { objectName: "stopAgent"; visible: banner.usingScreen; text: "Stop agent"; tone: "danger"; implicitHeight: 28; hint: "Turns AI agent access off"; onClicked: agent.stop() }
    }
}
