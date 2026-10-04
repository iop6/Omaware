// SPDX-License-Identifier: GPL-3.0-or-later
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right-click menu on the network map's empty canvas: a new network there, layout, export and import,
// and cutting off the internet.
AppMenu {
    id: canvasMenu
    objectName: "topologyCanvasMenu"
    property var at: ({
            x: 0,
            y: 0
        })
    AppMenuItem {
        text: "New network here…"
        onTriggered: {
            topo.placement = canvasMenu.at;
            topo.createNetwork("nat", []);
        }
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        text: "Tidy up"
        onTriggered: topo.arrange()
    }
    AppMenuItem {
        text: "Fit to view"
        onTriggered: topo.refit()
    }
    AppMenuItem {
        text: "Export the map…"
        onTriggered: exportMenu.popup()
    }
    AppMenuItem {
        text: topo.operations ? "Standard map style" : "Operations-center style"
        onTriggered: topo.setOperations(!topo.operations)
    }
    MenuSeparator {
        contentItem: Rectangle {
            implicitHeight: 1
            color: theme.colors.border
        }
    }
    AppMenuItem {
        objectName: "importLabFile"
        text: "Import lab file…"
        enabled: !!topo.labs
        onTriggered: labOpenDialog.open()
    }
    AppMenu {
        id: labExportMenu
        title: "Export a lab"
        enabled: !!topo.labs && topo.labs.labs.length > 0
        Instantiator {
            model: topo.labs ? topo.labs.labs : []
            AppMenuItem {
                required property var modelData
                text: modelData.name
                onTriggered: {
                    labSaveDialog.slug = modelData.slug;
                    labSaveDialog.open();
                }
            }
            onObjectAdded: function (index, object) {
                labExportMenu.insertItem(index, object);
            }
            onObjectRemoved: function (index, object) {
                labExportMenu.removeItem(object);
            }
        }
    }
    AppMenuItem {
        text: "Cut off internet for every VM"
        onTriggered: topo.killInternet()
    }
}
