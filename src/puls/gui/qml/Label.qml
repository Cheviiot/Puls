import QtQuick

// Text in the interface font. Numbers use tabular figures so that changing
// values do not shift the text around them.
Text {
    property bool tabular: false

    color: Theme.text
    font.family: Theme.family
    font.pixelSize: Theme.textBody
    font.features: tabular ? { "tnum": 1 } : {}
    linkColor: Theme.accentText
    textFormat: Text.PlainText
}
