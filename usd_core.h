// usd.elf (Cut U): OpenUSD 26.05 opens a .usdz package (or a bare USDA/USDC
// layer) from bytes the host hands over and answers with flat mesh and
// material tables. Plain std types here so main.cpp (the sandbox API TU) and
// a native control can both see it; usd_core.cpp is the pxr-facing TU
// (gnu++17, as Gate 0G's probe).
//
// After open() the stage is dropped; only flat tables remain (AGENTS.md: keep
// long-lived guest data flat). Meshes are UsdGeomMesh prims in Traverse()
// order, fan-triangulated per faceVertexCounts (holes skipped, leftHanded
// orientation reversed). When normals and st are per point (vertex/varying
// interpolation) the mesh stays indexed on the USD points; when either is
// faceVarying every face corner becomes a vertex (indexed=false).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace usdg {

// Register the embedded plugins and the in-memory resolver (idempotent);
// usdmem::init()'s line.
std::string init();

// Open bytes (a .usdz zip, or a USDA/USDC layer) as the current document.
// Returns "ok layer=usdz|usdc|usda prims=P meshes=M materials=K textures=T
// up=Y|Z mpu=<metersPerUnit> ..." or "ERR: <reason>". A previous document is
// closed first.
std::string open(const std::string &bytes);
void close();
// Sizes of the current document's tables (0 when nothing is open).
int mesh_count();
int material_count();
int texture_count();

struct MeshInfo {
	std::string path, name;
	size_t points = 0, triangles = 0;
	bool has_normals = false, has_uvs = false, indexed = true;
	int material = -1; // index into the material table, -1 unbound
	std::string blake3_points, blake3_indices; // BLAKE3 hex over the f32 / i32 bytes
	float xform[16] = {}; // local-to-world, GfMatrix4d row-major (row i = image of axis i, row 3 = origin)
	int skeleton = -1; // the bound skeleton when the mesh is skinned
};
bool mesh_info(int i, MeshInfo &out);
// Views into the flat tables; n is the element count (points / triangles).
// nullptr when i is out of range or the mesh has no such attribute.
const float *mesh_points(int i, size_t &n_points);
const float *mesh_normals(int i, size_t &n_points);
const float *mesh_uvs(int i, size_t &n_points);
const int32_t *mesh_indices(int i, size_t &n_triangles);

// BasisCurves (the pen's stroke format): one record per curve, in Traverse()
// order; boundary is primvars:boundary authored true on the curve's prim.
int curve_count();
struct CurveInfo {
	std::string path, name;
	size_t points = 0;
	bool boundary = false;
	std::string blake3_points; // BLAKE3 hex over the f32 xyz bytes
	float xform[16] = {};
};
bool curve_info(int i, CurveInfo &out);
const float *curve_points(int i, size_t &n_points);
// The open document's customLayerData, each value as text (strings as-is).
const std::vector<std::pair<std::string, std::string>> &layer_data();
// A new layer, the inverse of the curves table: /Creation (upAxis Y,
// metersPerUnit 1) with one linear BasisCurves prim per curve, named from
// `names` or stroke_NNN, primvars:boundary authored only on the indices in
// `boundary`, `meta` as customLayerData strings. Returns the .usda text or
// "ERR: <reason>".
std::string write_curves(const float *points, size_t n_points, const int32_t *counts, size_t n_curves,
		const std::vector<std::string> &names, const std::vector<int32_t> &boundary,
		const std::vector<std::pair<std::string, std::string>> &meta);

// UsdSkelAnimation prims (RFD 2277 A2's motion clips), in Traverse() order.
// A clip's transforms are, per authored time sample, per animated joint, the
// parent-local transform UsdSkelAnimQuery composes: 12 floats, the row-major
// 3x3 in the column-vector convention (rule 11: never a quaternion here; the
// schema's quatf rotations are composed by OpenUSD), then the translation.
int skel_anim_count();
struct SkelAnimInfo {
	std::string path;
	std::string joints; // one joint path per line
	size_t frames = 0, njoints = 0;
	double fps = 0.0; // the stage's timeCodesPerSecond
};
bool skel_anim_info(int i, SkelAnimInfo &out);
const float *skel_anim_transforms(int i, size_t &n_floats);
// A new layer holding one motion clip: /Clip (a SkelRoot; upAxis Y,
// metersPerUnit 1, timeCodesPerSecond = fps, time codes 0 .. frames-1) with
// /Clip/Skeleton (every joint: joints are the names made valid identifiers
// and nested by `parents`, jointNames the names as given, bindTransforms the
// world rests, restTransforms the parent-local rests) and
// /Clip/Skeleton/Motion (a SkelAnimation of the joints in `anim_joints`,
// authored through UsdSkelAnimation::SetTransforms from anim_local, frames x
// anim_joints x 12). rest_local and bind_world are J x 12 in the same layout.
// Returns the .usda text or "ERR: <reason>".
std::string write_skel_clip(const std::vector<std::string> &names, const std::vector<int32_t> &parents,
		const float *rest_local, const float *bind_world, const std::vector<int32_t> &anim_joints,
		const float *anim_local, size_t frames, double fps,
		const std::vector<std::pair<std::string, std::string>> &meta);

// UsdSkelSkeleton prims, in Traverse() order. Joint transforms are 12 floats
// as the animation table's: bind is each joint's bindTransforms entry, rest its
// parent-local restTransforms entry (derived from the binds when not authored).
int skeleton_count();
struct SkeletonInfo {
	std::string path;
	std::string joints; // joint paths in skeleton order, one per line
	std::string names; // jointNames when authored, else each joint path's last element; one per line
	std::string animation; // skel:animationSource, "" when unbound
	std::vector<int32_t> parents;
	bool rest_from_bind = false, bind_from_rest = false;
	float xform[16] = {}; // the skeleton prim's local-to-world, as MeshInfo::xform
};
bool skeleton_info(int i, SkeletonInfo &out);
const float *skeleton_binds(int i, size_t &n_joints);
const float *skeleton_rests(int i, size_t &n_joints);

// A skinned mesh's influences, per vertex of its point table (a faceVarying
// mesh repeats them per corner, a constant one per point), element_size a
// vertex, joint indices in its skeleton's order (skel:joints resolved).
struct SkinInfo {
	int skeleton = -1;
	int element_size = 0;
	int max_nonzero = 0; // the most non-zero weights any vertex has
	std::string interpolation; // as authored: vertex or constant
	std::string joints; // the mesh's own joint order (skel:joints, else the skeleton's), one per line
	std::string method; // skel:skinningMethod
	float geom_bind[16] = {}; // as MeshInfo::xform
	std::string blake3_indices, blake3_weights;
};
bool mesh_skin(int i, SkinInfo &out);
const int32_t *mesh_skin_indices(int i, size_t &n_points);
const float *mesh_skin_weights(int i, size_t &n_points);
// OpenUSD's own linear blend skinning of mesh i with joint-local transforms
// `local` (skeleton order, 12 floats a joint): world-space points, one per
// vertex of the point table; `normalize` first scales each vertex's weights to
// sum 1 (UsdSkelNormalizeWeights). Empty with `why` set on a bad input.
std::vector<float> mesh_skin_pose(int i, const float *local, size_t n_joints, bool normalize, std::string &why);

struct MaterialInfo {
	std::string path, name, shader_id; // shader_id "UsdPreviewSurface" or the surface's id / ""
	float diffuse[3] = { 0.18f, 0.18f, 0.18f };
	float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
	// Texture table indices, -1 when the input is a constant; channel is the
	// connected output ("rgb", "r", "g", "b", "a").
	int diffuse_tex = -1, metallic_tex = -1, roughness_tex = -1, opacity_tex = -1, normal_tex = -1;
	std::string diffuse_channel, metallic_channel, roughness_channel, opacity_channel, normal_channel;
	// The connected texture's authored file per input (same order as the
	// indices above), set even when it did not resolve: the wiring is the
	// material graph, whether or not the bytes are in the package.
	std::string file[5];
};
bool material_info(int i, MaterialInfo &out);

struct TextureInfo {
	std::string path; // the resolved package-relative path
	std::string file; // the asset path as authored
	size_t size = 0;
};
bool texture_info(int i, TextureInfo &out);
const uint8_t *texture_bytes(int i, size_t &n);

} // namespace usdg
