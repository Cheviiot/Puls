import QtQuick
import QtQuick.Templates as T

// A text button: the primary action, a secondary action or a quiet link.
T.Button {
    id: control

    // "primary", "secondary" or "quiet".
    property string kind: "primary"
    property string iconPath
    property bool busy: false

    readonly property bool primary: kind === "primary"
    readonly property bool quiet: kind === "quiet"
    readonly property color foreground: !enabled ? Theme.textTertiary
                                      : primary ? Theme.textOnAccent
                                      : quiet ? Theme.accentText : Theme.text

    implicitWidth: Math.ceil(implicitContentWidth) + leftPadding + rightPadding
    implicitHeight: quiet ? 32 : Theme.buttonHeight
    leftPadding: quiet ? 10 : 20
    rightPadding: quiet ? 10 : 20
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    font.family: Theme.family
    font.pixelSize: quiet ? Theme.textLabel : 15
    font.weight: Font.DemiBold

    Accessible.role: Accessible.Button
    Accessible.name: text

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row

            anchors.centerIn: parent
            spacing: 8

            Spinner {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.busy
                size: 16
                color: control.foreground
            }
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: !control.busy && control.iconPath.length > 0
                path: control.iconPath
                size: control.quiet ? 16 : 18
                color: control.foreground
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.foreground
                font: control.font
            }
        }
    }

    background: Rectangle {
        radius: control.quiet ? Theme.radiusSmall : Theme.radiusButton
        color: {
            if (control.primary) {
                if (!control.enabled)
                    return Theme.surfaceMuted
                return control.down ? Theme.accentPressed
                     : control.hovered ? Theme.accentHover : Theme.accentStrong
            }
            if (control.quiet)
                return control.down ? Theme.surfacePressed
                     : control.hovered ? Theme.accentSoft : "transparent"
            return control.down ? Theme.surfacePressed
                 : control.hovered ? Theme.surfaceHover : Theme.surfaceMuted
        }
        border.width: control.kind === "secondary" ? 1 : 0
        border.color: Theme.border

        Behavior on color {
            ColorAnimation {
                duration: Theme.durationFast
            }
        }

        FocusRing {
            visible: control.visualFocus
            targetRadius: control.quiet ? Theme.radiusSmall : Theme.radiusButton
        }
    }
}
