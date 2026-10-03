import QtQuick
import QtQuick.Templates as T

// A narrow scroll bar that appears while the content moves.
T.ScrollBar {
    id: control

    implicitWidth: 12
    implicitHeight: 12
    padding: 3
    minimumSize: 0.08
    visible: size < 1.0

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        radius: 3
        color: control.pressed ? Theme.textTertiary : Theme.borderStrong
        opacity: control.active || control.hovered ? 1 : 0

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.durationNormal
            }
        }
    }
}
