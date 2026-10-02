import QtQuick
import QtQuick.Layouts

// A titled detail with an icon; an action may follow it.
RowLayout {
    id: row

    property string iconPath
    property string title
    property string value
    property bool muted: false
    default property alias trailing: trailingItems.data

    spacing: Theme.spacingMedium

    Rectangle {
        Layout.alignment: Qt.AlignTop
        implicitWidth: 34
        implicitHeight: 34
        radius: Theme.radiusSmall
        color: Theme.surfaceMuted

        Icon {
            anchors.centerIn: parent
            path: row.iconPath
            size: 17
            color: Theme.textSecondary
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2

        Label {
            Layout.fillWidth: true
            text: row.title
            font.pixelSize: Theme.textCaption
            font.weight: Font.Medium
            color: Theme.textSecondary
        }
        Label {
            Layout.fillWidth: true
            text: row.value
            wrapMode: Text.Wrap
            color: row.muted ? Theme.textTertiary : Theme.text
        }
    }

    Row {
        id: trailingItems

        Layout.alignment: Qt.AlignVCenter
        spacing: Theme.spacing
    }
}
