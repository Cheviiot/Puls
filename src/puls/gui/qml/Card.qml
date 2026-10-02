import QtQuick
import QtQuick.Layouts

// A surface for a group of content; children are stacked in a column.
Rectangle {
    id: card

    property real padding: Theme.spacingLarge
    property real spacing: Theme.spacingMedium
    default property alias content: column.data

    implicitWidth: column.implicitWidth + 2 * padding
    implicitHeight: column.implicitHeight + 2 * padding
    radius: Theme.radiusCard
    color: Theme.surface
    border.width: 1
    border.color: Theme.border

    ColumnLayout {
        id: column

        anchors.fill: parent
        anchors.margins: card.padding
        spacing: card.spacing
    }
}
