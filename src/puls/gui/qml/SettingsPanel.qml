import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T

// Measurement and appearance settings in a panel over the dashboard. The
// dashboard model validates and stores the values when they are saved.
Item {
    id: panel

    required property var dashboard
    // Takes the whole width on narrow windows.
    property bool fullWidth: false
    property real bottomInset: 0
    readonly property bool opened: state === "open"

    // The values being edited.
    property int profile
    property int duration
    property int connections
    property int phase
    property string server
    property bool showIp
    property int theme
    property string error

    function open() {
        profile = dashboard.profileIndex
        duration = dashboard.durationSeconds
        connections = dashboard.connectionsIndex
        phase = dashboard.phaseIndex
        server = dashboard.speedtestServer
        showIp = dashboard.showIp
        theme = dashboard.themeIndex
        error = ""
        content.contentY = 0
        state = "open"
        sheet.forceActiveFocus()
    }

    function close() {
        state = ""
    }

    function save() {
        error = dashboard.applySettings(dashboard.serviceIndex, profile, String(duration),
                                        connections, phase, server, showIp, theme)
        if (error.length === 0)
            close()
    }

    visible: opened || sheet.x < width

    // The dimmed dashboard; a click closes the panel without saving.
    Rectangle {
        anchors.fill: parent
        color: Theme.overlay
        opacity: panel.opened ? 1 : 0
        visible: !panel.fullWidth

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.durationNormal
            }
        }

        TapHandler {
            onTapped: panel.close()
        }
    }

    FocusScope {
        id: sheet

        width: panel.fullWidth ? panel.width : Math.min(420, panel.width - 40)
        height: panel.height
        x: panel.opened ? panel.width - width : panel.width

        Behavior on x {
            NumberAnimation {
                duration: Theme.durationSlow
                easing.type: Easing.OutCubic
            }
        }

        Keys.onEscapePressed: panel.close()
        // The Android back gesture closes the panel instead of the app.
        Keys.onBackPressed: event => {
            panel.close()
            event.accepted = true
        }

        Rectangle {
            anchors.fill: parent
            color: Theme.surface
        }
        Rectangle {
            visible: !panel.fullWidth
            width: 1
            height: parent.height
            color: Theme.border
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.bottomMargin: panel.bottomInset
            spacing: 0

            // Header.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 14
                Layout.topMargin: 16
                Layout.bottomMargin: 8

                Label {
                    Layout.fillWidth: true
                    text: "Настройки"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                IconButton {
                    objectName: "settingsCloseButton"
                    iconPath: Icons.close
                    hint: "Закрыть"
                    onClicked: panel.close()
                }
            }

            Flickable {
                id: content

                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: sections.implicitHeight + 16
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                T.ScrollBar.vertical: ThinScrollBar {}

                ColumnLayout {
                    id: sections

                    x: 24
                    width: content.width - 48
                    spacing: 0

                    component SectionTitle: Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 20
                        Layout.bottomMargin: 10
                        font.pixelSize: Theme.textLabel
                        font.weight: Font.DemiBold
                        color: Theme.textSecondary
                    }

                    SectionTitle {
                        Layout.topMargin: 8
                        text: "Профиль"
                    }
                    ChoiceList {
                        objectName: "profileChoice"
                        Layout.fillWidth: true
                        titles: panel.dashboard.profileNames
                        details: panel.dashboard.profileDetails
                        currentIndex: panel.profile
                        onActivated: index => {
                            panel.profile = index
                            panel.duration = panel.dashboard.profileDurationSeconds(index)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 20

                        SectionTitle {
                            Layout.topMargin: 0
                            Layout.bottomMargin: 0
                            text: "Длительность"
                        }
                        Label {
                            tabular: true
                            text: panel.duration + " с"
                            font.weight: Font.DemiBold
                        }
                    }
                    ValueSlider {
                        objectName: "durationSlider"
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        from: 3
                        to: 60
                        value: panel.duration
                        onMoved: panel.duration = Math.round(value)
                        Accessible.name: "Длительность, секунд"
                    }

                    SectionTitle {
                        text: "Соединения"
                    }
                    Stepper {
                        objectName: "connectionsStepper"
                        Layout.fillWidth: true
                        labels: panel.dashboard.connectionLabels
                        currentIndex: panel.connections
                        accessibleName: "Соединения"
                        onActivated: index => panel.connections = index
                    }

                    SectionTitle {
                        text: "Этапы"
                    }
                    SegmentedControl {
                        objectName: "phaseSelector"
                        Layout.fillWidth: true
                        model: panel.dashboard.phaseNames
                        accessibleNames: panel.dashboard.phaseLabels
                        currentIndex: panel.phase
                        onActivated: index => panel.phase = index
                    }

                    SectionTitle {
                        text: "Сервер speedtest.ru"
                    }
                    InputField {
                        objectName: "serverField"
                        Layout.fillWidth: true
                        placeholderText: "Автоматически"
                        text: panel.server
                        enabled: panel.dashboard.serviceIndex === 1
                        onTextEdited: panel.server = text
                        Accessible.name: "Сервер speedtest.ru"
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        text: panel.dashboard.serviceIndex === 1
                              ? "Адрес в виде host:port; пустое поле — ближайший сервер."
                              : "Доступно, когда выбран сервис speedtest.ru."
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.textCaption
                        color: Theme.textSecondary
                    }

                    SectionTitle {
                        text: "Подключение"
                    }
                    SwitchRow {
                        objectName: "showIpSwitch"
                        Layout.fillWidth: true
                        text: "Показывать IP и провайдера"
                        description: "Определяются вместе с измерением и не сохраняются."
                        checked: panel.showIp
                        onToggled: panel.showIp = checked
                    }

                    SectionTitle {
                        text: "Оформление"
                    }
                    SegmentedControl {
                        objectName: "themeSelector"
                        Layout.fillWidth: true
                        model: panel.dashboard.themeLabels
                        currentIndex: panel.theme
                        onActivated: index => panel.theme = index
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 28
                        text: "Puls " + panel.dashboard.version
                        font.pixelSize: Theme.textCaption
                        color: Theme.textTertiary
                    }
                }
            }

            // Footer.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.border
            }
            Banner {
                objectName: "settingsError"
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.topMargin: 12
                visible: panel.error.length > 0
                text: panel.error
                tone: 3
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: Theme.spacingMedium

                ActionButton {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    kind: "secondary"
                    text: "Отмена"
                    onClicked: panel.close()
                }
                ActionButton {
                    objectName: "saveButton"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    text: "Сохранить"
                    onClicked: panel.save()
                }
            }
        }
    }

    states: State {
        name: "open"
    }
}
