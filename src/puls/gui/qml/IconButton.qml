import QtQuick
import QtQuick.Templates as T

// A square button with an icon and a hint.
T.AbstractButton {
    id: control

    property string iconPath
    property color iconColor: Theme.textSecondary
    property string hint
    // The close button of a window highlights in red.
    property bool destructive: false
    property real iconSize: 18

    implicitWidth: 32
    implicitHeight: 32
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    Accessible.role: Accessible.Button
    Accessible.name: hint

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            path: control.iconPath
            size: control.iconSize
            color: control.destructive && control.hovered ? Theme.danger
                 : control.enabled ? (control.hovered ? Theme.text : control.iconColor)
                 : Theme.textTertiary
        }
    }

    background: Rectangle {
        radius: Theme.radiusSmall
        color: control.destructive && control.hovered ? Theme.dangerSoft
             : control.down ? Theme.surfacePressed
             : control.hovered ? Theme.surfaceHover : "transparent"

        Behavior on color {
            ColorAnimation {
                duration: Theme.durationFast
            }
        }

        FocusRing {
            visible: control.visualFocus
            targetRadius: Theme.radiusSmall
            gap: 2
        }
    }

    ToolTip {
        text: control.hint
        visible: control.hovered && control.hint.length > 0
    }
}
