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
// up=Y|Z mpu=<metersPerUnit> ... textures_missing=N [missing=<file>,...]" or
// "ERR: <reason>". A previous document is closed first.
std::string open(const std::string &bytes);
void close();
// Streaming input: put each file of a loose document (its root layer, every
// sublayer, every texture) by the path it is referenced under, then open the
// root. A .usdz is one file; a bare .usda is the root of a tree. Nothing has to
// be bundled first. Files above the host's syscall view are pushed in pieces.
std::string asset_put(const std::string &rel, const std::string &bytes);
std::string asset_push(const std::string &rel, const std::string &chunk);
std::string asset_end(const std::string &rel);
std::string open_asset(const std::string &rel);
void asset_clear();
std::vector<std::string> asset_list();
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
	bool double_sided = false;
	// (material, first triangle, triangles) per run of a materialBind GeomSubset's faces;
	// empty when the mesh has none and every triangle takes `material`.
	std::vector<int32_t> surfaces;
	// The mesh's blend shapes: records [shape_first, shape_first + shapes) of the blend-shape table.
	int shape_first = 0, shapes = 0;
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

// UsdSkelAnimation prims (RFD66606-2277 A2's motion clips), in Traverse() order.
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
	std::string shapes; // the clip's blendShapes order, one name per line: originals (customData original_names) when authored
	std::string tokens; // the same order as authored in blendShapes (valid identifiers)
	size_t nshapes = 0;
	std::string times; // the frames' time codes, one per line (joint and weight samples, merged)
};
bool skel_anim_info(int i, SkelAnimInfo &out);
const float *skel_anim_transforms(int i, size_t &n_floats);
// frames x nshapes blend-shape weights, in the clip's blendShapes order.
const float *skel_anim_blend_weights(int i, size_t &n_floats);
// A new layer holding one motion clip: /Clip (a SkelRoot; upAxis Y,
// metersPerUnit 1, timeCodesPerSecond = fps, time codes 0 .. frames-1) with
// /Clip/Skeleton (every joint: joints are the names made valid identifiers
// and nested by `parents`, jointNames the names as given, bindTransforms the
// world rests, restTransforms the parent-local rests) and
// /Clip/Skeleton/Motion (a SkelAnimation of the joints in `anim_joints`,
// authored through UsdSkelAnimation::SetTransforms from anim_local, frames x
// anim_joints x 12). rest_local and bind_world are J x 12 in the same layout.
// With `shapes` not empty the same SkelAnimation also carries blendShapes (the working names of
// `shapes`, by the blend-shape name rule documented at BlendShapeIn; the originals in customData
// original_names) and blendShapeWeights.timeSamples at time codes 0 .. frames-1, `weights` being
// frames x shapes row-major; anim_joints may then be empty (a weights-only clip, `frames` given).
// Returns the .usda text or "ERR: <reason>".
std::string write_skel_clip(const std::vector<std::string> &names, const std::vector<int32_t> &parents,
		const float *rest_local, const float *bind_world, const std::vector<int32_t> &anim_joints,
		const float *anim_local, size_t frames, double fps,
		const std::vector<std::pair<std::string, std::string>> &meta,
		const std::vector<std::string> &shapes = {}, const float *weights = nullptr);

// One mesh as a .usdz package (a zip of one .usda member, stored and 64-byte aligned: the host
// has no streaming, so a mesh crosses as one package, as usd_open takes it). The layer is the
// writing half of mesh_points / mesh_indices / mesh_uvs /
// mesh_skin_*: /<name>/<name> is a UsdGeomMesh with `points` (xyz), per-face `counts` and
// corner `indices`, primvars:st from `uvs` (2 floats a point, vertex interpolation, null for
// none), and, when `joints` is not empty, a UsdSkelRoot /<name> with /<name>/Skeleton
// (jointNames as given, joints the names made valid identifiers nested by `parents`, bind and
// rest as J x 12) and the mesh's skel binding with skin_indices / skin_weights (n_points x
// element_size). Parents must come before their children, so joint order, and with it every
// skin index, is kept. `meta` goes in customLayerData. The text is parsed back before it is
// returned. Returns the .usdz bytes (they begin "PK"), or text beginning "ERR: <reason>".
//
// Blend shapes (the writing half of blend_shape_*): each BlendShapeIn becomes a UsdSkelBlendShape
// child /<name>/<name>/<working name> of the mesh, authored sparse: pointIndices (strictly
// increasing, inside the point table, at least one) and offsets, plus normalOffsets when `normals`
// is not null. Nothing is dropped or rounded. A mesh with shapes and no joints is a UsdSkelRoot (an
// Xform otherwise), so the shapes bind.
//
// Names follow the engine's rule (modules/gltf/gltf_document.cpp _gen_unique_name_static, FBX's
// _gen_unique_name): String::validate_node_name() replaces . : @ / " % with _, then
// String::validate_ascii_identifier() makes it a USD-valid identifier (empty -> "_", a leading digit
// gets "_" in front, every other non [A-Za-z0-9_] code point becomes "_"), then the first free one of
// <s>, <s>2, <s>3 ... (index appended without a separator) is taken. That working name is the prim
// name, the skel:blendShapes entry and the animation's blendShapes entry, so targets and tracks match.
// The name as given is kept as customData { string original_name = "..." } on the BlendShape prim
// (always written) and, for the clip, customData { string[] original_names } on the SkelAnimation.
//
// A BlendShapeIn may carry in-betweens (UsdSkelInbetweenShape, inbetweens:<working name> on the
// BlendShape prim, same working-name rule within the target, original in the attribute's customData
// original_name). An in-between has the target's pointIndices, so it gives offsets for the same n
// points (list the union of the points where the target or any in-between is non-zero); `normals`
// only when it has normal offsets; weight must be in (0, 1).
//
// A BlendClipIn adds a SkelAnimation (/<name>/Skeleton/Motion on a skinned mesh, bound by the
// skeleton's skel:animationSource; /<name>/Motion otherwise, bound by the mesh's) with blendShapes in
// the targets' order (in-betweens have no track) and blendShapeWeights.timeSamples at time codes
// 0 .. frames-1, weights frames x targets row-major; the stage's timeCodesPerSecond is fps. The
// animation carries no joints. Shapes empty and clip null: the output is as before.
struct InbetweenIn {
	std::string name;
	float weight = 0.5f; // in (0, 1)
	const float *offsets = nullptr; // n x 3, for the target's n points
	const float *normals = nullptr; // n x 3 or null
};
struct BlendShapeIn {
	std::string name;
	size_t n = 0; // listed points
	const int32_t *indices = nullptr; // n point indices
	const float *offsets = nullptr; // n x 3
	const float *normals = nullptr; // n x 3 normal offsets, or null
	std::vector<InbetweenIn> inbetweens;
};
struct BlendClipIn {
	size_t frames = 0;
	double fps = 0.0;
	const float *weights = nullptr; // frames x shapes
};
std::string write_mesh(const std::string &name, const float *points, size_t n_points, const std::vector<int32_t> &counts,
		const std::vector<int32_t> &indices, const float *uvs, const std::vector<std::string> &joints,
		const std::vector<int32_t> &parents, const float *rest_local, const float *bind_world, const int32_t *skin_indices,
		const float *skin_weights, size_t element_size, const std::vector<std::pair<std::string, std::string>> &meta,
		const std::vector<BlendShapeIn> &shapes = {}, const BlendClipIn *clip = nullptr);

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

// Blend shapes (UsdSkelBlendShape targets bound by skel:blendShapeTargets), all
// meshes' in one table, a mesh's records contiguous (MeshInfo::shape_first).
// Offsets (and normal offsets, when authored) are dense: 3 floats per vertex of
// the mesh's point table. An in-between is its own record: primary is the
// index of its target's record and position its weight in (0, 1); a target
// has primary -1, position 1. Names are the skel:blendShapes entry, and an
// in-between's is "<name>:<inbetween>".
int blend_shape_count();
struct BlendShapeInfo {
	std::string name; // the shape's original name when the file keeps one (customData original_name), else the authored token
	std::string token; // the authored name: the BlendShape prim's skel:blendShapes entry, a valid identifier
	int mesh = -1, primary = -1;
	float position = 1.0f;
	bool has_normals = false;
	size_t points = 0;
	std::string blake3_offsets;
};
bool blend_shape_info(int k, BlendShapeInfo &out);
const float *blend_shape_offsets(int k, size_t &n_points);
const float *blend_shape_normals(int k, size_t &n_points);
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
