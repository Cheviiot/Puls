import QtQuick
import QtQuick.Shapes

// An activity indicator: a rotating arc.
Item {
    id: spinner

    property color color: Theme.accent
    property real size: 16
    property bool running: visible

    implicitWidth: size
    implicitHeight: size

    Shape {
        id: arc

        anchors.centerIn: parent
        width: 24
        height: 24
        scale: spinner.size / 24
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: spinner.color
            strokeWidth: 3
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap

            PathAngleArc {
                centerX: 12
                centerY: 12
                radiusX: 9
                radiusY: 9
                startAngle: -90
                sweepAngle: 270
            }
        }

        RotationAnimator on rotation {
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
            running: spinner.running
        }
    }
}
