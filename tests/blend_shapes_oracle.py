"""Check usd.elf's blend shapes and clip weights against OpenUSD's own Python API (usd-core).

    python tests/blend_shapes_oracle.py <file.usda> <dump.txt> [--plant]

<dump.txt> holds the "DUMP {json}" line from tests/blend_shapes_dump.gd. The expected arrays
follow interactor-flow's MeshConverter.h: each target, and each authored in-between with a
weight in (0, 1), densified to one offset per vertex of the mesh's point table (zeros where a
sparse target has no pointIndices entry; a mesh the guest keeps per face corner, because its
normals or UVs are faceVarying, repeats each point's offset at every corner of it); a clip's frames are the union of its joint and
blend-shape-weight time samples. --plant changes one expected value; the check must then fail.
Exit 0 and CHECK-OK on a match.
"""
import json
import sys

import numpy as np
from pxr import Usd, UsdGeom, UsdSkel


def dense(vals, pi, npts):
    out = np.zeros((npts, 3), np.float32)
    vals = np.asarray(vals, np.float32).reshape(-1, 3)
    if len(pi):
        out[np.asarray(pi)] = vals
    else:
        out[:] = vals
    return out


def expected(path, corner_meshes):
    stage = Usd.Stage.Open(path)
    shapes, anims = [], []
    mesh_index = 0
    for prim in stage.Traverse():
        if prim.IsA(UsdSkel.Animation):
            q = UsdSkel.Cache().GetAnimQuery(prim)
            times = sorted(set(q.GetJointTransformTimeSamples()) | set(q.GetBlendShapeWeightTimeSamples()))
            order = list(q.GetBlendShapeOrder())
            w = [float(x) for t in times for x in (q.ComputeBlendShapeWeights(Usd.TimeCode(t)) if order else [])]
            anims.append({"path": str(prim.GetPath()), "frames": len(times), "blend_shapes": order, "weights": w})
            continue
        if not prim.IsA(UsdGeom.Mesh):
            continue
        mesh = UsdGeom.Mesh(prim)
        npts = len(mesh.GetPointsAttr().Get())
        fvi = np.asarray(mesh.GetFaceVertexIndicesAttr().Get(), np.int64)
        per_vertex = (lambda a: a[fvi]) if str(prim.GetPath()) in corner_meshes else (lambda a: a)
        b = UsdSkel.BindingAPI(prim)
        names = b.GetBlendShapesAttr().Get() if prim.HasAPI(UsdSkel.BindingAPI) else None
        if names:
            targets = b.GetBlendShapeTargetsRel().GetTargets()
            for name, tp in zip(names, targets):
                bs = UsdSkel.BlendShape(stage.GetPrimAtPath(tp))
                pi = bs.GetPointIndicesAttr().Get() or []
                nrm = bs.GetNormalOffsetsAttr().Get()
                primary = len(shapes)
                shapes.append({"name": name, "mesh": mesh_index, "primary": -1, "position": 1.0,
                               "offsets": per_vertex(dense(bs.GetOffsetsAttr().Get(), pi, npts)),
                               "normals": per_vertex(dense(nrm, pi, npts)) if nrm else None})
                for ib in bs.GetAuthoredInbetweens():
                    pos = ib.GetWeight()
                    if not (0.0 < pos < 1.0):
                        continue
                    inrm = ib.GetNormalOffsets()
                    shapes.append({"name": f"{name}:{ib.GetAttr().GetBaseName()}", "mesh": mesh_index,
                                   "primary": primary, "position": pos,
                                   "offsets": per_vertex(dense(ib.GetOffsets(), pi, npts)),
                                   "normals": per_vertex(dense(inrm, pi, npts)) if inrm else None})
        mesh_index += 1
    return shapes, anims


def floats(v):
    """The guest's array, or None when it answered an error string instead."""
    return None if isinstance(v, str) else np.asarray(v, np.float32)


def main():
    src, dump_path = sys.argv[1], sys.argv[2]
    line = next(x for x in open(dump_path, encoding="utf-8") if x.startswith("DUMP "))
    got = json.loads(line[5:])
    shapes, anims = expected(src, {m["path"] for m in got["meshes"] if m["indexed"] is False})
    if "--plant" in sys.argv:
        if shapes:
            shapes[-1]["offsets"][-1][0] += 0.25
        elif anims and anims[-1]["weights"]:
            anims[-1]["weights"][-1] += 0.25
        else:  # nothing to change: expect one clip more than the file has
            anims.append({"path": "/planted", "frames": 1, "blend_shapes": [], "weights": []})
    bad = []
    if not got["open"].startswith("ok "):
        bad.append(f"open: {got['open']}")
    if len(got["shapes"]) != len(shapes):
        bad.append(f"blend shapes: guest {len(got['shapes'])}, OpenUSD {len(shapes)}")
    for g, e in zip(got["shapes"], shapes):
        for k in ("name", "mesh", "primary"):
            if g[k] != e[k]:
                bad.append(f"shape {e['name']}: {k} {g[k]!r} vs {e[k]!r}")
        if abs(g["position"] - e["position"]) > 1e-7:
            bad.append(f"shape {e['name']}: position {g['position']} vs {e['position']}")
        go = floats(g["offsets"])
        if go is None or not np.array_equal(go.reshape(-1, 3), e["offsets"]):
            bad.append(f"shape {e['name']}: offsets differ")
        if (e["normals"] is None) == g["has_normals"]:
            bad.append(f"shape {e['name']}: has_normals {g['has_normals']}")
        elif e["normals"] is not None and (floats(g["normals"]) is None or not np.array_equal(
                floats(g["normals"]).reshape(-1, 3), e["normals"])):
            bad.append(f"shape {e['name']}: normal offsets differ")
    if len(got["anims"]) != len(anims):
        bad.append(f"clips: guest {len(got['anims'])}, OpenUSD {len(anims)}")
    for g, e in zip(got["anims"], anims):
        if g["frames"] != e["frames"]:
            bad.append(f"clip {e['path']}: frames {g['frames']} vs {e['frames']}")
        if g["blend_shapes"].split() != e["blend_shapes"]:
            bad.append(f"clip {e['path']}: blend shapes {g['blend_shapes'].split()} vs {e['blend_shapes']}")
        gw = floats(g["weights"])
        if gw is None:
            bad.append(f"clip {e['path']}: weights: {g['weights']}")
        elif not np.array_equal(gw, np.asarray(e["weights"], np.float32)):
            bad.append(f"clip {e['path']}: weights differ")
    for b in bad[:10]:
        print("MISMATCH", b)
    print(("CHECK-OK" if not bad else "CHECK-FAIL"), f"{src}: {len(shapes)} shapes, {len(anims)} clips, {len(bad)} mismatches")
    sys.exit(0 if not bad else 1)


if __name__ == "__main__":
    main()
