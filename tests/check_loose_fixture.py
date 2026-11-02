"""The loose fixture, read by OpenUSD's Python API: what usd.elf is checked against.

    python3 tests/check_loose_fixture.py tests/loose

Exits 1 unless the stage has 6 prims, one mesh, one material and a texture that resolves to a real file.
"""
import os
import sys

from pxr import Usd, UsdGeom, UsdShade

root = os.path.join(sys.argv[1], "root.usda")
stage = Usd.Stage.Open(root)
prims = [str(p.GetPath()) for p in stage.Traverse()] if stage else []
meshes = [p for p in (stage.Traverse() if stage else []) if p.IsA(UsdGeom.Mesh)]
materials = [p for p in (stage.Traverse() if stage else []) if p.IsA(UsdShade.Material)]
texture = ""
if stage:
    tex = stage.GetPrimAtPath("/World/Looks/Mat/Tex")
    if tex:
        value = UsdShade.Shader(tex).GetInput("file").Get()
        texture = value.resolvedPath if value else ""
ok = len(prims) == 6 and len(meshes) == 1 and len(materials) == 1 and texture != "" and os.path.isfile(texture)
print("prims=%d meshes=%d materials=%d texture=%r" % (len(prims), len(meshes), len(materials), texture))
print("RESULT: %s" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
