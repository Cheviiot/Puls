import QtQuick
import QtQuick.Shapes

// A stroke icon from Icons, drawn as a vector path in any color and size.
Item {
    id: icon

    property string path
    property color color: Theme.text
    property real size: 20

    implicitWidth: size
    implicitHeight: size

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        scale: icon.size / 24
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: icon.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathSvg {
                path: icon.path
            }
        }
    }
}
