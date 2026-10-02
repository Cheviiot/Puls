import QtQuick
import QtQuick.Templates as T

// A single-line text field.
T.TextField {
    id: control

    implicitWidth: 240
    implicitHeight: Theme.controlHeight
    leftPadding: 12
    rightPadding: 12
    verticalAlignment: TextInput.AlignVCenter
    hoverEnabled: true
    font.family: Theme.family
    font.pixelSize: Theme.textBody
    color: enabled ? Theme.text : Theme.textTertiary
    placeholderTextColor: Theme.textTertiary
    selectionColor: Theme.accent
    selectedTextColor: Theme.textOnAccent

    Label {
        x: control.leftPadding
        width: control.width - control.leftPadding - control.rightPadding
        height: control.height
        verticalAlignment: Text.AlignVCenter
        visible: control.length === 0 && control.preeditText.length === 0
        text: control.placeholderText
        elide: Text.ElideRight
        color: control.placeholderTextColor
        font: control.font
    }

    background: Rectangle {
        radius: Theme.radiusControl
        color: control.enabled ? Theme.surface : Theme.surfaceMuted
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent
                    : control.hovered && control.enabled ? Theme.textTertiary : Theme.borderStrong
    }
}
