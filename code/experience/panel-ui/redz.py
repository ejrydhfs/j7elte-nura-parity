for p in ("/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml",
          "/data/build/plasma-mobile-6.8.3/containments/taskpanel/qml/main.qml"):
    s = open(p).read().replace("z: -1", "z: 5")
    open(p, "w").write(s)
print("ok")
