import QtQuick
import QtQuick.Shapes

// The application mark: the pulse line of the application icon.
Rectangle {
    id: logo

    property real size: 22

    implicitWidth: size
    implicitHeight: size
    radius: size * 116 / 512
    color: "#0b1216"
    border.width: Theme.dark ? 1 : 0
    border.color: Theme.border

    Shape {
        anchors.centerIn: parent
        width: 512
        height: 512
        scale: logo.size / 512
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: "#22d3ee"
            strokeWidth: 34
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathSvg {
                path: "M76 276h80l38-112 62 220 52-164 32 56h96"
            }
        }
    }
}
