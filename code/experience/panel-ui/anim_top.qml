import QtQuick
import QtQuick.Window
import org.kde.layershell 1.0 as LayerShell
Window {
    visible: true
    width: 720; height: 1280
    color: "#ff00ff"
    LayerShell.Window.layer: LayerShell.Window.LayerTop
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop
    LayerShell.Window.exclusionZone: 0
    ColorAnimation on color { from: "#ff00ff"; to: "#00ff00"; duration: 600; loops: Animation.Infinite }
}
