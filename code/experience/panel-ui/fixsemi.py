p="/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml"
s=open(p).read()
s=s.replace('panelState = "default";;','panelState = "default";')
open(p,"w").write(s)
print("fixed" if '";;' not in s else "still-double")
