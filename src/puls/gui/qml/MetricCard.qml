import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A compact metric: title, value and unit.
Pane {
    id: card

    property string title
    property string value
    property string unit

    readonly property bool dark: Material.theme === Material.Dark

    Layout.fillWidth: true
    padding: 12

    background: Rectangle {
        radius: 12
        color: card.dark ? "#142028" : "#ffffff"
        border.color: card.dark ? "#1f2d35" : "#d9e4e8"
    }

    ColumnLayout {
        width: card.availableWidth
        spacing: 2

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: card.title
            color: card.dark ? "#9aa8af" : "#5d6b72"
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: card.value
            font.pixelSize: 24
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: card.unit
            color: card.dark ? "#9aa8af" : "#5d6b72"
        }
    }
}
