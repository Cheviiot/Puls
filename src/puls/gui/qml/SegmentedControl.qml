pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T

// A row of mutually exclusive options; the arrow keys move the choice.
FocusScope {
    id: control

    property var model: []
    property int currentIndex: 0
    // Full option names for screen readers when the labels are short.
    property var accessibleNames: []
    readonly property real segmentWidth: (width - 6) / Math.max(1, model.length)

    signal activated(int index)

    function choose(index) {
        if (!enabled || index < 0 || index >= model.length || index === currentIndex)
            return
        activated(index)
    }

    implicitWidth: 240
    implicitHeight: 36
    activeFocusOnTab: true
    opacity: enabled ? 1 : 0.55

    Keys.onLeftPressed: choose(currentIndex - 1)
    Keys.onRightPressed: choose(currentIndex + 1)

    Rectangle {
        id: track

        anchors.fill: parent
        radius: Theme.radiusControl
        color: Theme.surfaceMuted

        FocusRing {
            visible: control.activeFocus
            targetRadius: track.radius
        }
    }

    Rectangle {
        id: indicator

        visible: control.currentIndex >= 0
        x: 3 + control.segmentWidth * control.currentIndex
        y: 3
        width: control.segmentWidth
        height: control.height - 6
        radius: Theme.radiusControl - 3
        color: Theme.dark ? Theme.surfacePressed : Theme.surface
        border.width: 1
        border.color: Theme.dark ? Theme.borderStrong : Theme.border

        Behavior on x {
            NumberAnimation {
                duration: Theme.durationNormal
                easing.type: Easing.OutCubic
            }
        }
    }

    Row {
        anchors.fill: parent
        anchors.margins: 3

        Repeater {
            model: control.model

            delegate: T.AbstractButton {
                id: segment

                required property int index
                required property string modelData
                readonly property bool selected: index === control.currentIndex

                width: control.segmentWidth
                height: parent.height
                hoverEnabled: true
                focusPolicy: Qt.NoFocus

                Accessible.role: Accessible.RadioButton
                Accessible.name: control.accessibleNames[index] || modelData
                Accessible.checked: selected

                contentItem: Label {
                    text: segment.modelData
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font.pixelSize: Theme.textLabel
                    font.weight: segment.selected ? Font.DemiBold : Font.Medium
                    color: segment.selected ? Theme.text
                         : segment.hovered ? Theme.text : Theme.textSecondary
                }

                onClicked: {
                    control.forceActiveFocus(Qt.MouseFocusReason)
                    control.choose(index)
                }
            }
        }
    }
}
