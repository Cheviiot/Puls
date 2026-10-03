pragma Singleton
import QtQuick

// Colors, type, geometry and motion of the interface. Every component takes
// its look from here, so the dashboard is drawn the same on every platform.
QtObject {
    id: theme

    // Set by the window from the theme the user chose.
    property bool dark: false

    // The bundled Inter font: the platform fonts differ between systems.
    readonly property FontLoader regularFont: FontLoader {
        source: "fonts/Inter-Regular.ttf"
    }
    readonly property FontLoader mediumFont: FontLoader {
        source: "fonts/Inter-Medium.ttf"
    }
    readonly property FontLoader semiBoldFont: FontLoader {
        source: "fonts/Inter-SemiBold.ttf"
    }
    readonly property FontLoader displayFont: FontLoader {
        source: "fonts/InterDisplay-SemiBold.ttf"
    }
    readonly property string family: "Inter"
    readonly property string displayFamily: "Inter Display"

    // Surfaces and text.
    readonly property color background: dark ? "#0a0f14" : "#f3f6f8"
    readonly property color surface: dark ? "#111a21" : "#ffffff"
    readonly property color surfaceMuted: dark ? "#17222b" : "#edf2f5"
    readonly property color surfaceHover: dark ? "#1d2a35" : "#e3eaef"
    readonly property color surfacePressed: dark ? "#243441" : "#d8e1e7"
    readonly property color border: dark ? "#1e2a34" : "#e1e8ed"
    readonly property color borderStrong: dark ? "#2d3d4a" : "#c9d4dc"
    readonly property color text: dark ? "#e7eef3" : "#0e1a24"
    readonly property color textSecondary: dark ? "#94a5b3" : "#586776"
    readonly property color textTertiary: dark ? "#5f7180" : "#8696a4"
    readonly property color overlay: dark ? "#a6000000" : "#590e1a24"

    // The cyan accent.
    readonly property color accent: dark ? "#22d3ee" : "#0891b2"
    readonly property color accentText: dark ? "#67e8f9" : "#0e7490"
    readonly property color accentStrong: dark ? "#22d3ee" : "#0e7490"
    readonly property color accentHover: dark ? "#4fdcf1" : "#0c6a84"
    readonly property color accentPressed: dark ? "#7ee6f6" : "#155e75"
    readonly property color textOnAccent: dark ? "#062730" : "#ffffff"
    readonly property color accentSoft: dark ? "#0c2a33" : "#e0f4f8"

    // States; tone values follow puls::gui::Tone.
    readonly property color success: dark ? "#4ade80" : "#15803d"
    readonly property color successSoft: dark ? "#0f2a1c" : "#e5f5eb"
    readonly property color warning: dark ? "#fbbf24" : "#b45309"
    readonly property color warningSoft: dark ? "#2d2310" : "#fdf2df"
    readonly property color danger: dark ? "#f87171" : "#dc2626"
    readonly property color dangerSoft: dark ? "#321617" : "#fdebeb"

    function tone(value) {
        switch (value) {
        case 1:
            return success
        case 2:
            return warning
        case 3:
            return danger
        default:
            return textSecondary
        }
    }

    function toneSoft(value) {
        switch (value) {
        case 1:
            return successSoft
        case 2:
            return warningSoft
        case 3:
            return dangerSoft
        default:
            return surfaceMuted
        }
    }

    // Type scale in pixels.
    readonly property int textDisplay: 56
    readonly property int textTitle: 22
    readonly property int textHeadline: 16
    readonly property int textBody: 14
    readonly property int textLabel: 13
    readonly property int textCaption: 12
    readonly property int textMetric: 26

    // Geometry.
    readonly property int spacingSmall: 4
    readonly property int spacing: 8
    readonly property int spacingMedium: 12
    readonly property int spacingLarge: 16
    readonly property int spacingXLarge: 24
    readonly property int radiusSmall: 8
    readonly property int radiusControl: 10
    readonly property int radiusButton: 12
    readonly property int radiusCard: 16
    readonly property int controlHeight: 40
    readonly property int buttonHeight: 48

    // Motion.
    readonly property int durationFast: 120
    readonly property int durationNormal: 200
    readonly property int durationSlow: 280
}
