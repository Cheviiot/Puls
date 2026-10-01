import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One mobile-first dashboard: service, current value, four metrics, server,
// connection, notices and per-service results.
ApplicationWindow {
    id: window

    required property var dashboard

    readonly property bool mobile: Qt.platform.os === "android" || Qt.platform.os === "ios"
    readonly property bool dark: dashboard.themeIndex === 2
                                 || (dashboard.themeIndex === 0 && dashboard.systemDark)
    readonly property color accentColor: dark ? "#22d3ee" : "#0891b2"
    readonly property color surfaceColor: dark ? "#0b1216" : "#f7fafb"
    readonly property color headerColor: dark ? "#101a20" : "#eef5f7"
    readonly property color secondaryColor: dark ? "#9aa8af" : "#5d6b72"
    readonly property color dangerColor: dark ? "#f87171" : "#dc2626"

    // Tones follow puls::gui::Tone: neutral, success, warning, danger.
    function toneColor(tone) {
        switch (tone) {
        case 1:
            return dark ? "#4ade80" : "#15803d"
        case 2:
            return dark ? "#fbbf24" : "#b45309"
        case 3:
            return dangerColor
        default:
            return secondaryColor
        }
    }

    function showError(message) {
        errorDialog.message = message
        errorDialog.open()
    }

    width: dashboard.windowWidth
    height: dashboard.windowHeight
    minimumWidth: mobile ? 0 : 390
    minimumHeight: mobile ? 0 : 640
    visible: true
    title: "Puls"
    color: surfaceColor

    Material.theme: dark ? Material.Dark : Material.Light
    Material.accent: accentColor
    Material.primary: accentColor
    Material.background: surfaceColor

    onClosing: {
        if (!mobile)
            dashboard.saveWindowSize(width, height)
        dashboard.shutdown()
    }

    Connections {
        target: window.dashboard
        function onErrorOccurred(message) {
            window.showError(message)
        }
    }

    header: ToolBar {
        Material.background: window.headerColor
        Material.foreground: window.dark ? "#e6eef1" : "#0f172a"
        leftPadding: 16
        rightPadding: 4
        topPadding: 6
        bottomPadding: 6

        RowLayout {
            width: parent.width

            ColumnLayout {
                spacing: 0
                Label {
                    text: "Puls"
                    font.pixelSize: 24
                    font.bold: true
                }
                Label {
                    text: "v" + window.dashboard.version.replace(/^v/, "")
                    color: window.secondaryColor
                    font.pixelSize: 12
                }
            }
            Item {
                Layout.fillWidth: true
            }
            ToolButton {
                text: "◐"
                font.pixelSize: 20
                ToolTip.visible: hovered
                ToolTip.text: "Оформление: " + window.dashboard.themeLabel
                Accessible.name: "Оформление"
                onClicked: window.dashboard.cycleTheme()
            }
            ToolButton {
                text: "⚙"
                font.pixelSize: 20
                enabled: !window.dashboard.busy
                ToolTip.visible: hovered
                ToolTip.text: "Настройки"
                Accessible.name: "Настройки"
                onClicked: settingsDialog.open()
            }
        }
    }

    ScrollView {
        id: scroll

        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 12

            Item {
                Layout.preferredHeight: 4
            }
            ComboBox {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                model: window.dashboard.serviceLabels
                currentIndex: window.dashboard.serviceIndex
                enabled: !window.dashboard.busy
                onActivated: index => window.dashboard.selectService(index)
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: window.dashboard.settingsSummary
                color: window.secondaryColor
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                implicitHeight: 1
                color: window.dark ? "#1f2d35" : "#d9e4e8"
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: window.dashboard.status
                    color: window.toneColor(window.dashboard.statusTone)
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: window.dashboard.currentValue
                    font.pixelSize: 52
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: window.dashboard.currentUnit
                    color: window.secondaryColor
                }
                ProgressBar {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: window.dashboard.progressVisible
                    value: window.dashboard.progress
                    indeterminate: window.dashboard.busy && window.dashboard.progress <= 0
                }
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                columns: window.width >= 720 ? 4 : 2
                columnSpacing: 8
                rowSpacing: 8

                MetricCard {
                    title: "Задержка"
                    value: window.dashboard.ping
                    unit: "мс"
                }
                MetricCard {
                    title: "Джиттер"
                    value: window.dashboard.jitter
                    unit: "мс"
                }
                MetricCard {
                    title: "Загрузка"
                    value: window.dashboard.download
                    unit: "Мбит/с"
                }
                MetricCard {
                    title: "Отдача"
                    value: window.dashboard.upload
                    unit: "Мбит/с"
                }
            }

            InfoCard {
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                title: "Сервер измерения"

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: window.dashboard.serverText
                }
            }

            InfoCard {
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                title: "Подключение"

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: window.dashboard.connectionText
                }
                Button {
                    flat: true
                    text: "Определить подключение"
                    font.capitalization: Font.MixedCase
                    enabled: !window.dashboard.busy
                    onClicked: window.dashboard.detectConnection()
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                visible: text.length > 0
                wrapMode: Text.WordWrap
                text: window.dashboard.notices
                color: window.toneColor(window.dashboard.noticeTone)
            }

            Repeater {
                model: window.dashboard.results

                delegate: InfoCard {
                    required property var modelData

                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    title: modelData.title
                    subtitle: modelData.subtitle

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: modelData.details
                    }
                }
            }

            Item {
                Layout.preferredHeight: 4
            }
        }
    }

    footer: Pane {
        padding: 12
        Material.background: window.surfaceColor

        // Drawn without Material elevation so that the primary action stays
        // visible with the software renderer too.
        Button {
            id: startButton

            width: parent.width
            text: window.dashboard.startLabel
            enabled: !window.dashboard.stopping
            onClicked: window.dashboard.toggleMeasurement()

            contentItem: Label {
                text: startButton.text
                color: "#ffffff"
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            background: Rectangle {
                implicitHeight: 48
                radius: 12
                color: !startButton.enabled ? (window.dark ? "#3b4a52" : "#94a3ab")
                     : window.dashboard.busy ? (startButton.down ? "#b91c1c" : "#dc2626")
                     : (startButton.down ? "#0e7490" : "#0891b2")
            }
        }
    }

    SettingsDialog {
        id: settingsDialog

        objectName: "settingsDialog"
        dashboard: window.dashboard
        onFailed: message => window.showError(message)
    }

    Dialog {
        id: errorDialog

        property string message

        objectName: "errorDialog"
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(420, parent.width - 32)
        modal: true
        title: "Ошибка"

        background: Rectangle {
            radius: 4
            color: errorDialog.Material.dialogColor
        }
        footer: DialogButtonBox {
            Button {
                flat: true
                text: "ОК"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: errorDialog.message
        }
    }
}
