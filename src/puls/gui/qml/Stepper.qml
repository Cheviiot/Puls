import QtQuick
import QtQuick.Layouts

// Steps through a list of labelled values with minus and plus buttons.
FocusScope {
    id: control

    property var labels: []
    property int currentIndex: 0
    property string accessibleName

    signal activated(int index)

    function step(delta) {
        const index = Math.max(0, Math.min(labels.length - 1, currentIndex + delta))
        if (index !== currentIndex)
            activated(index)
    }

    implicitWidth: 200
    implicitHeight: Theme.controlHeight
    activeFocusOnTab: true

    Accessible.role: Accessible.SpinBox
    Accessible.name: accessibleName
    Accessible.description: labels[currentIndex] || ""

    Keys.onLeftPressed: step(-1)
    Keys.onDownPressed: step(-1)
    Keys.onRightPressed: step(1)
    Keys.onUpPressed: step(1)

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusControl
        color: Theme.surfaceMuted

        FocusRing {
            visible: control.activeFocus
            targetRadius: Theme.radiusControl
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 0

        IconButton {
            iconPath: Icons.minus
            iconSize: 16
            hint: "Меньше"
            enabled: control.currentIndex > 0
            focusPolicy: Qt.NoFocus
            onClicked: control.step(-1)
        }
        Label {
            Layout.fillWidth: true
            tabular: true
            text: control.labels[control.currentIndex] || ""
            horizontalAlignment: Text.AlignHCenter
            font.weight: Font.DemiBold
        }
        IconButton {
            iconPath: Icons.plus
            iconSize: 16
            hint: "Больше"
            enabled: control.currentIndex < control.labels.length - 1
            focusPolicy: Qt.NoFocus
            onClicked: control.step(1)
        }
    }
}
