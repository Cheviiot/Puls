pragma ComponentBehavior: Bound

import QtQuick

// The edges of a window without a system frame: dragging them resizes the
// window through the window manager.
Item {
    id: frame

    required property Window window
    property int thickness: 6
    property int corner: 14

    component Edge: MouseArea {
        property int edges

        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onPressed: frame.window.startSystemResize(edges)
    }

    Edge {
        x: 0
        y: frame.corner
        width: frame.thickness
        height: frame.height - 2 * frame.corner
        edges: Qt.LeftEdge
        cursorShape: Qt.SizeHorCursor
    }
    Edge {
        x: frame.width - width
        y: frame.corner
        width: frame.thickness
        height: frame.height - 2 * frame.corner
        edges: Qt.RightEdge
        cursorShape: Qt.SizeHorCursor
    }
    Edge {
        x: frame.corner
        y: 0
        width: frame.width - 2 * frame.corner
        height: frame.thickness
        edges: Qt.TopEdge
        cursorShape: Qt.SizeVerCursor
    }
    Edge {
        x: frame.corner
        y: frame.height - height
        width: frame.width - 2 * frame.corner
        height: frame.thickness
        edges: Qt.BottomEdge
        cursorShape: Qt.SizeVerCursor
    }
    Edge {
        width: frame.corner
        height: frame.corner
        edges: Qt.TopEdge | Qt.LeftEdge
        cursorShape: Qt.SizeFDiagCursor
    }
    Edge {
        x: frame.width - width
        width: frame.corner
        height: frame.corner
        edges: Qt.TopEdge | Qt.RightEdge
        cursorShape: Qt.SizeBDiagCursor
    }
    Edge {
        y: frame.height - height
        width: frame.corner
        height: frame.corner
        edges: Qt.BottomEdge | Qt.LeftEdge
        cursorShape: Qt.SizeBDiagCursor
    }
    Edge {
        x: frame.width - width
        y: frame.height - height
        width: frame.corner
        height: frame.corner
        edges: Qt.BottomEdge | Qt.RightEdge
        cursorShape: Qt.SizeFDiagCursor
    }
}
