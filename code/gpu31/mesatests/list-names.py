import gzip, re
for profile in ["quick_shader", "quick_gl"]:
    p = "/data/pigroot/usr/lib/piglit/tests/%s.xml.gz" % profile
    d = gzip.open(p, "rt").read()
    names = re.findall(r'<Test[^>]* name="([^"]+)"', d)
    open("/data/mesatests/%s-names.txt" % profile, "w").write("\n".join(names))
    print(profile, len(names))
    for n in names[:12]:
        print("  ", n)
