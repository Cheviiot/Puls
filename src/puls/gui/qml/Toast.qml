import QtQuick

// An error that does not belong to a field: shown over the dashboard for a
// few seconds.
Banner {
    id: toast

    property bool shown: false

    function show(message) {
        text = message
        shown = true
        hideTimer.restart()
    }

    tone: 3
    closable: true
    opacity: shown ? 1 : 0
    visible: opacity > 0
    border.color: Qt.alpha(Theme.danger, 0.45)

    Behavior on opacity {
        NumberAnimation {
            duration: Theme.durationNormal
        }
    }

    onClosed: shown = false

    Timer {
        id: hideTimer

        interval: 8000
        onTriggered: toast.shown = false
    }
}
