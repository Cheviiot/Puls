import QtQuick
import QtQuick.Layouts

// A message about the measurement: a warning, an error or a hint.
Rectangle {
    id: banner

    property string text
    // A puls::gui::Tone value.
    property int tone: 2
    property bool closable: false

    signal closed()

    implicitWidth: 320
    implicitHeight: row.implicitHeight + 2 * Theme.spacingMedium
    radius: Theme.radiusButton
    color: Theme.toneSoft(tone)
    border.width: 1
    border.color: Qt.alpha(Theme.tone(tone), Theme.dark ? 0.35 : 0.25)

    Accessible.role: Accessible.AlertMessage
    Accessible.name: text

    RowLayout {
        id: row

        anchors.fill: parent
        anchors.margins: Theme.spacingMedium
        anchors.rightMargin: banner.closable ? Theme.spacing : Theme.spacingMedium
        spacing: 10

        Icon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 1
            path: banner.tone === 3 ? Icons.error
                : banner.tone === 1 ? Icons.success
                : banner.tone === 2 ? Icons.warning : Icons.info
            size: 18
            color: Theme.tone(banner.tone)
        }
        Label {
            Layout.fillWidth: true
            text: banner.text
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }
        IconButton {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: -6
            visible: banner.closable
            iconPath: Icons.close
            iconSize: 16
            hint: "Закрыть"
            onClicked: banner.closed()
        }
    }
}
