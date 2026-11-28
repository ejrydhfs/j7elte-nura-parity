import QtQuick

Item {
    width: 720
    height: 720
    visible: true

    Rectangle {
        id: box
        width: 240
        height: 240
        color: "#c03030"
        anchors.centerIn: parent

        NumberAnimation on rotation {
            from: 0; to: 360; duration: 1200
            loops: Animation.Infinite
            running: true
        }
    }

    Text {
        id: tick
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        color: "white"
        font.pixelSize: 48
        property int n: 0
        text: n
        Timer {
            interval: 16; running: true; repeat: true
            onTriggered: tick.n = (tick.n + 1) % 1000
        }
    }
}
