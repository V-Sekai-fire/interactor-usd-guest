import json, sys, numpy as np, trimesh
S = sys.argv[1]
z = np.load(f'{S}/finch/rigid_soft_poly.npz')
soft = z['soft']
sc = trimesh.load(f'{S}/finch/cobalt-finch.glb')
geoms = list(sc.geometry.values())
pieces, off, vo = [], 0, 0
allp, allu, allf = [], [], []
for i, g in enumerate(geoms):
    f = np.asarray(g.faces, np.int32); p = np.asarray(g.vertices, np.float32)
    uv = np.asarray(g.visual.uv, np.float32)
    lab = soft[off:off+len(f)].astype(np.int32)
    pieces.append((f'finch_part{i}', p, uv, f, lab))
    allp.append(p); allu.append(uv); allf.append(f+vo); vo += len(p); off += len(f)
assert off == len(soft), (off, len(soft))
T = z['T']; F = np.concatenate(allf)
print('T equals concatenated faces:', bool((T == F).all()))
pieces.insert(0, ('finch', np.concatenate(allp), np.concatenate(allu), F, soft.astype(np.int32)))
man = []
for name, p, uv, f, lab in pieces:
    p.tofile(f'{S}/out/in/{name}.p'); uv.tofile(f'{S}/out/in/{name}.uv')
    f.astype(np.int32).tofile(f'{S}/out/in/{name}.f'); lab.tofile(f'{S}/out/in/{name}.l')
    man.append(dict(name=name, np=len(p), nf=len(f), soft=int(lab.sum())))
json.dump(man, open(f'{S}/out/in/manifest.json', 'w'))
print(man[0], len(man))
