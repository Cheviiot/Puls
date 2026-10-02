pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T

// A list of mutually exclusive options with a title and a detail each.
FocusScope {
    id: control

    property var titles: []
    property var details: []
    property int currentIndex: 0

    signal activated(int index)

    function choose(index) {
        if (index >= 0 && index < titles.length && index !== currentIndex)
            activated(index)
    }

    implicitWidth: 280
    implicitHeight: column.implicitHeight
    activeFocusOnTab: true

    Keys.onUpPressed: choose(currentIndex - 1)
    Keys.onDownPressed: choose(currentIndex + 1)

    ColumnLayout {
        id: column

        width: parent.width
        spacing: 6

        Repeater {
            model: control.titles

            delegate: T.AbstractButton {
                id: option

                required property int index
                required property string modelData
                readonly property bool selected: index === control.currentIndex

                Layout.fillWidth: true
                implicitHeight: 46
                hoverEnabled: true
                focusPolicy: Qt.NoFocus

                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData + ", " + (control.details[index] || "")
                Accessible.checked: selected

                contentItem: RowLayout {
                    spacing: 10

                    Rectangle {
                        implicitWidth: 18
                        implicitHeight: 18
                        radius: 9
                        color: "transparent"
                        border.width: option.selected ? 5 : 1.5
                        border.color: option.selected ? Theme.accent : Theme.borderStrong

                        Behavior on border.width {
                            NumberAnimation {
                                duration: Theme.durationFast
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: option.modelData
                        elide: Text.ElideRight
                        font.weight: option.selected ? Font.DemiBold : Font.Normal
                    }
                    Label {
                        tabular: true
                        text: control.details[option.index] || ""
                        font.pixelSize: Theme.textLabel
                        color: Theme.textSecondary
                    }
                }
                leftPadding: 12
                rightPadding: 14

                background: Rectangle {
                    radius: Theme.radiusControl
                    color: option.selected ? Theme.accentSoft
                         : option.hovered ? Theme.surfaceHover : Theme.surface
                    border.width: 1
                    border.color: option.selected ? Qt.alpha(Theme.accent, 0.6) : Theme.border

                    FocusRing {
                        visible: control.activeFocus && option.selected
                        targetRadius: Theme.radiusControl
                    }
                }

                onClicked: {
                    control.forceActiveFocus(Qt.MouseFocusReason)
                    control.choose(index)
                }
            }
        }
    }
}
