import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T

// One dashboard for phones and desktops. The window draws its own title bar:
// on Windows and macOS the system keeps its window buttons in it, elsewhere
// the bar has its own.
Window {
    id: window

    required property var dashboard
    // The platform; the tests show the window of every platform.
    property string os: Qt.platform.os

    readonly property bool mobile: os === "android" || os === "ios"
    readonly property bool systemButtons: os === "windows" || os === "osx"
    readonly property bool ownFrame: !mobile && !systemButtons
    readonly property bool wide: width >= 760
    readonly property bool dark: dashboard.themeIndex === 2
                                 || (dashboard.themeIndex === 0 && dashboard.systemDark)

    function openSettings() {
        if (!dashboard.busy)
            settings.open()
    }

    width: dashboard.windowWidth
    // A stored or default height may not fit a small screen.
    height: Math.min(dashboard.windowHeight,
                     Math.max(minimumHeight, Screen.desktopAvailableHeight - 48))
    minimumWidth: mobile ? 0 : dashboard.minimumWindowWidth
    minimumHeight: mobile ? 0 : dashboard.minimumWindowHeight
    visible: true
    // macOS would draw the title over the bar.
    title: os === "osx" ? "" : "Puls"
    color: Theme.background
    flags: {
        // Phones show the interface under the system bars.
        if (mobile)
            return Qt.Window | Qt.ExpandedClientAreaHint
        // The system keeps its window buttons, shadow and snapping; without
        // the title hint Windows draws no title and icon of its own.
        if (os === "windows")
            return Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
                   | Qt.CustomizeWindowHint | Qt.WindowMinimizeButtonHint
                   | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint
        if (os === "osx")
            return Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
        // Elsewhere the window draws its whole frame.
        return Qt.Window | Qt.FramelessWindowHint
    }

    Binding {
        target: Theme
        property: "dark"
        value: window.dark
    }

    onClosing: {
        if (!mobile)
            dashboard.saveWindowSize(width, height)
        dashboard.shutdown()
    }

    Connections {
        target: window.dashboard

        function onErrorOccurred(message) {
            toast.show(message)
        }
    }

    Item {
        id: root

        anchors.fill: parent

        TitleBar {
            id: titleBar

            objectName: "titleBar"
            y: window.mobile ? root.SafeArea.margins.top : 0
            width: parent.width
            height: window.mobile ? 56
                  : window.systemButtons ? Math.max(root.SafeArea.margins.top,
                                                    window.os === "osx" ? 28 : 32)
                  : 40
            window: window
            dashboard: window.dashboard
            os: window.os
            mobile: window.mobile
            leadingInset: window.os === "osx" ? 72 : root.SafeArea.margins.left
            // The minimize, maximize and close buttons that Windows draws.
            trailingInset: window.os === "windows" ? Math.round(height * 4.5)
                                                   : root.SafeArea.margins.right
            onSettingsRequested: window.openSettings()
        }

        Flickable {
            id: page

            anchors.top: titleBar.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            contentWidth: width
            contentHeight: layout.implicitHeight + layout.y + 24 + root.SafeArea.margins.bottom
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            T.ScrollBar.vertical: ThinScrollBar {}

            GridLayout {
                id: layout

                readonly property real margin: window.wide ? 24 : 16

                x: Math.max(margin + root.SafeArea.margins.left,
                            (page.width - width) / 2)
                y: window.wide ? 12 : 4
                width: Math.min(page.width - 2 * margin - root.SafeArea.margins.left
                                - root.SafeArea.margins.right,
                                window.wide ? 1040 : 640)
                columns: window.wide ? 2 : 1
                columnSpacing: 16
                rowSpacing: 12

                HeroCard {
                    objectName: "hero"
                    Layout.preferredWidth: window.wide ? 372 : -1
                    Layout.fillWidth: !window.wide
                    Layout.rowSpan: window.wide ? 4 : 1
                    Layout.alignment: Qt.AlignTop
                    dashboard: window.dashboard
                    onSettingsRequested: window.openSettings()
                }

                Banner {
                    objectName: "notices"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    visible: window.dashboard.notices.length > 0
                    text: window.dashboard.notices
                    tone: window.dashboard.noticeTone
                }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 12

                    MetricTile {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        title: "Задержка"
                        iconPath: Icons.ping
                        value: window.dashboard.ping
                        unit: "мс"
                        active: window.dashboard.activeMetric === "ping"
                    }
                    MetricTile {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        title: "Джиттер"
                        iconPath: Icons.jitter
                        value: window.dashboard.jitter
                        unit: "мс"
                        active: window.dashboard.activeMetric === "ping"
                    }
                    MetricTile {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        title: "Загрузка"
                        iconPath: Icons.download
                        value: window.dashboard.download
                        unit: "Мбит/с"
                        active: window.dashboard.activeMetric === "download"
                    }
                    MetricTile {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        title: "Отдача"
                        iconPath: Icons.upload
                        value: window.dashboard.upload
                        unit: "Мбит/с"
                        active: window.dashboard.activeMetric === "upload"
                    }
                }

                Card {
                    objectName: "details"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: 14

                    DetailRow {
                        Layout.fillWidth: true
                        iconPath: Icons.server
                        title: "Сервер измерения"
                        value: window.dashboard.serverText
                        muted: !window.dashboard.serverKnown
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.border
                    }
                    DetailRow {
                        Layout.fillWidth: true
                        iconPath: Icons.connection
                        title: "Подключение"
                        value: window.dashboard.connectionText
                        muted: !window.dashboard.connectionKnown

                        ActionButton {
                            objectName: "detectButton"
                            kind: "quiet"
                            text: window.dashboard.detecting ? "Остановить" : "Определить"
                            iconPath: Icons.refresh
                            busy: window.dashboard.detecting
                            enabled: !window.dashboard.measuring
                                     && !(window.dashboard.detecting && window.dashboard.stopping)
                            onClicked: window.dashboard.detecting ? window.dashboard.cancel()
                                                                  : window.dashboard.detectConnection()
                        }
                    }
                }

                Card {
                    objectName: "results"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    // Only runs that measure several services have it.
                    visible: window.dashboard.results.length > 0
                    spacing: 14

                    Label {
                        text: "Итоги по сервисам"
                        font.pixelSize: Theme.textHeadline
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: window.dashboard.results

                        delegate: ColumnLayout {
                            id: resultItem

                            required property var modelData
                            required property int index

                            Layout.fillWidth: true
                            spacing: 14

                            Rectangle {
                                Layout.fillWidth: true
                                visible: resultItem.index > 0
                                implicitHeight: 1
                                color: Theme.border
                            }
                            ResultRow {
                                Layout.fillWidth: true
                                result: resultItem.modelData
                            }
                        }
                    }
                }
            }
        }

        SettingsPanel {
            id: settings

            objectName: "settingsPanel"
            anchors.top: titleBar.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            fullWidth: root.width < 560
            leftInset: root.SafeArea.margins.left
            rightInset: root.SafeArea.margins.right
            bottomInset: root.SafeArea.margins.bottom
            dashboard: window.dashboard
        }

        Toast {
            id: toast

            objectName: "toast"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16 + root.SafeArea.margins.bottom
            width: Math.min(460, parent.width - 32)
        }

        // Without a system frame the window draws its edge and resizes from it.
        Rectangle {
            anchors.fill: parent
            visible: window.ownFrame && window.visibility === Window.Windowed
            color: "transparent"
            border.width: 1
            border.color: Theme.borderStrong
        }
        ResizeFrame {
            anchors.fill: parent
            visible: window.ownFrame && window.visibility === Window.Windowed
            window: window
        }
    }
}
