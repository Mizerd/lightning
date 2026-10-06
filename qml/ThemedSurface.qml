import QtQuick
import QtQuick.Shapes
import MatrixClient

// A shell surface painted in one theme role: the flat colour, or the role's
// gradient when the theme gives it one (a custom theme's own gradient, or
// Settings -> Appearance -> Depth). One component instead of touching every
// `color:` site: a host keeps its flat `color:` and lays this beneath its
// children with `flatFill: false`, so nothing changes until a gradient
// exists.
//
// Rectangular only (the shell grounds have no radius). Readability: every
// stop is graded by CustomThemeStore::auditWithGradients, at the worst one.
Item {
    id: root

    // "background" | "sidebar" | "rail" | "surface" | "settingsPage" |
    // "settingsNav" (CustomThemeStore::gradientRoles()).
    property string role: "background"
    // Paint the flat colour when there is no gradient. A host that already
    // paints its own flat colour sets this false and the item draws nothing
    // until a gradient exists.
    property bool flatFill: true
    // Depth only: a soft inner shade along one edge, where a raised
    // neighbour meets this surface. "" | "left" | "right" | "top" | "bottom".
    property string innerShadowEdge: ""

    // An explicit gradient (or null for none) instead of the live theme's,
    // for previews of a theme that is not applied (the theme editor).
    property var specOverride: undefined

    readonly property var spec: specOverride !== undefined
                                ? specOverride : AppTheme.gradientFor(role)
    readonly property bool hasGradient: !!spec
    readonly property bool _radial: hasGradient && spec.type === "radial"
    readonly property int _stopCount: hasGradient ? spec.stops.length : 0

    function _stop(i) {
        if (!hasGradient)
            return AppTheme.surfaceFlat(role)
        return spec.stops[Math.max(0, Math.min(_stopCount - 1, i))]
    }

    // CSS convention: 0 points up, 90 right, 180 down. The gradient line
    // passes through the centre and is long enough to reach both corners.
    readonly property real _angleRad: hasGradient && !_radial
                                      ? spec.angle * Math.PI / 180 : Math.PI
    readonly property real _dx: Math.sin(_angleRad)
    readonly property real _dy: -Math.cos(_angleRad)
    readonly property real _halfLength: (Math.abs(width * _dx)
                                         + Math.abs(height * _dy)) / 2

    Rectangle {
        anchors.fill: parent
        visible: root.flatFill && !root.hasGradient
        color: AppTheme.surfaceFlat(root.role)
    }

    Shape {
        id: shape
        anchors.fill: parent
        visible: root.hasGradient && root.width > 0 && root.height > 0
        ShapePath {
            strokeWidth: -1
            strokeColor: "transparent"
            fillGradient: root._radial ? radialFill : linearFill
            startX: 0
            startY: 0
            PathLine { x: root.width; y: 0 }
            PathLine { x: root.width; y: root.height }
            PathLine { x: 0; y: root.height }
            PathLine { x: 0; y: 0 }
        }
    }

    LinearGradient {
        id: linearFill
        x1: root.width / 2 - root._dx * root._halfLength
        y1: root.height / 2 - root._dy * root._halfLength
        x2: root.width / 2 + root._dx * root._halfLength
        y2: root.height / 2 + root._dy * root._halfLength
        GradientStop { position: 0.0; color: root._stop(0) }
        GradientStop {
            position: root._stopCount > 2 ? 0.5 : 1.0
            color: root._stop(1)
        }
        GradientStop { position: 1.0; color: root._stop(root._stopCount - 1) }
    }

    // Subtle by construction: centred, reaching the far corners.
    RadialGradient {
        id: radialFill
        centerX: root.width / 2
        centerY: root.height / 2
        focalX: centerX
        focalY: centerY
        centerRadius: Math.sqrt(root.width * root.width + root.height * root.height) / 2
        focalRadius: 0
        GradientStop { position: 0.0; color: root._stop(0) }
        GradientStop {
            position: root._stopCount > 2 ? 0.5 : 1.0
            color: root._stop(1)
        }
        GradientStop { position: 1.0; color: root._stop(root._stopCount - 1) }
    }

    // The inner shade: Depth's "this neighbour sits above me". Never under
    // text that matters (12 px at the boundary) and translucent black, which
    // only darkens; on a light theme the ink is dark, so the shade is kept to
    // the shadowSoft token.
    Rectangle {
        id: shade
        readonly property bool horizontal: root.innerShadowEdge === "left"
                                           || root.innerShadowEdge === "right"
        visible: AppTheme.surfaceDepth === 1 && root.innerShadowEdge !== ""
        width: horizontal ? 12 : root.width
        height: horizontal ? root.height : 12
        x: root.innerShadowEdge === "right" ? root.width - width : 0
        y: root.innerShadowEdge === "bottom" ? root.height - height : 0
        gradient: Gradient {
            orientation: shade.horizontal ? Gradient.Horizontal : Gradient.Vertical
            GradientStop {
                position: 0.0
                color: root.innerShadowEdge === "left" || root.innerShadowEdge === "top"
                       ? AppTheme.shadowSoft : "transparent"
            }
            GradientStop {
                position: 1.0
                color: root.innerShadowEdge === "left" || root.innerShadowEdge === "top"
                       ? "transparent" : AppTheme.shadowSoft
            }
        }
    }
}
