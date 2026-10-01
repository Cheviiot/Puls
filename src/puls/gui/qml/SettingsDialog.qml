import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Templates as T

// Measurement and appearance settings. Values are validated by the dashboard
// model when the dialog is saved.
Dialog {
    id: dialog

    required property var dashboard

    signal failed(string message)

    function load() {
        serviceBox.currentIndex = dashboard.serviceIndex
        profileBox.currentIndex = dashboard.profileIndex
        durationField.text = String(dashboard.durationSeconds)
        connectionsBox.currentIndex = dashboard.connectionsIndex
        phaseBox.currentIndex = dashboard.phaseIndex
        serverField.text = dashboard.speedtestServer
        showIpBox.checked = dashboard.showIp
        themeBox.currentIndex = dashboard.themeIndex
    }

    parent: T.Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(420, parent.width - 32)
    height: Math.min(implicitHeight, parent.height - 32)
    modal: true
    title: "Настройки"

    // Explicit background and buttons: Material elevation needs a GPU, and
    // the standard buttons would not be in Russian.
    background: Rectangle {
        radius: 4
        color: dialog.Material.dialogColor
    }
    footer: DialogButtonBox {
        Button {
            flat: true
            text: "Отмена"
            font.capitalization: Font.MixedCase
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        Button {
            objectName: "saveButton"
            flat: true
            text: "Сохранить"
            font.capitalization: Font.MixedCase
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }

    onAboutToShow: load()
    onAccepted: {
        const error = dashboard.applySettings(serviceBox.currentIndex, profileBox.currentIndex,
                                              durationField.text, connectionsBox.currentIndex,
                                              phaseBox.currentIndex, serverField.text,
                                              showIpBox.checked, themeBox.currentIndex)
        if (error.length > 0)
            failed(error)
    }

    contentItem: ScrollView {
        id: scroll

        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 2

            Label {
                text: "Сервис"
            }
            ComboBox {
                id: serviceBox

                Layout.fillWidth: true
                model: dialog.dashboard.serviceLabels
            }
            Label {
                Layout.topMargin: 8
                text: "Профиль"
            }
            ComboBox {
                id: profileBox

                Layout.fillWidth: true
                model: dialog.dashboard.profileLabels
                onActivated: index => {
                    durationField.text = String(dialog.dashboard.profileDurationSeconds(index))
                }
            }
            Label {
                Layout.topMargin: 8
                text: "Длительность, с"
            }
            TextField {
                id: durationField

                objectName: "durationField"
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhDigitsOnly
            }
            Label {
                Layout.topMargin: 8
                text: "Соединения"
            }
            ComboBox {
                id: connectionsBox

                Layout.fillWidth: true
                model: dialog.dashboard.connectionLabels
            }
            Label {
                Layout.topMargin: 8
                text: "Этап"
            }
            ComboBox {
                id: phaseBox

                Layout.fillWidth: true
                model: dialog.dashboard.phaseLabels
            }
            Label {
                Layout.topMargin: 8
                text: "Сервер speedtest"
            }
            TextField {
                id: serverField

                Layout.fillWidth: true
                placeholderText: "host:port"
                enabled: serviceBox.currentIndex === 1
            }
            CheckBox {
                id: showIpBox

                Layout.topMargin: 4
                text: "Показывать IP в результате"
            }
            Label {
                Layout.topMargin: 8
                text: "Оформление"
            }
            ComboBox {
                id: themeBox

                Layout.fillWidth: true
                model: dialog.dashboard.themeLabels
            }
        }
    }
}
