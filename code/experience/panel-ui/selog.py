p="/home/user/cs21.sh"
s=open(p).read()
s=s.replace('export QT_LOGGING_RULES="*=true"','export QT_LOGGING_RULES="kwin_core.debug=true;qt.scenegraph.renderloop=true"')
open(p,"w").write(s)
print("logging narrowed")
