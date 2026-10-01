import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A titled card; child items are stacked below the title.
Pane {
    id: card

    property string title
    property string subtitle
    default property alias content: column.data

    readonly property bool dark: Material.theme === Material.Dark

    Layout.fillWidth: true
    padding: 16

    background: Rectangle {
        radius: 12
        color: card.dark ? "#142028" : "#ffffff"
        border.color: card.dark ? "#1f2d35" : "#d9e4e8"
    }

    ColumnLayout {
        id: column

        width: card.availableWidth
        spacing: 6

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: card.title
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            text: card.subtitle
            color: card.dark ? "#9aa8af" : "#5d6b72"
        }
    }
}
