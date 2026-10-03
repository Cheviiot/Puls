import QtQuick
import QtQuick.Layouts

// One measured value: its name, number and unit. The tile of the phase
// being measured is outlined in the accent color.
Rectangle {
    id: tile

    property string title
    property string value
    property string unit
    property string iconPath
    property bool active: false

    readonly property bool empty: value.length === 0 || value === "—"

    implicitWidth: 160
    implicitHeight: content.implicitHeight + 2 * 14
    radius: 14
    color: Theme.surface
    border.width: active ? 1.5 : 1
    border.color: active ? Theme.accent : Theme.border

    Accessible.role: Accessible.StaticText
    Accessible.name: title + ": " + (empty ? "нет данных" : value + " " + unit)

    Behavior on border.color {
        ColorAnimation {
            duration: Theme.durationFast
        }
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        anchors.margins: 14
        spacing: 6

        RowLayout {
            spacing: 6

            Icon {
                path: tile.iconPath
                size: 15
                color: tile.active ? Theme.accent : Theme.textSecondary
            }
            Label {
                Layout.fillWidth: true
                text: tile.title
                elide: Text.ElideRight
                font.pixelSize: Theme.textLabel
                font.weight: Font.Medium
                color: Theme.textSecondary
            }
        }

        Item {
            Layout.fillWidth: true
            implicitHeight: number.implicitHeight

            Label {
                id: number

                tabular: true
                text: tile.empty ? "—" : tile.value
                font.pixelSize: Theme.textMetric
                font.weight: Font.DemiBold
                font.letterSpacing: -0.3
                color: tile.empty ? Theme.textTertiary : Theme.text
            }
            Label {
                anchors.left: number.right
                anchors.leftMargin: 5
                anchors.baseline: number.baseline
                visible: !tile.empty
                text: tile.unit
                font.pixelSize: Theme.textLabel
                font.weight: Font.Medium
                color: Theme.textSecondary
            }
        }
    }
}
