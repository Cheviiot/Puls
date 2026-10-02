import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T

// An option that is on or off, with a title and an explanation.
T.Switch {
    id: control

    property string description

    implicitWidth: 280
    implicitHeight: Math.max(row.implicitHeight, 40)
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.description: description
    Accessible.checked: checked

    indicator: Item {}

    contentItem: RowLayout {
        id: row

        spacing: Theme.spacingMedium

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: control.text
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: control.description
                wrapMode: Text.Wrap
                font.pixelSize: Theme.textCaption
                color: Theme.textSecondary
            }
        }

        Rectangle {
            implicitWidth: 40
            implicitHeight: 24
            radius: 12
            color: control.checked ? Theme.accentStrong : Theme.borderStrong

            Behavior on color {
                ColorAnimation {
                    duration: Theme.durationFast
                }
            }

            Rectangle {
                x: control.checked ? parent.width - width - 3 : 3
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                radius: 9
                color: Theme.dark && control.checked ? Theme.textOnAccent : "#ffffff"

                Behavior on x {
                    NumberAnimation {
                        duration: Theme.durationNormal
                        easing.type: Easing.OutCubic
                    }
                }
            }

            FocusRing {
                visible: control.visualFocus
                targetRadius: 12
            }
        }
    }
}
