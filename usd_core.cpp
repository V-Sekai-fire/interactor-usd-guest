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
#include "../common/blake3.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/packageUtils.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/xformCache.h"
#include "pxr/usd/usdShade/connectableAPI.h"
#include "pxr/usd/usdShade/input.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usdShade/tokens.h"

#include <cstdio>
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
std::vector<MatRec> g_mats;
std::vector<TexRec> g_texs;
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
		if (resolved.empty() && !layer_path.empty() && authored[0] != '/')
			resolved = ArJoinPackageRelativePath(layer_path, authored);
	}
	if (resolved.empty()) {
		why = "texture '" + authored + "' does not resolve";
		return -1;
	}
	auto it = g_tex_by_path.find(resolved);
	if (it != g_tex_by_path.end())
		return it->second;
	std::shared_ptr<ArAsset> asset = ArGetResolver().OpenAsset(ArResolvedPath(resolved));
	if (!asset) {
		why = "texture '" + resolved + "' cannot be opened";
		return -1;
	}
	const size_t n = asset->GetSize();
	std::shared_ptr<const char> buf = asset->GetBuffer();
	if (!buf || n == 0) {
		why = "texture '" + resolved + "' is empty";
		return -1;
	}
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

std::string add_mesh(const UsdPrim &prim, UsdGeomXformCache &xc, const std::string &layer_path, std::string &warn) {
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
	// triangles: a fan per face (corner 0, i, i+1), reversed for leftHanded
	size_t c0 = 0;
	for (size_t f = 0; f < fvc.size(); ++f) {
		const size_t n = (size_t)fvc[f];
		if (!hole[f] && n >= 3) {
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
		c0 += n;
	}
	r.b3_p = intern(blake3::hex(g_points.data() + r.pt_off, r.npts * 3 * sizeof(float)));
	r.b3_i = intern(blake3::hex(g_indices.data() + r.idx_off, r.ntris * 3 * sizeof(int32_t)));

	const GfMatrix4d m = xc.GetLocalToWorldTransform(prim);
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			r.xf[i * 4 + j] = (float)m[i][j];

	UsdShadeMaterial mat = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
	if (mat) {
		std::string mwarn;
		r.material = add_material(mat, layer_path, mwarn);
		if (!mwarn.empty() && warn.empty())
			warn = mwarn;
	}
	g_meshes.push_back(r);
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
	g_mats.clear();
	g_texs.clear();
	g_tex_by_path.clear();
	g_mat_by_path.clear();
	// Give the memory back to the guest heap: the next document may be bigger
	// and the heap's ceiling is the whole sandbox.
	std::vector<float>().swap(g_points);
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
		up = UsdGeomGetStageUpAxis(stage).GetString();
		mpu = UsdGeomGetStageMetersPerUnit(stage);
		UsdGeomXformCache xc(UsdTimeCode::Default());
		for (const UsdPrim &prim : stage->Traverse()) {
			++prims;
			if (!prim.IsA<UsdGeomMesh>())
				continue;
			std::string mwarn;
			std::string err = add_mesh(prim, xc, g_pkg_path, mwarn);
			if (!err.empty()) {
				std::string e = "ERR: mesh " + prim.GetPath().GetString() + ": " + err;
				close();
				return e;
			}
			if (!mwarn.empty() && warn.empty())
				warn = prim.GetName().GetString() + ": " + mwarn;
		}
		// The stage and layer go out of scope here; the flat tables stay.
	} catch (const std::exception &e) {
		std::string s = std::string("ERR: ") + ext + " exception: " + e.what();
		close();
		return s;
	}
	char buf[320];
	std::snprintf(buf, sizeof buf, "ok layer=%s prims=%zu meshes=%zu materials=%zu textures=%zu up=%s mpu=%g bytes=%zu",
			ext, prims, g_meshes.size(), g_mats.size(), g_texs.size(), up.c_str(), mpu, bytes.size());
	std::string out = buf;
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
	return true;
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

} // namespace usdg
