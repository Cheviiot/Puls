pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// The phases of a measurement: latency, download and upload. Each phase is
// pending, active, done, failed or skipped; the active one shows its progress.
RowLayout {
    id: track

    // [{title, state, progress}]; a negative progress is indeterminate.
    property var phases: []

    spacing: Theme.spacing

    Repeater {
        // A count keeps the delegates and their animations while the
        // progress changes.
        model: track.phases.length

        delegate: ColumnLayout {
            id: phase

            required property int index
            readonly property var entry: track.phases[index] || ({})
            readonly property string phaseState: entry.state || "pending"
            readonly property bool active: phaseState === "active"
            readonly property real progress: phaseState === "done" || phaseState === "failed"
                                             ? 1 : active ? entry.progress : 0
            readonly property color tint: phaseState === "failed" ? Theme.danger
                                        : phaseState === "pending" || phaseState === "skipped"
                                          ? Theme.textTertiary : Theme.accent

            Layout.fillWidth: true
            Layout.preferredWidth: 1
            spacing: 6
            opacity: phaseState === "skipped" ? 0.5 : 1

            Accessible.role: Accessible.ProgressBar
            Accessible.name: entry.title || ""

            RowLayout {
                spacing: 4

                Icon {
                    visible: phase.phaseState === "done" || phase.phaseState === "failed"
                    path: phase.phaseState === "failed" ? Icons.error : Icons.check
                    size: 13
                    color: phase.phaseState === "failed" ? Theme.danger : Theme.success
                }
                Label {
                    Layout.fillWidth: true
                    text: phase.entry.title || ""
                    elide: Text.ElideRight
                    font.pixelSize: Theme.textCaption
                    font.weight: phase.active ? Font.DemiBold : Font.Medium
                    color: phase.active ? Theme.accentText
                         : phase.phaseState === "failed" ? Theme.danger
                         : phase.phaseState === "done" ? Theme.textSecondary : Theme.textTertiary
                }
            }

            Rectangle {
                id: bar

                Layout.fillWidth: true
                implicitHeight: 4
                radius: 2
                color: Theme.surfaceMuted
                clip: true

                Rectangle {
                    visible: phase.progress >= 0
                    width: parent.width * Math.max(0, Math.min(1, phase.progress))
                    height: parent.height
                    radius: 2
                    color: phase.tint

                    Behavior on width {
                        NumberAnimation {
                            duration: Theme.durationNormal
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                // Indeterminate progress: a segment that sweeps the track.
                Rectangle {
                    id: sweep

                    visible: phase.active && phase.progress < 0
                    width: parent.width * 0.4
                    height: parent.height
                    radius: 2
                    color: Theme.accent

                    XAnimator on x {
                        from: -sweep.width
                        to: bar.width
                        duration: 1100
                        loops: Animation.Infinite
                        running: sweep.visible
                        easing.type: Easing.InOutQuad
                    }
                }
            }
        }
    }
}
