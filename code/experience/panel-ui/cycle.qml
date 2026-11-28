import QtQuick
import QtQuick.Window
Window {
  id: w; width:720; height:1280; visible:true
  property var cols: ["#ff0000","#00ff00","#0000ff","#ffffff"]
  property int i: 0
  color: cols[i]
  Timer { interval: 6000; running: true; repeat: true; onTriggered: w.i = (w.i+1)%4 }
}
