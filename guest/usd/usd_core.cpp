// See usd_core.h. The only pxr-facing TU of usd.elf besides mem_resolver.cpp.
//
// The package crosses as one asset: usdmem::put_asset("/mem/pkgN.usdz")
// holds the zip bytes, SdfLayer::FindOrOpen picks SdfUsdzFileFormat by the
// extension, Sdf_UsdzResolver (an ArPackageResolver, in sdf's plugInfo.json)
// opens the package through our resolver and reads its members
// (asset.usdc, the textures) as sub-ranges of that one buffer. Nothing is
// unpacked, and no member is copied until it is asked for.
//
// A design note on the org's idtx-flow: its converter core
// (shared/include/idtxflow/converter/MeshConverter.h, MaterialConverter.h)
// reads the same UsdGeomMesh / UsdShade data but is a C++20 concept-templated
// header library over a TargetEngine type set (TargetTypes.h, TypeConverter.h,
// MdlMaterialConverter.h, usdSkel queries) and densifies every mesh to one
// vertex per face corner. Gate 0G's pxr-facing TU has to be gnu++17, so the
// reads are redone here on the USD API directly, with the same fan
// triangulation and the same UsdPreviewSurface inputs.

#include "usd_core.h"

#include "mem_resolver.h"
#include "common/blake3.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/pathUtils.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/dictionary.h"
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/packageUtils.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdGeom/xformCache.h"
#include "pxr/usd/usdShade/connectableAPI.h"
#include "pxr/usd/usdShade/input.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usdShade/tokens.h"
#include "pxr/usd/usdSkel/animQuery.h"
#include "pxr/usd/usdSkel/animation.h"
#include "pxr/usd/usdSkel/binding.h"
#include "pxr/usd/usdSkel/bindingAPI.h"
#include "pxr/usd/usdSkel/cache.h"
#include "pxr/usd/usdSkel/root.h"
#include "pxr/usd/usdSkel/skeleton.h"
#include "pxr/usd/usdSkel/skeletonQuery.h"
#include "pxr/usd/usdSkel/skinningQuery.h"
#include "pxr/usd/usdSkel/topology.h"
#include "pxr/usd/usdSkel/utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// --- the flat tables (the only state kept across vmcalls) ------------------

struct Span {
	size_t off = 0, len = 0;
};

struct MeshRec {
	Span path, name;
	size_t pt_off = 0, npts = 0; // into g_points (3 floats a point)
	size_t nrm_off = 0; // into g_normals (3 floats a point), valid when has_normals
	size_t uv_off = 0; // into g_uvs (2 floats a point), valid when has_uvs
	size_t idx_off = 0, ntris = 0; // into g_indices (3 ints a triangle)
	bool has_normals = false, has_uvs = false, indexed = true;
	int material = -1;
	Span b3_p, b3_i; // BLAKE3 hex of the f32 point / i32 corner bytes, in g_strings
	float xf[16] = {};
	int skin = -1; // into g_skins
	bool double_sided = false;
	size_t surf_off = 0, nsurf = 0; // into g_surfaces, 3 ints a surface
};

struct SkelRec {
	Span path, joints, names, anim;
	size_t njoints = 0;
	size_t par_off = 0; // into g_skel_parents
	size_t xf_off = 0; // into g_skel_bind and g_skel_rest, 12 floats a joint
	bool rest_from_bind = false, bind_from_rest = false;
	float xf[16] = {};
};

struct SkinRec {
	int skel = -1, element = 0, nonzero = 0;
	Span interp, joints, method, b3_i, b3_w;
	size_t off = 0; // into g_skin_idx / g_skin_w, element a vertex
	float geom[16] = {};
};

// One curve of a BasisCurves prim: a stroke. A prim with N curveVertexCounts
// gives N records, named <prim>_<k> when N > 1.
struct CurveRec {
	Span path, name;
	size_t pt_off = 0, npts = 0; // into g_cpoints (3 floats a point)
	bool boundary = false; // primvars:boundary authored true on the prim
	Span b3_p;
	float xf[16] = {};
};

struct MatRec {
	Span path, name, shader;
	float diffuse[3] = { 0.18f, 0.18f, 0.18f };
	float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
	int tex[5] = { -1, -1, -1, -1, -1 }; // diffuse, metallic, roughness, opacity, normal
	Span channel[5];
	Span file[5]; // the connected UsdUVTexture's authored file, resolved or not
};

struct TexRec {
	Span path, file;
	size_t data_off = 0, size = 0;
};

std::string g_strings;
std::vector<float> g_points, g_normals, g_uvs;
std::vector<int32_t> g_indices;
std::vector<uint8_t> g_texdata;
std::vector<MeshRec> g_meshes;
std::vector<int32_t> g_surfaces;
std::vector<std::string> g_tex_missing; // authored files that did not resolve or open, once each
std::vector<float> g_cpoints;
std::vector<CurveRec> g_curves;
std::vector<std::pair<std::string, std::string>> g_layer_data; // customLayerData, values as text
std::vector<MatRec> g_mats;
std::vector<TexRec> g_texs;
// UsdSkelAnimation prims (motion clips, RFD 2277 A2): joints as one text
// blob, parent-local transforms per time sample in g_animxf (12 floats a
// joint: row-major 3x3 with column-vector convention, then the translation).
struct AnimRec {
	Span path, joints;
	size_t xf_off = 0, frames = 0, njoints = 0;
	double fps = 0.0;
};
std::vector<AnimRec> g_anims;
std::vector<float> g_animxf;
std::vector<SkelRec> g_skels;
std::vector<int32_t> g_skel_parents;
std::vector<float> g_skel_bind, g_skel_rest;
std::map<std::string, int> g_skel_by_path;
std::vector<SkinRec> g_skins;
std::vector<int32_t> g_skin_idx;
std::vector<float> g_skin_w;
std::string g_pkg_path; // the current asset's path in the resolver ("" when closed)

Span intern(const std::string &s) {
	Span sp{ g_strings.size(), s.size() };
	g_strings += s;
	return sp;
}

std::string str(const Span &s) {
	return g_strings.substr(s.off, s.len);
}

std::string first_error(const TfErrorMark &m) {
	for (auto it = m.GetBegin(); it != m.GetEnd(); ++it)
		return it->GetCommentary();
	return "unknown";
}

// --- textures ---------------------------------------------------------------

std::map<std::string, int> g_tex_by_path; // resolved path -> table index (per open)

// Resolve a shader's file asset path, read the bytes once, return its index
// (-1 when it does not resolve or is empty).
int add_texture(const SdfAssetPath &ap, const std::string &layer_path, std::string &why) {
	std::string resolved = ap.GetResolvedPath();
	const std::string authored = ap.GetAssetPath();
	if (resolved.empty() && !authored.empty()) {
		ArResolver &r = ArGetResolver();
		resolved = r.Resolve(authored).GetPathString();
		if (resolved.empty() && TfStringEndsWith(layer_path, ".usdz") && authored[0] != '/')
			resolved = ArJoinPackageRelativePath(layer_path, TfNormPath(authored));
	}
	auto missing = [&](const std::string &w) {
		why = w;
		if (std::find(g_tex_missing.begin(), g_tex_missing.end(), authored) == g_tex_missing.end())
			g_tex_missing.push_back(authored);
		return -1;
	};
	if (resolved.empty())
		return missing("texture '" + authored + "' does not resolve");
	auto it = g_tex_by_path.find(resolved);
	if (it != g_tex_by_path.end())
		return it->second;
	std::shared_ptr<ArAsset> asset = ArGetResolver().OpenAsset(ArResolvedPath(resolved));
	if (!asset)
		return missing("texture '" + resolved + "' cannot be opened");
	const size_t n = asset->GetSize();
	std::shared_ptr<const char> buf = asset->GetBuffer();
	if (!buf || n == 0)
		return missing("texture '" + resolved + "' is empty");
	TexRec t;
	t.path = intern(resolved);
	t.file = intern(authored);
	t.data_off = g_texdata.size();
	t.size = n;
	g_texdata.insert(g_texdata.end(), buf.get(), buf.get() + n);
	g_texs.push_back(t);
	g_tex_by_path[resolved] = (int)g_texs.size() - 1;
	return (int)g_texs.size() - 1;
}

// --- materials --------------------------------------------------------------

std::map<std::string, int> g_mat_by_path;

// One UsdPreviewSurface input: a constant (kept in *value) or a connected
// UsdUVTexture (its file becomes a texture, channel = the connected output).
template <typename T>
void read_input(const UsdShadeShader &surface, const char *name, const std::string &layer_path, T *value,
		int &tex, Span &channel, Span &file_span, std::string &warn) {
	UsdShadeInput in = surface.GetInput(TfToken(name));
	if (!in)
		return;
	UsdShadeConnectableAPI src;
	TfToken src_name;
	UsdShadeAttributeType src_type;
	if (in.GetConnectedSource(&src, &src_name, &src_type)) {
		UsdShadeShader texsh(src.GetPrim());
		if (!texsh)
			return;
		UsdShadeInput file = texsh.GetInput(TfToken("file"));
		SdfAssetPath ap;
		if (!file || !file.Get(&ap)) {
			warn = std::string(name) + ": connected shader has no file input";
			return;
		}
		std::string why;
		file_span = intern(ap.GetAssetPath());
		tex = add_texture(ap, layer_path, why);
		if (tex < 0)
			warn = std::string(name) + ": " + why;
		channel = intern(src_name.GetString());
		// A UsdUVTexture's fallback stands in for a texture that did not resolve.
		if (tex < 0 && value) {
			UsdShadeInput fb = texsh.GetInput(TfToken("fallback"));
			GfVec4f f;
			if (fb && fb.Get(&f)) {
				if constexpr (std::is_same<T, GfVec3f>::value)
					*value = GfVec3f(f[0], f[1], f[2]);
				else
					*value = src_name == TfToken("r") ? f[0] : src_name == TfToken("g") ? f[1] : src_name == TfToken("b") ? f[2] : f[3];
			}
		}
		return;
	}
	if (value)
		in.Get(value);
}

int add_material(const UsdShadeMaterial &mat, const std::string &layer_path, std::string &warn) {
	const std::string path = mat.GetPath().GetString();
	auto it = g_mat_by_path.find(path);
	if (it != g_mat_by_path.end())
		return it->second;
	MatRec m;
	m.path = intern(path);
	m.name = intern(mat.GetPrim().GetName().GetString());
	UsdShadeShader surface = mat.ComputeSurfaceSource();
	if (surface) {
		TfToken id;
		surface.GetShaderId(&id);
		m.shader = intern(id.GetString());
		GfVec3f diffuse(m.diffuse[0], m.diffuse[1], m.diffuse[2]);
		read_input(surface, "diffuseColor", layer_path, &diffuse, m.tex[0], m.channel[0], m.file[0], warn);
		read_input(surface, "metallic", layer_path, &m.metallic, m.tex[1], m.channel[1], m.file[1], warn);
		read_input(surface, "roughness", layer_path, &m.roughness, m.tex[2], m.channel[2], m.file[2], warn);
		read_input(surface, "opacity", layer_path, &m.opacity, m.tex[3], m.channel[3], m.file[3], warn);
		read_input<GfVec3f>(surface, "normal", layer_path, nullptr, m.tex[4], m.channel[4], m.file[4], warn);
		m.diffuse[0] = diffuse[0];
		m.diffuse[1] = diffuse[1];
		m.diffuse[2] = diffuse[2];
	} else {
		m.shader = intern("");
	}
	g_mats.push_back(m);
	g_mat_by_path[path] = (int)g_mats.size() - 1;
	return (int)g_mats.size() - 1;
}

// --- meshes -----------------------------------------------------------------

// Per-point (vertex/varying) or per-corner (faceVarying) values of a primvar
// or the normals attribute, flattened; interp says which. Empty when absent.
template <typename ArrayT>
bool read_pv(const UsdGeomPrimvar &pv, ArrayT *out, TfToken *interp) {
	if (!pv || !pv.HasValue())
		return false;
	*interp = pv.GetInterpolation();
	return pv.ComputeFlattened(out, UsdTimeCode::Default()) && !out->empty();
}

void matrix_to16(const GfMatrix4d &m, float *o) {
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			o[i * 4 + j] = (float)m[i][j];
}

struct SkinSrc {
	UsdSkelSkinningQuery query;
	int skel = -1;
};

// The mesh's joint influences, one run of elementSize per vertex of its point
// table, joint indices mapped into the skeleton's order by the query's own
// mapper (skel:joints). An index out of range, or a weight on a joint the
// skeleton does not have, is refused.
std::string add_skin(const SkinSrc &src, size_t usd_points, const VtIntArray &fvi, MeshRec &r) {
	const UsdSkelSkinningQuery &q = src.query;
	if (!q.HasJointInfluences())
		return "";
	VtIntArray ji;
	VtFloatArray jw;
	if (!q.ComputeJointInfluences(&ji, &jw))
		return "joint influences do not compute";
	const int e = q.GetNumInfluencesPerComponent();
	const bool constant = q.GetInterpolation() == UsdGeomTokens->constant;
	const size_t want = (constant ? 1 : usd_points) * (size_t)(e > 0 ? e : 0);
	if (e <= 0 || ji.size() != want || jw.size() != want)
		return "skin: " + std::to_string(ji.size()) + " indices and " + std::to_string(jw.size()) + " weights, elementSize " +
				std::to_string(e) + " over " + std::to_string(usd_points) + " points (" + q.GetInterpolation().GetString() + ")";
	const SkelRec &s = g_skels[(size_t)src.skel];
	VtIntArray skel_order(s.njoints), to_skel;
	for (size_t j = 0; j < s.njoints; ++j)
		skel_order[j] = (int)j;
	const UsdSkelAnimMapperRefPtr &mapper = q.GetJointMapper();
	const int unmapped = -1;
	if (mapper && !mapper->IsIdentity())
		mapper->Remap(skel_order, &to_skel, 1, &unmapped);
	else
		to_skel = skel_order;
	VtTokenArray order;
	std::string jl;
	if (q.GetJointOrder(&order)) {
		for (const TfToken &t : order)
			jl += t.GetString() + "\n";
	} else {
		jl = str(s.joints);
	}
	for (size_t k = 0; k < ji.size(); ++k) {
		const int j = ji[k];
		if (j < 0 || (size_t)j >= to_skel.size())
			return "skin: joint index " + std::to_string(j) + " out of range (" + std::to_string(to_skel.size()) + " joints)";
		if (to_skel[(size_t)j] < 0 && jw[k] != 0.0f)
			return "skin: weight on joint " + std::to_string(j) + ", which the skeleton does not have";
	}
	SkinRec k;
	k.skel = src.skel;
	k.element = e;
	k.interp = intern(q.GetInterpolation().GetString());
	k.joints = intern(jl);
	TfToken method = UsdSkelTokens->classicLinear;
	if (q.GetSkinningMethodAttr())
		q.GetSkinningMethodAttr().Get(&method);
	k.method = intern(method.GetString());
	matrix_to16(q.GetGeomBindTransform(), k.geom);
	k.off = g_skin_idx.size();
	for (size_t v = 0; v < r.npts; ++v) {
		const size_t p = constant ? 0 : r.indexed ? v : (size_t)fvi[v];
		int nonzero = 0;
		for (int c = 0; c < e; ++c) {
			const int mapped = to_skel[(size_t)ji[p * e + c]];
			g_skin_idx.push_back(mapped < 0 ? 0 : mapped);
			g_skin_w.push_back(mapped < 0 ? 0.0f : jw[p * e + c]);
			nonzero += g_skin_w.back() != 0.0f;
		}
		k.nonzero = std::max(k.nonzero, nonzero);
	}
	const size_t n = r.npts * (size_t)e;
	k.b3_i = intern(blake3::hex(g_skin_idx.data() + k.off, n * sizeof(int32_t)));
	k.b3_w = intern(blake3::hex(g_skin_w.data() + k.off, n * sizeof(float)));
	r.skin = (int)g_skins.size();
	g_skins.push_back(k);
	return "";
}

std::string add_mesh(const UsdPrim &prim, UsdGeomXformCache &xc, const std::string &layer_path, const SkinSrc *skin,
		std::string &warn) {
	UsdGeomMesh mesh(prim);
	MeshRec r;
	r.path = intern(prim.GetPath().GetString());
	r.name = intern(prim.GetName().GetString());
	VtVec3fArray pts;
	VtIntArray fvc, fvi, holes;
	mesh.GetPointsAttr().Get(&pts, UsdTimeCode::Default());
	mesh.GetFaceVertexCountsAttr().Get(&fvc, UsdTimeCode::Default());
	mesh.GetFaceVertexIndicesAttr().Get(&fvi, UsdTimeCode::Default());
	mesh.GetHoleIndicesAttr().Get(&holes, UsdTimeCode::Default());
	size_t corners = 0;
	for (int c : fvc) {
		if (c < 0)
			return "faceVertexCounts has a negative count";
		corners += (size_t)c;
	}
	if (corners != fvi.size())
		return "faceVertexCounts sum " + std::to_string(corners) + " != faceVertexIndices " + std::to_string(fvi.size());
	for (int i : fvi)
		if (i < 0 || (size_t)i >= pts.size())
			return "faceVertexIndices out of range (" + std::to_string(i) + " of " + std::to_string(pts.size()) + " points)";

	// normals: primvars:normals wins over the normals attribute (UsdGeom's rule)
	UsdGeomPrimvarsAPI pvapi(prim);
	VtVec3fArray nrm;
	TfToken nrm_interp;
	if (!read_pv(pvapi.GetPrimvar(UsdGeomTokens->normals), &nrm, &nrm_interp)) {
		nrm.clear();
		if (mesh.GetNormalsAttr().Get(&nrm, UsdTimeCode::Default()) && !nrm.empty())
			nrm_interp = mesh.GetNormalsInterpolation();
	}
	// st: primvars:st, else the first texCoord2f / float2 primvar
	VtVec2fArray st;
	TfToken st_interp;
	if (!read_pv(pvapi.GetPrimvar(TfToken("st")), &st, &st_interp)) {
		st.clear();
		for (const UsdGeomPrimvar &pv : pvapi.GetPrimvarsWithValues()) {
			const SdfValueTypeName t = pv.GetTypeName();
			if (t == SdfValueTypeNames->TexCoord2fArray || t == SdfValueTypeNames->Float2Array) {
				if (read_pv(pv, &st, &st_interp))
					break;
				st.clear();
			}
		}
	}
	auto per_point_ok = [&](const TfToken &interp, size_t n) {
		return (interp == UsdGeomTokens->vertex || interp == UsdGeomTokens->varying) && n == pts.size();
	};
	auto per_corner_ok = [&](const TfToken &interp, size_t n) {
		return interp == UsdGeomTokens->faceVarying && n == corners;
	};
	bool nrm_pt = false, nrm_fv = false, st_pt = false, st_fv = false;
	if (!nrm.empty()) {
		nrm_pt = per_point_ok(nrm_interp, nrm.size());
		nrm_fv = per_corner_ok(nrm_interp, nrm.size());
		if (!nrm_pt && !nrm_fv) {
			warn = "normals dropped: interpolation " + nrm_interp.GetString() + " with " + std::to_string(nrm.size()) + " values";
			nrm.clear();
		}
	}
	if (!st.empty()) {
		st_pt = per_point_ok(st_interp, st.size());
		st_fv = per_corner_ok(st_interp, st.size());
		if (!st_pt && !st_fv) {
			warn = "st dropped: interpolation " + st_interp.GetString() + " with " + std::to_string(st.size()) + " values";
			st.clear();
		}
	}
	r.indexed = !(nrm_fv || st_fv);
	r.has_normals = !nrm.empty();
	r.has_uvs = !st.empty();

	TfToken orient;
	mesh.GetOrientationAttr().Get(&orient);
	const bool flip = orient == UsdGeomTokens->leftHanded;
	std::vector<char> hole(fvc.size(), 0);
	for (int h : holes)
		if (h >= 0 && (size_t)h < hole.size())
			hole[h] = 1;
	mesh.GetDoubleSidedAttr().Get(&r.double_sided);

	// Group 0 is the mesh's own binding; each materialBind GeomSubset is a group of its faces.
	std::vector<int> group_mat(1, -1);
	UsdShadeMaterial mat = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
	std::string mwarn;
	if (mat)
		group_mat[0] = add_material(mat, layer_path, mwarn);
	std::vector<int> face_group(fvc.size(), 0);
	const std::vector<UsdGeomSubset> subsets = UsdShadeMaterialBindingAPI(prim).GetMaterialBindSubsets();
	for (const UsdGeomSubset &s : subsets) {
		TfToken et;
		s.GetElementTypeAttr().Get(&et);
		if (et != UsdGeomTokens->face)
			continue;
		VtIntArray faces;
		s.GetIndicesAttr().Get(&faces, UsdTimeCode::Default());
		UsdShadeMaterial sm = UsdShadeMaterialBindingAPI(s.GetPrim()).ComputeBoundMaterial();
		const int g = (int)group_mat.size();
		group_mat.push_back(sm ? add_material(sm, layer_path, mwarn) : -1);
		for (int f : faces)
			if (f >= 0 && (size_t)f < face_group.size())
				face_group[(size_t)f] = g;
	}
	if (!mwarn.empty() && warn.empty())
		warn = mwarn;

	// vertices
	r.pt_off = g_points.size();
	r.nrm_off = g_normals.size();
	r.uv_off = g_uvs.size();
	r.idx_off = g_indices.size();
	auto push_pt = [&](size_t p, size_t c) {
		g_points.push_back(pts[p][0]);
		g_points.push_back(pts[p][1]);
		g_points.push_back(pts[p][2]);
		if (r.has_normals) {
			const GfVec3f &n = nrm_fv ? nrm[c] : nrm[p];
			g_normals.push_back(n[0]);
			g_normals.push_back(n[1]);
			g_normals.push_back(n[2]);
		}
		if (r.has_uvs) {
			const GfVec2f &t = st_fv ? st[c] : st[p];
			g_uvs.push_back(t[0]);
			g_uvs.push_back(t[1]);
		}
	};
	if (r.indexed) {
		for (size_t p = 0; p < pts.size(); ++p)
			push_pt(p, 0);
		r.npts = pts.size();
	} else {
		for (size_t c = 0; c < corners; ++c)
			push_pt((size_t)fvi[c], c);
		r.npts = corners;
	}
	// triangles: a fan per face (corner 0, i, i+1), reversed for leftHanded, in face order
	// within each group and the groups one after another
	std::vector<size_t> first_corner(fvc.size());
	for (size_t f = 0, c0 = 0; f < fvc.size(); c0 += (size_t)fvc[f], ++f)
		first_corner[f] = c0;
	r.surf_off = g_surfaces.size();
	for (size_t g = 0; g < group_mat.size(); ++g) {
		const size_t first = r.ntris;
		for (size_t f = 0; f < fvc.size(); ++f) {
			const size_t n = (size_t)fvc[f];
			const size_t c0 = first_corner[f];
			if ((size_t)face_group[f] != g || hole[f] || n < 3)
				continue;
			for (size_t i = 1; i + 1 < n; ++i) {
				int32_t a = r.indexed ? fvi[c0] : (int32_t)c0;
				int32_t b = r.indexed ? fvi[c0 + i] : (int32_t)(c0 + i);
				int32_t c = r.indexed ? fvi[c0 + i + 1] : (int32_t)(c0 + i + 1);
				if (flip)
					std::swap(b, c);
				g_indices.push_back(a);
				g_indices.push_back(b);
				g_indices.push_back(c);
				++r.ntris;
			}
		}
		if (group_mat.size() > 1 && r.ntris > first) {
			g_surfaces.push_back(group_mat[g]);
			g_surfaces.push_back((int32_t)first);
			g_surfaces.push_back((int32_t)(r.ntris - first));
			++r.nsurf;
		}
	}
	r.b3_p = intern(blake3::hex(g_points.data() + r.pt_off, r.npts * 3 * sizeof(float)));
	r.b3_i = intern(blake3::hex(g_indices.data() + r.idx_off, r.ntris * 3 * sizeof(int32_t)));

	matrix_to16(xc.GetLocalToWorldTransform(prim), r.xf);
	if (skin) {
		const std::string err = add_skin(*skin, pts.size(), fvi, r);
		if (!err.empty())
			return err;
	}

	r.material = group_mat[0];
	g_meshes.push_back(r);
	return "";
}

std::string add_curves(const UsdPrim &prim, UsdGeomXformCache &xc) {
	UsdGeomBasisCurves bc(prim);
	VtVec3fArray pts;
	VtIntArray counts;
	bc.GetPointsAttr().Get(&pts, UsdTimeCode::Default());
	bc.GetCurveVertexCountsAttr().Get(&counts, UsdTimeCode::Default());
	size_t total = 0;
	for (int c : counts) {
		if (c < 2)
			return "a curve has " + std::to_string(c) + " point(s); a stroke needs 2 or more";
		total += (size_t)c;
	}
	if (total != pts.size())
		return "curveVertexCounts sum " + std::to_string(total) + " != points " + std::to_string(pts.size());
	bool boundary = false;
	UsdGeomPrimvar pv = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken("boundary"));
	if (pv && pv.HasAuthoredValue()) {
		bool v = false;
		if (pv.Get(&v, UsdTimeCode::Default()))
			boundary = v;
	}
	const GfMatrix4d m = xc.GetLocalToWorldTransform(prim);
	const std::string name = prim.GetName().GetString();
	size_t at = 0;
	for (size_t k = 0; k < counts.size(); ++k) {
		CurveRec r;
		r.path = intern(prim.GetPath().GetString());
		r.name = intern(counts.size() == 1 ? name : name + "_" + std::to_string(k));
		r.pt_off = g_cpoints.size() / 3;
		r.npts = (size_t)counts[k];
		for (size_t i = 0; i < r.npts; ++i) {
			const GfVec3f &q = pts[at + i];
			g_cpoints.push_back(q[0]);
			g_cpoints.push_back(q[1]);
			g_cpoints.push_back(q[2]);
		}
		at += r.npts;
		r.boundary = boundary;
		r.b3_p = intern(blake3::hex(g_cpoints.data() + r.pt_off * 3, r.npts * 3 * sizeof(float)));
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				r.xf[i * 4 + j] = (float)m[i][j];
		g_curves.push_back(r);
	}
	return "";
}

// GfMatrix4d is row-vector (p' = p M): its upper 3x3 is the transpose of the
// column-vector rotation the tables hold, and its row 3 is the translation.
void matrix_to12(const GfMatrix4d &m, float *o) {
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			o[r * 3 + c] = (float)m[c][r];
	o[9] = (float)m[3][0];
	o[10] = (float)m[3][1];
	o[11] = (float)m[3][2];
}

// The double nearest the shortest decimal that reads back as this float: the
// same float, but the text writer prints it in ~9 digits, not the ~17 of
// the float's exact double. It keeps a skeleton's matrix4d[] under the 64 KiB
// a single value's text survives in the guest (gates/10-motion: past that,
// ExportToString's text of the value is cut short; the writer's self-check
// below refuses such a file).
double short_double(float f) {
	char buf[32];
	for (int p = 6; p <= 9; ++p) {
		std::snprintf(buf, sizeof buf, "%.*g", p, (double)f);
		const double d = std::strtod(buf, nullptr);
		if ((float)d == f)
			return d;
	}
	return (double)f;
}

GfMatrix4d matrix_from12(const float *o) {
	GfMatrix4d m(1.0);
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			m[c][r] = short_double(o[r * 3 + c]);
	m[3][0] = short_double(o[9]);
	m[3][1] = short_double(o[10]);
	m[3][2] = short_double(o[11]);
	return m;
}

GfMatrix4d matrix_exact12(const float *o) {
	GfMatrix4d m(1.0);
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			m[c][r] = o[r * 3 + c];
	m[3][0] = o[9];
	m[3][1] = o[10];
	m[3][2] = o[11];
	return m;
}

// One UsdSkelSkeleton: joint order, parents, binds and parent-local rests.
// Without restTransforms the rest is derived from the binds, without
// bindTransforms the bind is the rest in skeleton space.
std::string add_skeleton(const UsdPrim &prim, const UsdSkelCache &cache, UsdGeomXformCache &xc) {
	UsdSkelSkeleton skel(prim);
	UsdSkelSkeletonQuery q = cache.GetSkelQuery(skel);
	if (!q)
		return "no skeleton query";
	const VtTokenArray joints = q.GetJointOrder();
	const size_t J = joints.size();
	const UsdSkelTopology &topo = q.GetTopology();
	std::string why;
	if (J == 0 || !topo.Validate(&why))
		return "topology: " + (J == 0 ? std::string("no joints") : why);
	const VtIntArray &parents = topo.GetParentIndices();
	VtMatrix4dArray bind, rest;
	const bool has_bind = q.GetJointWorldBindTransforms(&bind) && bind.size() == J;
	const bool has_rest = q.HasRestPose() && q.ComputeJointLocalTransforms(&rest, UsdTimeCode::Default(), true) && rest.size() == J;
	if (!has_bind && !has_rest)
		return "neither bindTransforms nor restTransforms for " + std::to_string(J) + " joints";
	SkelRec r;
	if (!has_rest) {
		rest.resize(J);
		for (size_t j = 0; j < J; ++j)
			rest[j] = parents[j] < 0 ? bind[j] : bind[j] * bind[(size_t)parents[j]].GetInverse();
		r.rest_from_bind = true;
	}
	if (!has_bind) {
		if (!q.ComputeJointSkelTransforms(&bind, UsdTimeCode::Default(), true) || bind.size() != J)
			return "no bind pose and the rest does not compose";
		r.bind_from_rest = true;
	}
	r.path = intern(prim.GetPath().GetString());
	VtTokenArray names;
	skel.GetJointNamesAttr().Get(&names);
	std::string jl, nl;
	for (size_t j = 0; j < J; ++j) {
		jl += joints[j].GetString() + "\n";
		nl += (names.size() == J ? names[j].GetString() : SdfPath(joints[j].GetString()).GetName()) + "\n";
	}
	r.joints = intern(jl);
	r.names = intern(nl);
	UsdPrim anim;
	r.anim = intern(UsdSkelBindingAPI(prim).GetAnimationSource(&anim) && anim ? anim.GetPath().GetString() : std::string());
	r.njoints = J;
	r.par_off = g_skel_parents.size();
	g_skel_parents.insert(g_skel_parents.end(), parents.begin(), parents.end());
	r.xf_off = g_skel_bind.size();
	g_skel_bind.resize(r.xf_off + J * 12);
	g_skel_rest.resize(r.xf_off + J * 12);
	for (size_t j = 0; j < J; ++j) {
		matrix_to12(bind[j], &g_skel_bind[r.xf_off + j * 12]);
		matrix_to12(rest[j], &g_skel_rest[r.xf_off + j * 12]);
	}
	matrix_to16(xc.GetLocalToWorldTransform(prim), r.xf);
	g_skel_by_path[prim.GetPath().GetString()] = (int)g_skels.size();
	g_skels.push_back(r);
	return "";
}

// One UsdSkelAnimation: its joints and, per authored time sample, the
// parent-local transforms UsdSkelAnimQuery composes from its translations,
// rotations and scales.
std::string add_anim(const UsdPrim &prim, double fps) {
	UsdSkelCache cache;
	UsdSkelAnimQuery q = cache.GetAnimQuery(prim);
	if (!q)
		return "no anim query";
	std::vector<double> times;
	q.GetJointTransformTimeSamples(&times);
	const VtTokenArray joints = q.GetJointOrder();
	AnimRec r;
	r.path = intern(prim.GetPath().GetString());
	std::string jl;
	for (const TfToken &t : joints)
		jl += t.GetString() + "\n";
	r.joints = intern(jl);
	r.njoints = joints.size();
	r.fps = fps;
	r.xf_off = g_animxf.size();
	for (double t : times) {
		VtMatrix4dArray xf;
		if (!q.ComputeJointLocalTransforms(&xf, UsdTimeCode(t)))
			return "ComputeJointLocalTransforms failed at time " + std::to_string(t);
		if (xf.size() != joints.size())
			return "transforms for " + std::to_string(xf.size()) + " of " + std::to_string(joints.size()) + " joints";
		const size_t at = g_animxf.size();
		g_animxf.resize(at + xf.size() * 12);
		for (size_t j = 0; j < xf.size(); ++j)
			matrix_to12(xf[j], &g_animxf[at + j * 12]);
		++r.frames;
	}
	g_anims.push_back(r);
	return "";
}

void clear_tables() {
	g_strings.clear();
	g_points.clear();
	g_normals.clear();
	g_uvs.clear();
	g_indices.clear();
	g_texdata.clear();
	g_meshes.clear();
	std::vector<int32_t>().swap(g_surfaces);
	g_tex_missing.clear();
	g_curves.clear();
	g_anims.clear();
	std::vector<float>().swap(g_animxf);
	g_skels.clear();
	g_skel_by_path.clear();
	g_skins.clear();
	std::vector<int32_t>().swap(g_skel_parents);
	std::vector<float>().swap(g_skel_bind);
	std::vector<float>().swap(g_skel_rest);
	std::vector<int32_t>().swap(g_skin_idx);
	std::vector<float>().swap(g_skin_w);
	g_layer_data.clear();
	g_cpoints.clear();
	g_mats.clear();
	g_texs.clear();
	g_tex_by_path.clear();
	g_mat_by_path.clear();
	// Give the memory back to the guest heap: the next document may be bigger
	// and the heap's ceiling is the whole sandbox.
	std::vector<float>().swap(g_points);
	std::vector<float>().swap(g_cpoints);
	std::vector<float>().swap(g_normals);
	std::vector<float>().swap(g_uvs);
	std::vector<int32_t>().swap(g_indices);
	std::vector<uint8_t>().swap(g_texdata);
}

} // namespace

namespace usdg {

std::string init() {
	return usdmem::init();
}

std::string open(const std::string &bytes) {
	close();
	const std::string init = usdmem::init();
	if (init.compare(0, 3, "ok ") != 0)
		return "ERR: init: " + init;
	if (bytes.empty())
		return "ERR: empty package";
	const char *ext = "usda";
	if (bytes.size() >= 4 && std::memcmp(bytes.data(), "PK\x03\x04", 4) == 0)
		ext = "usdz";
	else if (bytes.size() >= 8 && std::memcmp(bytes.data(), "PXR-USDC", 8) == 0)
		ext = "usdc";
	static int serial = 0;
	g_pkg_path = "/mem/pkg" + std::to_string(++serial) + "." + ext;
	usdmem::put_asset(g_pkg_path, bytes.data(), bytes.size());

	TfErrorMark mark;
	size_t prims = 0;
	std::string warn;
	std::string up = "Y";
	double mpu = 1.0;
	size_t skins_unbound = 0;
	try {
		SdfLayerRefPtr layer = SdfLayer::FindOrOpen(g_pkg_path);
		if (!layer) {
			std::string e = "ERR: " + std::string(ext) + " open failed: " + first_error(mark);
			close();
			return e;
		}
		UsdStageRefPtr stage = UsdStage::Open(layer, UsdStage::LoadAll);
		if (!stage) {
			std::string e = "ERR: " + std::string(ext) + " no stage: " + first_error(mark);
			close();
			return e;
		}
		const VtDictionary cld = stage->GetRootLayer()->GetCustomLayerData();
		for (VtDictionary::const_iterator it = cld.begin(); it != cld.end(); ++it) {
			const VtValue &v = it->second;
			g_layer_data.emplace_back(it->first, v.IsHolding<std::string>() ? v.UncheckedGet<std::string>() : TfStringify(v));
		}
		up = UsdGeomGetStageUpAxis(stage).GetString();
		mpu = UsdGeomGetStageMetersPerUnit(stage);
		UsdGeomXformCache xc(UsdTimeCode::Default());
		UsdSkelCache skel_cache;
		std::map<std::string, SkinSrc> skinned;
		size_t bound = 0;
		for (const UsdPrim &prim : stage->Traverse()) {
			if (!prim.IsA<UsdSkelSkeleton>())
				continue;
			const std::string err = add_skeleton(prim, skel_cache, xc);
			if (!err.empty()) {
				std::string e = "ERR: skeleton " + prim.GetPath().GetString() + ": " + err;
				close();
				return e;
			}
		}
		for (const UsdPrim &prim : stage->Traverse()) {
			if (prim.HasAPI<UsdSkelBindingAPI>() && prim.IsA<UsdGeomMesh>())
				++bound;
			if (!prim.IsA<UsdSkelRoot>())
				continue;
			const UsdSkelRoot root(prim);
			std::vector<UsdSkelBinding> bindings;
			skel_cache.Populate(root, UsdPrimDefaultPredicate);
			skel_cache.ComputeSkelBindings(root, &bindings, UsdPrimDefaultPredicate);
			for (const UsdSkelBinding &b : bindings) {
				std::map<std::string, int>::const_iterator s = g_skel_by_path.find(b.GetSkeleton().GetPath().GetString());
				if (s == g_skel_by_path.end())
					continue;
				for (const UsdSkelSkinningQuery &q : b.GetSkinningTargets())
					skinned[q.GetPrim().GetPath().GetString()] = SkinSrc{ q, s->second };
			}
		}
		for (const UsdPrim &prim : stage->Traverse()) {
			++prims;
			if (prim.IsA<UsdSkelAnimation>()) {
				std::string err = add_anim(prim, stage->GetTimeCodesPerSecond());
				if (!err.empty()) {
					std::string e = "ERR: skel animation " + prim.GetPath().GetString() + ": " + err;
					close();
					return e;
				}
				continue;
			}
			if (prim.IsA<UsdGeomBasisCurves>()) {
				std::string err = add_curves(prim, xc);
				if (!err.empty()) {
					std::string e = "ERR: curves " + prim.GetPath().GetString() + ": " + err;
					close();
					return e;
				}
				continue;
			}
			if (!prim.IsA<UsdGeomMesh>())
				continue;
			std::string mwarn;
			std::map<std::string, SkinSrc>::const_iterator sk = skinned.find(prim.GetPath().GetString());
			std::string err = add_mesh(prim, xc, g_pkg_path, sk == skinned.end() ? nullptr : &sk->second, mwarn);
			if (!err.empty()) {
				std::string e = "ERR: mesh " + prim.GetPath().GetString() + ": " + err;
				close();
				return e;
			}
			if (!mwarn.empty() && warn.empty())
				warn = prim.GetName().GetString() + ": " + mwarn;
		}
		skins_unbound = bound > g_skins.size() ? bound - g_skins.size() : 0;
		// The stage and layer go out of scope here; the flat tables stay.
	} catch (const std::exception &e) {
		std::string s = std::string("ERR: ") + ext + " exception: " + e.what();
		close();
		return s;
	}
	char buf[360];
	std::snprintf(buf, sizeof buf, "ok layer=%s prims=%zu meshes=%zu curves=%zu materials=%zu textures=%zu up=%s mpu=%g bytes=%zu anims=%zu skels=%zu skins=%zu skins_unbound=%zu textures_missing=%zu",
			ext, prims, g_meshes.size(), g_curves.size(), g_mats.size(), g_texs.size(), up.c_str(), mpu, bytes.size(), g_anims.size(),
			g_skels.size(), g_skins.size(), skins_unbound, g_tex_missing.size());
	std::string out = buf;
	if (!g_tex_missing.empty())
		out += " missing=" + TfStringJoin(g_tex_missing, ",");
	if (!warn.empty())
		out += " warn=" + warn;
	if (!mark.IsClean())
		out += " usd_error=" + first_error(mark);
	return out;
}

void close() {
	if (!g_pkg_path.empty()) {
		usdmem::erase_asset(g_pkg_path);
		g_pkg_path.clear();
	}
	clear_tables();
}

int mesh_count() { return (int)g_meshes.size(); }
int curve_count() { return (int)g_curves.size(); }
int material_count() { return (int)g_mats.size(); }
int texture_count() { return (int)g_texs.size(); }

bool mesh_info(int i, MeshInfo &out) {
	if (i < 0 || (size_t)i >= g_meshes.size())
		return false;
	const MeshRec &r = g_meshes[i];
	out.path = str(r.path);
	out.name = str(r.name);
	out.points = r.npts;
	out.triangles = r.ntris;
	out.has_normals = r.has_normals;
	out.has_uvs = r.has_uvs;
	out.indexed = r.indexed;
	out.material = r.material;
	out.blake3_points = str(r.b3_p);
	out.blake3_indices = str(r.b3_i);
	std::memcpy(out.xform, r.xf, sizeof r.xf);
	out.skeleton = r.skin < 0 ? -1 : g_skins[(size_t)r.skin].skel;
	out.double_sided = r.double_sided;
	out.surfaces.assign(g_surfaces.begin() + (long)r.surf_off, g_surfaces.begin() + (long)(r.surf_off + 3 * r.nsurf));
	return true;
}

int skeleton_count() { return (int)g_skels.size(); }

bool skeleton_info(int i, SkeletonInfo &out) {
	if (i < 0 || (size_t)i >= g_skels.size())
		return false;
	const SkelRec &r = g_skels[i];
	out.path = str(r.path);
	out.joints = str(r.joints);
	out.names = str(r.names);
	out.animation = str(r.anim);
	out.parents.assign(g_skel_parents.begin() + (long)r.par_off, g_skel_parents.begin() + (long)(r.par_off + r.njoints));
	out.rest_from_bind = r.rest_from_bind;
	out.bind_from_rest = r.bind_from_rest;
	std::memcpy(out.xform, r.xf, sizeof r.xf);
	return true;
}

const float *skeleton_binds(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_skels.size())
		return nullptr;
	n = g_skels[i].njoints;
	return g_skel_bind.data() + g_skels[i].xf_off;
}

const float *skeleton_rests(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_skels.size())
		return nullptr;
	n = g_skels[i].njoints;
	return g_skel_rest.data() + g_skels[i].xf_off;
}

bool mesh_skin(int i, SkinInfo &out) {
	if (i < 0 || (size_t)i >= g_meshes.size() || g_meshes[i].skin < 0)
		return false;
	const SkinRec &k = g_skins[(size_t)g_meshes[i].skin];
	out.skeleton = k.skel;
	out.element_size = k.element;
	out.max_nonzero = k.nonzero;
	out.interpolation = str(k.interp);
	out.joints = str(k.joints);
	out.method = str(k.method);
	std::memcpy(out.geom_bind, k.geom, sizeof k.geom);
	out.blake3_indices = str(k.b3_i);
	out.blake3_weights = str(k.b3_w);
	return true;
}

const int32_t *mesh_skin_indices(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_meshes.size() || g_meshes[i].skin < 0)
		return nullptr;
	n = g_meshes[i].npts;
	return g_skin_idx.data() + g_skins[(size_t)g_meshes[i].skin].off;
}

const float *mesh_skin_weights(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_meshes.size() || g_meshes[i].skin < 0)
		return nullptr;
	n = g_meshes[i].npts;
	return g_skin_w.data() + g_skins[(size_t)g_meshes[i].skin].off;
}

std::vector<float> mesh_skin_pose(int i, const float *local, size_t n_joints, bool normalize, std::string &why) {
	std::vector<float> out;
	if (i < 0 || (size_t)i >= g_meshes.size() || g_meshes[i].skin < 0) {
		why = "mesh " + std::to_string(i) + " is not skinned";
		return out;
	}
	const MeshRec &m = g_meshes[i];
	const SkinRec &k = g_skins[(size_t)m.skin];
	const SkelRec &s = g_skels[(size_t)k.skel];
	if (n_joints != s.njoints) {
		why = std::to_string(n_joints) + " joint transforms for " + std::to_string(s.njoints) + " joints";
		return out;
	}
	VtIntArray parents(s.njoints);
	for (size_t j = 0; j < s.njoints; ++j)
		parents[j] = g_skel_parents[s.par_off + j];
	const UsdSkelTopology topo(parents);
	std::vector<GfMatrix4d> loc(s.njoints), skel_xf(s.njoints), skin_xf(s.njoints);
	for (size_t j = 0; j < s.njoints; ++j)
		loc[j] = matrix_exact12(local + j * 12);
	if (!UsdSkelConcatJointTransforms(topo, TfSpan<const GfMatrix4d>(loc), TfSpan<GfMatrix4d>(skel_xf))) {
		why = "the joint transforms do not compose";
		return out;
	}
	for (size_t j = 0; j < s.njoints; ++j)
		skin_xf[j] = matrix_exact12(&g_skel_bind[s.xf_off + j * 12]).GetInverse() * skel_xf[j];
	GfMatrix4d geom, world;
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c) {
			geom[r][c] = k.geom[r * 4 + c];
			world[r][c] = s.xf[r * 4 + c];
		}
	std::vector<GfVec3f> pts(m.npts);
	for (size_t v = 0; v < m.npts; ++v)
		pts[v] = GfVec3f(g_points[m.pt_off + v * 3], g_points[m.pt_off + v * 3 + 1], g_points[m.pt_off + v * 3 + 2]);
	const size_t n = m.npts * (size_t)k.element;
	std::vector<float> w(g_skin_w.begin() + (long)k.off, g_skin_w.begin() + (long)(k.off + n));
	if (normalize)
		UsdSkelNormalizeWeights(TfSpan<float>(w), k.element);
	if (!UsdSkelSkinPointsLBS(geom, TfSpan<const GfMatrix4d>(skin_xf), TfSpan<const int>(g_skin_idx.data() + k.off, n),
				TfSpan<const float>(w), k.element, TfSpan<GfVec3f>(pts), true)) {
		why = "UsdSkelSkinPointsLBS failed";
		return out;
	}
	out.resize(m.npts * 3);
	for (size_t v = 0; v < m.npts; ++v) {
		const GfVec3d w = world.Transform(GfVec3d(pts[v]));
		out[v * 3] = (float)w[0];
		out[v * 3 + 1] = (float)w[1];
		out[v * 3 + 2] = (float)w[2];
	}
	return out;
}

const float *mesh_points(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_meshes.size())
		return nullptr;
	n = g_meshes[i].npts;
	return g_points.data() + g_meshes[i].pt_off;
}

const float *mesh_normals(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_meshes.size() || !g_meshes[i].has_normals)
		return nullptr;
	n = g_meshes[i].npts;
	return g_normals.data() + g_meshes[i].nrm_off;
}

const float *mesh_uvs(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_meshes.size() || !g_meshes[i].has_uvs)
		return nullptr;
	n = g_meshes[i].npts;
	return g_uvs.data() + g_meshes[i].uv_off;
}

const int32_t *mesh_indices(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_meshes.size())
		return nullptr;
	n = g_meshes[i].ntris;
	return g_indices.data() + g_meshes[i].idx_off;
}

bool material_info(int i, MaterialInfo &out) {
	if (i < 0 || (size_t)i >= g_mats.size())
		return false;
	const MatRec &m = g_mats[i];
	out.path = str(m.path);
	out.name = str(m.name);
	out.shader_id = str(m.shader);
	std::memcpy(out.diffuse, m.diffuse, sizeof m.diffuse);
	out.metallic = m.metallic;
	out.roughness = m.roughness;
	out.opacity = m.opacity;
	out.diffuse_tex = m.tex[0];
	out.metallic_tex = m.tex[1];
	out.roughness_tex = m.tex[2];
	out.opacity_tex = m.tex[3];
	out.normal_tex = m.tex[4];
	out.diffuse_channel = str(m.channel[0]);
	out.metallic_channel = str(m.channel[1]);
	out.roughness_channel = str(m.channel[2]);
	out.opacity_channel = str(m.channel[3]);
	out.normal_channel = str(m.channel[4]);
	for (int k = 0; k < 5; k++)
		out.file[k] = str(m.file[k]);
	return true;
}

bool texture_info(int i, TextureInfo &out) {
	if (i < 0 || (size_t)i >= g_texs.size())
		return false;
	out.path = str(g_texs[i].path);
	out.file = str(g_texs[i].file);
	out.size = g_texs[i].size;
	return true;
}

const uint8_t *texture_bytes(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_texs.size())
		return nullptr;
	n = g_texs[i].size;
	return g_texdata.data() + g_texs[i].data_off;
}

bool curve_info(int i, CurveInfo &out) {
	if (i < 0 || (size_t)i >= g_curves.size())
		return false;
	const CurveRec &r = g_curves[i];
	out.path = str(r.path);
	out.name = str(r.name);
	out.points = r.npts;
	out.boundary = r.boundary;
	out.blake3_points = str(r.b3_p);
	std::memcpy(out.xform, r.xf, sizeof r.xf);
	return true;
}

const std::vector<std::pair<std::string, std::string>> &layer_data() {
	return g_layer_data;
}

const float *curve_points(int i, size_t &n) {
	if (i < 0 || (size_t)i >= g_curves.size())
		return nullptr;
	n = g_curves[i].npts;
	return g_cpoints.data() + g_curves[i].pt_off * 3;
}

std::string write_curves(const float *points, size_t n_points, const int32_t *counts, size_t n_curves,
		const std::vector<std::string> &names, const std::vector<int32_t> &boundary,
		const std::vector<std::pair<std::string, std::string>> &meta) {
	const std::string init = usdmem::init();
	if (init.compare(0, 3, "ok ") != 0)
		return "ERR: init: " + init;
	if (!names.empty() && names.size() != n_curves)
		return "ERR: " + std::to_string(names.size()) + " names for " + std::to_string(n_curves) + " curves";
	size_t total = 0;
	for (size_t k = 0; k < n_curves; ++k) {
		if (counts[k] < 2)
			return "ERR: curve " + std::to_string(k) + " has " + std::to_string(counts[k]) + " point(s); a stroke needs 2 or more";
		total += (size_t)counts[k];
	}
	if (total != n_points)
		return "ERR: counts sum " + std::to_string(total) + " != points " + std::to_string(n_points);
	std::vector<char> marked(n_curves, 0);
	for (int32_t b : boundary) {
		if (b < 0 || (size_t)b >= n_curves)
			return "ERR: boundary index " + std::to_string(b) + " out of range";
		marked[(size_t)b] = 1;
	}
	TfErrorMark mark;
	try {
		UsdStageRefPtr stage = UsdStage::CreateInMemory();
		UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
		UsdGeomSetStageMetersPerUnit(stage, 1.0);
		UsdGeomXform root = UsdGeomXform::Define(stage, SdfPath("/Creation"));
		stage->SetDefaultPrim(root.GetPrim());
		VtDictionary cld;
		for (const std::pair<std::string, std::string> &kv : meta)
			cld[kv.first] = VtValue(kv.second);
		stage->GetRootLayer()->SetCustomLayerData(cld);
		size_t at = 0;
		for (size_t k = 0; k < n_curves; ++k) {
			char dflt[32];
			std::snprintf(dflt, sizeof dflt, "stroke_%03zu", k);
			const std::string name = names.empty() ? std::string(dflt) : names[k];
			if (!SdfPath::IsValidIdentifier(name))
				return "ERR: curve " + std::to_string(k) + ": '" + name + "' is not a valid prim name";
			UsdGeomBasisCurves bc = UsdGeomBasisCurves::Define(stage, SdfPath("/Creation/" + name));
			bc.CreateTypeAttr(VtValue(UsdGeomTokens->linear));
			VtVec3fArray pts((size_t)counts[k]);
			for (size_t i = 0; i < pts.size(); ++i)
				pts[i] = GfVec3f(points[(at + i) * 3], points[(at + i) * 3 + 1], points[(at + i) * 3 + 2]);
			at += pts.size();
			bc.CreatePointsAttr(VtValue(pts));
			VtIntArray cvc(1, counts[k]);
			bc.CreateCurveVertexCountsAttr(VtValue(cvc));
			if (marked[k]) {
				UsdGeomPrimvar pv = UsdGeomPrimvarsAPI(bc.GetPrim())
						.CreatePrimvar(TfToken("boundary"), SdfValueTypeNames->Bool, UsdGeomTokens->uniform);
				pv.Set(true);
			}
		}
		std::string text;
		if (!stage->GetRootLayer()->ExportToString(&text))
			return "ERR: export failed: " + first_error(mark);
		return text;
	} catch (const std::exception &e) {
		return std::string("ERR: write exception: ") + e.what();
	}
}


int skel_anim_count() { return (int)g_anims.size(); }

bool skel_anim_info(int i, SkelAnimInfo &out) {
	if (i < 0 || (size_t)i >= g_anims.size())
		return false;
	const AnimRec &r = g_anims[i];
	out.path = str(r.path);
	out.joints = str(r.joints);
	out.frames = r.frames;
	out.njoints = r.njoints;
	out.fps = r.fps;
	return true;
}

const float *skel_anim_transforms(int i, size_t &n_floats) {
	n_floats = 0;
	if (i < 0 || (size_t)i >= g_anims.size())
		return nullptr;
	n_floats = g_anims[i].frames * g_anims[i].njoints * 12;
	return g_animxf.data() + g_anims[i].xf_off;
}

std::string write_skel_clip(const std::vector<std::string> &names, const std::vector<int32_t> &parents,
		const float *rest_local, const float *bind_world, const std::vector<int32_t> &anim_joints,
		const float *anim_local, size_t frames, double fps,
		const std::vector<std::pair<std::string, std::string>> &meta) {
	const std::string init = usdmem::init();
	if (init.compare(0, 3, "ok ") != 0)
		return "ERR: init: " + init;
	const size_t J = names.size();
	if (parents.size() != J)
		return "ERR: " + std::to_string(parents.size()) + " parents for " + std::to_string(J) + " joints";
	if (!(fps > 0.0) || frames == 0)
		return "ERR: a clip needs frames and a positive fps";
	for (int32_t a : anim_joints)
		if (a < 0 || (size_t)a >= J)
			return "ERR: animated joint " + std::to_string(a) + " out of range";
	// UsdSkel orders parents before children; take a depth-first order.
	std::vector<std::vector<size_t>> kids(J);
	std::vector<size_t> order, rank(J, 0);
	for (size_t j = 0; j < J; ++j) {
		if (parents[j] >= (int32_t)J)
			return "ERR: joint " + std::to_string(j) + " has parent " + std::to_string(parents[j]);
		if (parents[j] >= 0)
			kids[(size_t)parents[j]].push_back(j);
	}
	std::vector<size_t> stack;
	for (size_t j = J; j-- > 0;)
		if (parents[j] < 0)
			stack.push_back(j);
	while (!stack.empty()) {
		const size_t j = stack.back();
		stack.pop_back();
		rank[j] = order.size();
		order.push_back(j);
		for (size_t k = kids[j].size(); k-- > 0;)
			stack.push_back(kids[j][k]);
	}
	if (order.size() != J)
		return "ERR: the parents do not form a forest";
	// Joint paths: each name made a valid identifier (TfMakeValidIdentifier),
	// unique among its siblings; the names themselves go in jointNames.
	std::vector<std::string> path(J);
	std::map<std::string, int> seen;
	for (size_t j : order) {
		std::string id = TfMakeValidIdentifier(names[j]);
		std::string p = parents[j] < 0 ? id : path[(size_t)parents[j]] + "/" + id;
		for (int n = 1; seen.count(p); ++n)
			p = (parents[j] < 0 ? id : path[(size_t)parents[j]] + "/" + id) + "_" + std::to_string(n);
		seen[p] = 1;
		path[j] = p;
	}
	TfErrorMark mark;
	try {
		UsdStageRefPtr stage = UsdStage::CreateInMemory();
		UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
		UsdGeomSetStageMetersPerUnit(stage, 1.0);
		stage->SetTimeCodesPerSecond(fps);
		stage->SetFramesPerSecond(fps);
		stage->SetStartTimeCode(0.0);
		stage->SetEndTimeCode(double(frames - 1));
		UsdSkelRoot root = UsdSkelRoot::Define(stage, SdfPath("/Clip"));
		stage->SetDefaultPrim(root.GetPrim());
		VtDictionary cld;
		for (const std::pair<std::string, std::string> &kv : meta)
			cld[kv.first] = VtValue(kv.second);
		stage->GetRootLayer()->SetCustomLayerData(cld);
		UsdSkelSkeleton skel = UsdSkelSkeleton::Define(stage, SdfPath("/Clip/Skeleton"));
		VtTokenArray jt(J), jn(J);
		VtMatrix4dArray bind(J), rest(J);
		for (size_t k = 0; k < J; ++k) {
			const size_t j = order[k];
			jt[k] = TfToken(path[j]);
			jn[k] = TfToken(names[j]);
			bind[k] = matrix_from12(bind_world + j * 12);
			rest[k] = matrix_from12(rest_local + j * 12);
		}
		skel.CreateJointsAttr(VtValue(jt));
		skel.CreateJointNamesAttr(VtValue(jn));
		skel.CreateBindTransformsAttr(VtValue(bind));
		skel.CreateRestTransformsAttr(VtValue(rest));
		UsdSkelAnimation anim = UsdSkelAnimation::Define(stage, SdfPath("/Clip/Skeleton/Motion"));
		const size_t A = anim_joints.size();
		VtTokenArray at(A);
		for (size_t a = 0; a < A; ++a)
			at[a] = TfToken(path[(size_t)anim_joints[a]]);
		anim.CreateJointsAttr(VtValue(at));
		for (size_t f = 0; f < frames; ++f) {
			VtMatrix4dArray xf(A);
			for (size_t a = 0; a < A; ++a)
				xf[a] = matrix_from12(anim_local + (f * A + a) * 12);
			if (!anim.SetTransforms(xf, UsdTimeCode(double(f))))
				return "ERR: SetTransforms failed at frame " + std::to_string(f) + ": " + first_error(mark);
		}
		UsdSkelBindingAPI::Apply(skel.GetPrim()).CreateAnimationSourceRel().SetTargets({ anim.GetPath() });
		std::string why;
		UsdSkelTopology topo(jt);
		if (!topo.Validate(&why))
			return "ERR: skeleton topology: " + why;
		std::string text;
		if (!stage->GetRootLayer()->ExportToString(&text))
			return "ERR: export failed: " + first_error(mark);
		// The text must parse back before it leaves (a writer that emits a
		// file OpenUSD cannot read is refused here, not found by cage.elf).
		SdfLayerRefPtr check = SdfLayer::CreateAnonymous(".usda");
		if (!check->ImportFromString(text)) {
			size_t longest = 0, at = 0, line = 1, start = 0;
			for (size_t i = 0; i <= text.size(); ++i) {
				if (i == text.size() || text[i] == '\n') {
					if (i - start > longest) {
						longest = i - start;
						at = line;
					}
					start = i + 1;
					++line;
				}
			}
			return "ERR: the exported text does not parse back (" + std::to_string(text.size()) + " bytes, longest line " +
					std::to_string(at) + " of " + std::to_string(longest) + " chars): " + first_error(mark).substr(0, 200);
		}
		return text;
	} catch (const std::exception &e) {
		return std::string("ERR: write exception: ") + e.what();
	}
}

} // namespace usdg
