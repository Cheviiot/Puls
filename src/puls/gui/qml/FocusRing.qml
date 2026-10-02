import QtQuick

// The keyboard focus outline around a control.
Rectangle {
    // The corner radius of the outlined control.
    property real targetRadius: Theme.radiusSmall
    property real gap: 3

    anchors.fill: parent
    anchors.margins: -gap
    radius: targetRadius + gap
    color: "transparent"
    border.width: 2
    border.color: Theme.accent
}
