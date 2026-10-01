// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omaware 1.0
ColumnLayout {
    id: notice
    property string message: ""
    property var advice: backend.errorAdvice(message)
    property bool recoverable: true
    property string actionLabel: advice.label || "Review settings"
    signal recover(string action)
    Layout.fillWidth: true
    visible: message !== ""
    spacing: 8
    RowLayout { Layout.fillWidth: true
        AppIcon { name: "info"; color: theme.colors.warning }
        Label { textFormat: Text.PlainText; text: notice.advice.summary || notice.message; color: theme.colors.foreground; Layout.fillWidth: true; wrapMode: Text.Wrap; font.weight: Font.DemiBold }
    }
    Flow { Layout.fillWidth: true; spacing: 8
        AppButton { objectName: "errorRecovery"; visible: notice.recoverable; text: notice.actionLabel; onClicked: notice.recover(notice.advice.action || "review") }
        AppButton { text: "Copy error"; tone: "quiet"; onClicked: copyHelper.copy(notice.message) }
    }
    AppDisclosure {
        objectName: "errorDetails"
        title: "Error details"; resetKey: notice.message
        Label { textFormat: Text.PlainText; text: notice.advice.nextStep || ""; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
        ScrollView { Layout.fillWidth: true; Layout.preferredHeight: 110; clip: true; contentWidth: availableWidth
            AppTextArea { text: notice.message; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: theme.colors.muted }
        }
    }
    // Copying uses the same application-owned clipboard boundary as Activity.
    Workspace { id: clipboardWorkspace }
    property var copyHelper: clipboardWorkspace
}
