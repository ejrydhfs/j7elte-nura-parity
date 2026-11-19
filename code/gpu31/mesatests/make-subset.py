#!/usr/bin/env python3
"""Build gpu31 piglit subset from quick_gl only (single profile run).
Deterministic: sorted, capped, no duplicates."""
import os

OUT = "/data/mesatests"
qg = open(os.path.join(OUT, "quick_gl-names.txt")).read().splitlines()
uniq = sorted(set(qg))

want_shader = ["shaders@glsl-vs-", "shaders@glsl-fs-", "shaders@glsl-uniform",
               "shaders@glsl-varying", "shaders@glsl-builtin",
               "shaders@glsl-invariant", "shaders@glsl-derivatives",
               "shaders@glsl-loop", "shaders@glsl-if", "shaders@glsl-array",
               "shaders@glsl-fs-loop", "shaders@glsl-vs-loop"]
want_texture = ["texture", "tex-", "sampler", "mipmap", "fbo-generatemipmap",
                "compressed", "cubemap", "npot", "s3tc"]
want_depth = ["depth", "stencil", "scissor", "early-z", "hiz", "clear"]

sel = []
for prefix in want_shader:
    sel += [t for t in uniq if prefix in t][:3]
for key in want_texture:
    sel += [t for t in uniq if key in t.lower()][:4]
for key in want_depth:
    sel += [t for t in uniq if key in t.lower()][:4]

seen, subset = set(), []
for t in sel:
    if t not in seen and "wgl" not in t and "cl@" not in t and "vulkan" not in t:
        seen.add(t)
        subset.append(t)

subset = subset[:60]
open(os.path.join(OUT, "piglit-subset.txt"), "w").write("\n".join(subset) + "\n")
print("subset:", len(subset))
for t in subset:
    print("  ", t)
