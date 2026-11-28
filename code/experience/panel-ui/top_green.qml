import QtQuick
import QtQuick.Window
import org.kde.layershell 1.0 as LayerShell
Window {
    width: 720; height: 640; visible: true; color: "#00ff00"
    LayerShell.Window.layer: LayerShell.Window.LayerTop
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop
    LayerShell.Window.exclusionZone: 0
}
