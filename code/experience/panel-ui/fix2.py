p="/data/build/plasma-mobile-6.8.3/containments/taskpanel/qml/main.qml"
s=open(p).read().replace(";;",";")
open(p,"w").write(s)
print("fixed")
