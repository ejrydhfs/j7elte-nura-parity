import QtQuick
import QtQuick.Window
Window {
    width: 720; height: 1280; visible: true; color: "black"
    Grid {
        columns: 2; rows: 2; width: 720; height: 1280
        Rectangle { width: 360; height: 640; color: "#ff0000" }
        Rectangle { width: 360; height: 640; color: "#00ff00" }
        Rectangle { width: 360; height: 640; color: "#0000ff" }
        Rectangle { width: 360; height: 640; color: "#ffffff" }
    }
}
