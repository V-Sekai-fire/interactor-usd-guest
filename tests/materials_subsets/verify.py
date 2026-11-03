"""Gates for the finch .usdz pieces. Usage: verify.py <out dir> [guest readback log]
Each gate has a control that must fail."""
import json, re, sys, os, zipfile, numpy as np
from pxr import Usd, UsdGeom, UsdShade

out = sys.argv[1]
man = json.load(open(f'{out}/in/manifest.json'))
LIC = dict(author='Abcxyz51', license='CC-BY-4.0', author_url='https://sketchfab.com/Abcxyz51')


def read(path):
    st = Usd.Stage.Open(path)
    if not st:
        raise RuntimeError('does not open')
    mesh = [p for p in st.Traverse() if p.IsA(UsdGeom.Mesh)]
    assert len(mesh) == 1, len(mesh)
    m = UsdGeom.Mesh(mesh[0])
    counts = m.GetFaceVertexCountsAttr().Get()
    r = dict(points=len(m.GetPointsAttr().Get()), faces=len(counts), all_tri=bool((np.array(counts) == 3).all()),
             materials=len([p for p in st.Traverse() if p.IsA(UsdShade.Material)]),
             bound=bool(UsdShade.MaterialBindingAPI(m.GetPrim()).ComputeBoundMaterial()[0]))
    sub = {s.GetPrim().GetName(): list(s.GetIndicesAttr().Get()) for s in UsdGeom.Subset.GetGeomSubsets(m, 'face', 'rigidSoft')}
    r['subsets'] = {k: len(v) for k, v in sub.items()}
    r['subset_idx'] = sub
    r['partition_valid'] = UsdGeom.Subset.ValidateFamily(m, 'face', 'rigidSoft')[0] if sub else None
    r['cld'] = dict(st.GetRootLayer().customLayerData)
    r['doc'] = st.GetRootLayer().documentation
    sh = [p for p in st.Traverse() if p.GetTypeName() == 'Shader']
    r['surface'] = {}
    if sh:
        s = UsdShade.Shader(sh[0])
        r['surface'] = dict(id=s.GetIdAttr().Get(), **{i.GetBaseName(): i.Get() for i in s.GetInputs()})
    return r


def check(path, e, label):
    """Returns a list of mismatches against expectation e."""
    bad = []
    try:
        r = read(path)
    except Exception as x:
        return [f'open: {x}']
    for k in ('points', 'faces', 'materials'):
        if r[k] != e[k]:
            bad.append(f'{k} {r[k]} != {e[k]}')
    if r['all_tri'] is not True:
        bad.append('not all triangles')
    if r['bound'] != (e['materials'] > 0):
        bad.append('binding mismatch')
    if e.get('soft') is not None:
        if r['subsets'] != {'rigid': e['faces'] - e['soft'], 'soft': e['soft']}:
            bad.append(f"subsets {r['subsets']}")
        if sum(r['subsets'].values()) != r['faces']:
            bad.append('subset sum != faces')
        if r['partition_valid'] is not True:
            bad.append('partition invalid')
    for k, v in LIC.items():
        if r['cld'].get(k) != v:
            bad.append(f'cld {k}')
    if e['materials'] and r['surface'].get('id') != 'UsdPreviewSurface':
        bad.append('no UsdPreviewSurface')
    return bad


fails = 0
def gate(name, ok):
    global fails
    print(('PASS ' if ok else 'FAIL ') + name)
    fails += (not ok)

def must_fail(name, bad):
    gate('control (must fail): ' + name, bool(bad))
    if not bad:
        print('  CONTROL-BROKEN')

exp_mat = {m['name']: dict(points=m['np'], faces=m['nf'], materials=1, soft=m['soft']) for m in man}
exp_nomat = {k: dict(v, materials=0) for k, v in exp_mat.items()}
tot_soft = 0
for m in man:
    n = m['name']
    b1 = check(f'{out}/{n}.usdz', exp_mat[n], n)
    b2 = check(f'{out}/{n}.nomat.usdz', exp_nomat[n], n)
    gate(f'{n}.usdz: points {m["np"]}, tris {m["nf"]}, 1 material, rigid {m["nf"]-m["soft"]}/soft {m["soft"]}, licence {b1 or "ok"}', not b1)
    gate(f'{n}.nomat.usdz: 0 materials, same geometry/subsets {b2 or "ok"}', not b2)

# exact subset membership vs the npz labels (whole finch)
lab = np.fromfile(f'{out}/in/finch.l', np.int32)
r = read(f'{out}/finch.usdz')
soft_idx = np.array(r['subset_idx']['soft']); rigid_idx = np.array(r['subset_idx']['rigid'])
gate('finch soft subset == npz soft labels, exact membership', (np.sort(soft_idx) == np.nonzero(lab == 1)[0]).all() and (np.sort(rigid_idx) == np.nonzero(lab == 0)[0]).all())
gate('finch 79,683 triangles; subsets sum 79,683', r['faces'] == 79683 and sum(r['subsets'].values()) == 79683)
gate(f"finch material values {r['surface']}", abs(r['surface']['roughness'] - 0.6) < 1e-6 and abs(r['surface']['metallic'] - 0.1) < 1e-6 and tuple(round(c, 4) for c in r['surface']['diffuseColor']) == (0.15, 0.35, 0.75))
gate('finch layer documentation carries licence', 'CC-BY-4.0' in r['doc'] and 'Abcxyz51' in r['doc'])
parts = [m for m in man if m['name'] != 'finch']
gate('pieces sum to the whole: tris and soft', sum(m['nf'] for m in parts) == 79683 and sum(m['soft'] for m in parts) == man[0]['soft'])

# controls
must_fail('with-material package expected to have 0 materials', check(f'{out}/finch.usdz', exp_nomat['finch'], 'x'))
must_fail('no-material package expected to have 1 material', check(f'{out}/finch.nomat.usdz', exp_mat['finch'], 'x'))
must_fail('planted triangle count +1', check(f'{out}/finch.usdz', dict(exp_mat['finch'], faces=79684), 'x'))
must_fail('planted soft count +1', check(f'{out}/finch.usdz', dict(exp_mat['finch'], soft=exp_mat['finch']['soft'] + 1), 'x'))
must_fail('planted wrong point count', check(f'{out}/finch.usdz', dict(exp_mat['finch'], points=1), 'x'))
data = open(f'{out}/finch.usdz', 'rb').read()
open(f'{out}/_trunc.usdz', 'wb').write(data[:len(data) // 2])
must_fail('truncated package must not open/verify', check(f'{out}/_trunc.usdz', exp_mat['finch'], 'x'))
os.remove(f'{out}/_trunc.usdz')

# guest readback (usd.elf's own reader)
if len(sys.argv) > 2:
    rb = {}
    for ln in open(sys.argv[2]):
        if ln.startswith('RB '):
            d = json.loads(ln[3:]); rb[d['file']] = d
    for m in man:
        n = m['name']
        a, b = rb.get(n + '.usdz'), rb.get(n + '.nomat.usdz')
        ok = bool(a and b) and a['meshes'] == 1 and a['points'] == m['np'] and a['triangles'] == m['nf'] and a['materials'] == 1 and a['material'] == 0 \
            and a['subset_count'] == 2 and sorted(a['subsets'].split()) == sorted([f"rigidSoft/rigid={m['nf']-m['soft']}", f"rigidSoft/soft={m['soft']}"]) \
            and a['layer_data'].get('author') == 'Abcxyz51' and a['layer_data'].get('license') == 'CC-BY-4.0'
        okb = bool(b) and b['materials'] == 0 and b['points'] == m['np'] and b['triangles'] == m['nf'] and b['subset_count'] == 2 and b['material'] == -1
        gate(f'usd.elf reader {n}: pts/tris/1 material/2 subsets/licence', ok)
        gate(f'usd.elf reader {n}.nomat: 0 materials', okb)
    f = rb['finch.usdz']
    must_fail('usd.elf reader on with-material finch, expecting 0 materials', [] if f['materials'] == 0 else ['materials'])
    g = rb['finch.nomat.usdz']
    must_fail('usd.elf reader on no-material finch, expecting 1 material', [] if g['materials'] == 1 else ['materials'])
    must_fail('usd.elf reader finch, planted 79684 triangles', [] if f['triangles'] == 79684 else ['tris'])
print('FAILS', fails)
sys.exit(1 if fails else 0)
