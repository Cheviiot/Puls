import QtQuick
import QtQuick.Layouts

// The result of one measurement service when several were measured.
ColumnLayout {
    id: row

    // {service, status, tone, ping, download, upload, server}
    property var result: ({})

    spacing: 6

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing

        Label {
            Layout.fillWidth: true
            text: row.result.service || ""
            elide: Text.ElideRight
            font.weight: Font.DemiBold
        }
        Rectangle {
            implicitWidth: badge.implicitWidth + 16
            implicitHeight: 22
            radius: 11
            color: Theme.toneSoft(row.result.tone || 0)

            Label {
                id: badge

                anchors.centerIn: parent
                text: row.result.status || ""
                font.pixelSize: Theme.textCaption
                font.weight: Font.DemiBold
                color: Theme.tone(row.result.tone || 0)
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacingLarge

        Repeater {
            model: [
                { icon: Icons.ping, value: row.result.ping },
                { icon: Icons.download, value: row.result.download },
                { icon: Icons.upload, value: row.result.upload },
            ]

            delegate: RowLayout {
                id: figure

                required property var modelData

                spacing: 4

                Icon {
                    path: figure.modelData.icon
                    size: 14
                    color: Theme.textTertiary
                }
                Label {
                    tabular: true
                    text: figure.modelData.value || "—"
                    font.pixelSize: Theme.textLabel
                    color: Theme.textSecondary
                }
            }
        }
        Item {
            Layout.fillWidth: true
        }
    }

    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: row.result.server || ""
        elide: Text.ElideRight
        font.pixelSize: Theme.textCaption
        color: Theme.textTertiary
    }
}
