import QtQuick
import QtQuick.Templates as T

// A slider over whole numbers.
T.Slider {
    id: control

    stepSize: 1
    snapMode: T.Slider.SnapAlways
    implicitWidth: 240
    implicitHeight: 28
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    background: Item {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: control.availableWidth
        height: 4

        Rectangle {
            anchors.fill: parent
            radius: 2
            color: Theme.surfaceMuted
        }
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: control.enabled ? Theme.accent : Theme.borderStrong
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: 18
        implicitHeight: 18
        radius: 9
        color: Theme.surface
        border.width: 2
        border.color: control.enabled ? Theme.accent : Theme.borderStrong
        scale: control.pressed ? 1.1 : control.hovered ? 1.05 : 1

        Behavior on scale {
            NumberAnimation {
                duration: Theme.durationFast
            }
        }

        FocusRing {
            visible: control.visualFocus
            targetRadius: 9
        }
    }
}
