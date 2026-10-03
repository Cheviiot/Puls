import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T

// The measurement itself: the service, the state, the main number, the
// phases and the start or stop button.
Card {
    id: hero

    required property var dashboard

    signal settingsRequested()

    // A connection lookup does not change the card.
    readonly property bool started: dashboard.measuring || dashboard.hasResult

    padding: 20
    spacing: 0

    SegmentedControl {
        objectName: "serviceSelector"
        Layout.fillWidth: true
        model: hero.dashboard.serviceNames
        accessibleNames: hero.dashboard.serviceLabels
        currentIndex: hero.dashboard.serviceIndex
        enabled: !hero.dashboard.busy
        onActivated: index => hero.dashboard.selectService(index)
    }

    // The state of the measurement.
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 22
        spacing: 7
        visible: hero.started

        Spinner {
            visible: hero.dashboard.measuring
            size: 14
        }
        Icon {
            visible: !hero.dashboard.measuring
            path: hero.dashboard.statusTone === 1 ? Icons.success
                : hero.dashboard.statusTone === 3 ? Icons.error
                : hero.dashboard.statusTone === 2 ? Icons.warning : Icons.info
            size: 15
            color: Theme.tone(hero.dashboard.statusTone)
        }
        Label {
            Layout.fillWidth: true
            text: hero.dashboard.status
            elide: Text.ElideRight
            font.pixelSize: Theme.textLabel
            font.weight: Font.DemiBold
            color: hero.dashboard.measuring ? Theme.accentText
                                            : Theme.tone(hero.dashboard.statusTone)
        }
    }

    // The main number; before the first measurement, an invitation.
    Item {
        Layout.fillWidth: true
        Layout.topMargin: hero.started ? 6 : 22
        implicitHeight: hero.started ? valueColumn.implicitHeight : idleColumn.implicitHeight

        ColumnLayout {
            id: idleColumn

            width: parent.width
            visible: !hero.started
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: hero.dashboard.status
                wrapMode: Text.Wrap
                font.pixelSize: Theme.textTitle
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: hero.dashboard.idleHint
                wrapMode: Text.Wrap
                color: Theme.textSecondary
            }
        }

        ColumnLayout {
            id: valueColumn

            width: parent.width
            visible: hero.started
            spacing: 0

            Item {
                Layout.fillWidth: true
                implicitHeight: number.implicitHeight

                Label {
                    id: number

                    objectName: "heroNumber"
                    readonly property bool empty: hero.dashboard.heroValue.length === 0

                    // While measuring, the placeholder below waits for the value.
                    visible: !empty || !hero.dashboard.measuring
                    tabular: true
                    text: empty ? "—" : hero.dashboard.heroValue
                    color: empty ? Theme.textTertiary : Theme.text
                    font.family: Theme.displayFamily
                    font.pixelSize: Theme.textDisplay
                    font.weight: Font.DemiBold
                    font.letterSpacing: -1.2
                    Accessible.name: empty ? hero.dashboard.heroLabel
                                           : hero.dashboard.heroLabel + ": " + text + " "
                                             + hero.dashboard.heroUnit
                }
                Label {
                    anchors.left: number.right
                    anchors.leftMargin: 8
                    anchors.baseline: number.baseline
                    visible: number.visible && !number.empty
                    text: hero.dashboard.heroUnit
                    font.pixelSize: 18
                    font.weight: Font.Medium
                    color: Theme.textSecondary
                }
                // A placeholder until the first value arrives.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !number.visible
                    width: 132
                    height: 14
                    radius: 7
                    color: Theme.surfaceMuted
                }
            }
            Label {
                Layout.fillWidth: true
                text: hero.dashboard.heroLabel
                elide: Text.ElideRight
                font.pixelSize: Theme.textLabel
                font.weight: Font.Medium
                color: Theme.textSecondary
            }
        }
    }

    PhaseTrack {
        Layout.fillWidth: true
        Layout.topMargin: 20
        phases: hero.dashboard.phases
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 10
        visible: text.length > 0
        text: hero.dashboard.serviceProgress
        elide: Text.ElideRight
        font.pixelSize: Theme.textCaption
        color: Theme.textSecondary
    }

    ActionButton {
        objectName: "startButton"
        Layout.fillWidth: true
        Layout.topMargin: 22
        text: hero.dashboard.startLabel
        kind: hero.dashboard.measuring ? "secondary" : "primary"
        iconPath: hero.dashboard.measuring ? Icons.stop
                : hero.dashboard.hasResult ? Icons.retry : Icons.play
        busy: hero.dashboard.measuring && hero.dashboard.stopping
        // A connection lookup is stopped with its own button.
        enabled: !hero.dashboard.stopping && !hero.dashboard.detecting
        onClicked: hero.dashboard.toggleMeasurement()
    }

    // The measurement settings; a click opens them.
    T.AbstractButton {
        id: summary

        Layout.fillWidth: true
        Layout.topMargin: 10
        implicitHeight: 28
        enabled: !hero.dashboard.busy
        hoverEnabled: true
        focusPolicy: Qt.TabFocus

        Accessible.role: Accessible.Button
        Accessible.name: "Настройки измерения: " + hero.dashboard.settingsSummary

        contentItem: Item {
            RowLayout {
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width)
                spacing: 6

                Icon {
                    path: Icons.settings
                    size: 13
                    color: summary.hovered ? Theme.accentText : Theme.textTertiary
                }
                Label {
                    Layout.fillWidth: true
                    text: hero.dashboard.settingsSummary
                    elide: Text.ElideRight
                    font.pixelSize: Theme.textCaption
                    color: summary.hovered ? Theme.accentText : Theme.textTertiary
                }
            }
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: "transparent"

            FocusRing {
                visible: summary.visualFocus
                targetRadius: Theme.radiusSmall
            }
        }

        onClicked: hero.settingsRequested()
    }
}
