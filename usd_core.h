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
};
bool mesh_info(int i, MeshInfo &out);
// Views into the flat tables; n is the element count (points / triangles).
// nullptr when i is out of range or the mesh has no such attribute.
const float *mesh_points(int i, size_t &n_points);
const float *mesh_normals(int i, size_t &n_points);
const float *mesh_uvs(int i, size_t &n_points);
const int32_t *mesh_indices(int i, size_t &n_triangles);

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
