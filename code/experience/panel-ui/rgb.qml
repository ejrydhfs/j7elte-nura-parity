import QtQuick
import QtQuick.Window
Window {
    visible: true
    width: 720; height: 1280
    color: "#ff0000"
    SequentialAnimation on color {
        loops: Animation.Infinite
        ColorAnimation { to: "#ff0000"; duration: 1500 }
        ColorAnimation { to: "#00ff00"; duration: 1500 }
        ColorAnimation { to: "#0000ff"; duration: 1500 }
    }
}
