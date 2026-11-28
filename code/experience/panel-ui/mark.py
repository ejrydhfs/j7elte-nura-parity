b = "/data/build/plasma-mobile-6.8.3/containments"
tp = b + "/taskpanel/qml/main.qml"
s = open(tp).read()
s = s.replace('color: "#ff0000"\n        z: 5', 'color: "#00ff00"\n        z: 5', 1)
open(tp, "w").write(s)
pp = b + "/panel/qml/main.qml"
p = open(pp).read()
p = p.replace('color: "#00ff00"; z: -100', 'color: "#0000ff"; z: -100', 1)
open(pp, "w").write(p)
print("taskpanel marker -> green, panel fill -> blue")
