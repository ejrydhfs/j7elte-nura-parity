import QtQuick
import QtQuick.Window
import org.kde.layershell 1.0 as LayerShell

Window {
    visible: true
    width: 720
    height: 200
    color: "#ff00ff"
    LayerShell.Window.layer: LayerShell.Window.LayerTop
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop
    LayerShell.Window.exclusionZone: 0
    onVisibilityChanged: console.log("VIS", visibility)
    Component.onCompleted: console.log("INIT", visibility)
}
