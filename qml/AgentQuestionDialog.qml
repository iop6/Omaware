// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A question an AI agent asks the user (see AgentBridge::ask), with an optional
// "don't ask again" for the kind of change it is about.
AppDialog {
    id: agentQuestion
    objectName: "agentQuestion"
    readonly property var question: agent.confirmation
    width: Math.min(520, root.width - 40)
    heading: question.title || ""
    subtitle: "Asked by an AI agent"
    headerIcon: "info"
    closePolicy: Popup.CloseOnEscape
    onQuestionChanged: {
        grantBox.checked = false;
        if (question.id)
            open();
        else
            close();
    }
    onRejected: if (question.id)
        agent.answer(question.id, false)
    contentItem: ColumnLayout {
        spacing: 14
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: agentQuestion.question.text || ""
        }
        // Offered only for low-risk kinds of change; off unless ticked for this question.
        AppCheckBox {
            id: grantBox
            objectName: "agentQuestionGrant"
            visible: !!agentQuestion.question.grant
            Layout.fillWidth: true
            text: agentQuestion.question.grantLabel || ""
        }
        RowLayout {
            spacing: 8
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                objectName: "agentQuestionNo"
                text: "No"
                onClicked: agent.answer(agentQuestion.question.id, false)
            }
            AppButton {
                objectName: "agentQuestionYes"
                text: agentQuestion.question.action || "Yes"
                tone: "danger"
                onClicked: agent.answer(agentQuestion.question.id, true, grantBox.visible && grantBox.checked)
            }
        }
    }
}
