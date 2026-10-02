import QtQuick
import QtQuick.Templates as T

// A short hint shown after the pointer rests on a control.
T.ToolTip {
    id: control

    x: Math.round((parent ? parent.width - implicitWidth : 0) / 2)
    y: parent ? parent.height + 6 : 0
    delay: 600
    timeout: 5000
    margins: 8
    padding: 6
    leftPadding: 10
    rightPadding: 10
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
                 | T.Popup.CloseOnReleaseOutsideParent

    implicitWidth: Math.ceil(implicitContentWidth) + leftPadding + rightPadding
    implicitHeight: Math.ceil(implicitContentHeight) + topPadding + bottomPadding

    contentItem: Label {
        text: control.text
        color: Theme.dark ? Theme.text : "#ffffff"
        font.pixelSize: Theme.textCaption
        font.weight: Font.Medium
    }
    background: Rectangle {
        radius: 6
        color: Theme.dark ? "#2a3946" : "#1c2833"
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0
            to: 1
            duration: Theme.durationFast
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"
            from: 1
            to: 0
            duration: Theme.durationFast
        }
    }
}
