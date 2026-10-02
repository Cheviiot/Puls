import QtQuick
import QtQuick.Layouts

// The title bar: the application mark, the theme and settings buttons and,
// where the system draws none, the window buttons. On Windows and macOS the
// bar shares the window's title area with the system buttons.
Item {
    id: bar

    required property Window window
    required property var dashboard
    property string os: Qt.platform.os
    property bool mobile: false
    // Space at the edges taken by the system: the window buttons on Windows
    // and macOS, the display cutouts on phones.
    property real leadingInset: 0
    property real trailingInset: 0

    readonly property bool ownWindowButtons: !mobile && os !== "windows" && os !== "osx"
    readonly property bool maximized: window.visibility === Window.Maximized
                                      || window.visibility === Window.FullScreen

    signal settingsRequested()

    function toggleMaximized() {
        if (maximized)
            window.showNormal()
        else
            window.showMaximized()
    }

    // Dragging the empty part of the bar moves the window and a double click
    // maximizes it; the buttons above keep their own presses. On Windows the
    // system recognizes the bar from the presses that the interface leaves,
    // so the area is off there.
    MouseArea {
        id: moveArea

        property point origin
        property bool moving: false

        anchors.fill: parent
        enabled: !bar.mobile && bar.os !== "windows"
        onPressed: mouse => {
            origin = Qt.point(mouse.x, mouse.y)
            moving = false
        }
        onPositionChanged: mouse => {
            if (moving || !pressed)
                return
            const distance = Math.max(Math.abs(mouse.x - origin.x), Math.abs(mouse.y - origin.y))
            if (distance >= Application.styleHints.startDragDistance) {
                moving = true
                bar.window.startSystemMove()
            }
        }
        onDoubleClicked: bar.toggleMaximized()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: bar.leadingInset + (bar.mobile ? 16 : 12)
        anchors.rightMargin: bar.trailingInset + (bar.ownWindowButtons ? 6 : 8)
        spacing: 4

        AppLogo {
            size: bar.mobile ? 26 : 20
        }
        Label {
            Layout.leftMargin: 6
            text: "Puls"
            font.pixelSize: bar.mobile ? 18 : 14
            font.weight: Font.DemiBold
        }
        Item {
            Layout.fillWidth: true
        }

        IconButton {
            objectName: "themeButton"
            iconPath: bar.dashboard.themeIndex === 1 ? Icons.themeLight
                    : bar.dashboard.themeIndex === 2 ? Icons.themeDark : Icons.themeSystem
            hint: "Оформление: " + bar.dashboard.themeLabel
            implicitWidth: bar.mobile ? 40 : 32
            implicitHeight: implicitWidth
            iconSize: bar.mobile ? 20 : 18
            onClicked: bar.dashboard.cycleTheme()
        }
        IconButton {
            objectName: "settingsButton"
            iconPath: Icons.settings
            hint: "Настройки"
            enabled: !bar.dashboard.busy
            implicitWidth: bar.mobile ? 40 : 32
            implicitHeight: implicitWidth
            iconSize: bar.mobile ? 20 : 18
            onClicked: bar.settingsRequested()
        }

        // Window buttons where the system draws none.
        Rectangle {
            visible: bar.ownWindowButtons
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            implicitWidth: 1
            implicitHeight: 16
            color: Theme.border
        }
        IconButton {
            objectName: "minimizeButton"
            visible: bar.ownWindowButtons
            iconPath: Icons.minimize
            iconSize: 16
            hint: "Свернуть"
            onClicked: bar.window.showMinimized()
        }
        IconButton {
            objectName: "maximizeButton"
            visible: bar.ownWindowButtons
            iconPath: bar.maximized ? Icons.restore : Icons.maximize
            iconSize: bar.maximized ? 15 : 14
            hint: bar.maximized ? "Восстановить" : "Развернуть"
            onClicked: bar.toggleMaximized()
        }
        IconButton {
            objectName: "closeButton"
            visible: bar.ownWindowButtons
            iconPath: Icons.close
            iconSize: 17
            hint: "Закрыть"
            destructive: true
            onClicked: bar.window.close()
        }
    }
}
