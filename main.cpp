// usd.elf (Cut U) -- OpenUSD 26.05 (the org's riscv64-sandbox branch, static)
// opens a .usdz package from bytes the host hands over, no filesystem, and
// answers with packed arrays: the loop's INFER state reads Pixal3D's answer
// here (AGENTS.md, the Stage 7 exception). The sandbox API is seen in this TU
// only; usd_core.cpp sees OpenUSD and std types only.
//
// Sizes: the host views at most 16 MiB of guest memory per syscall, so a
// package above that is pushed in pieces (usd_push + usd_open_staged) and an
// array above it is read in slices (usd_mesh_*_slice, usd_texture_slice); the
// whole-array calls answer a "FAIL: ... use slices" String past kWholeLimit.
// Every ADD_API_FUNCTION has a no-argument wrapper in project/main.gd (rule 8)
// through stages/usd_stage.gd.

#include <api.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../common/blake3.h"
#include "usd_core.h"

namespace {

constexpr size_t kWholeLimit = size_t(15) << 20; // bytes a whole-array answer may hold

std::string g_staged; // usd_push accumulates here until usd_open_staged

// libriscv's instruction counter (instret CSR) counts from the vmcall's start
// (Gate 6.P), so a call's instructions are the counter at its end.
uint64_t instret() {
	uint64_t v;
	asm volatile("rdinstret %0" : "=r"(v));
	return v;
}

Variant text(const std::string &s) {
	return Variant(String(s));
}

Variant fail(const std::string &what) {
	return text("FAIL: " + what);
}

template <typename F>
Variant guarded(const char *name, F &&f) {
	try {
		return f();
	} catch (const std::exception &e) {
		return fail(std::string(name) + ": " + e.what());
	} catch (...) {
		return fail(std::string(name) + ": unknown exception");
	}
}

Variant open_bytes(const std::string &bytes) {
	std::string r = usdg::open(bytes);
	return text(r + " instr=" + std::to_string(instret()));
}

// A slice [from, from + count) of items of `stride` floats; count <= 0 means
// to the end. Whole answers past kWholeLimit are refused.
Variant float_slice(const char *what, const float *data, size_t n_items, size_t stride, long from, long count) {
	if (!data)
		return fail(std::string(what) + ": no such mesh or attribute");
	if (from < 0 || (size_t)from > n_items)
		return fail(std::string(what) + ": from out of range");
	size_t cnt = count <= 0 ? n_items - (size_t)from : (size_t)count;
	if ((size_t)from + cnt > n_items)
		cnt = n_items - (size_t)from;
	if (cnt * stride * sizeof(float) > kWholeLimit)
		return fail(std::string(what) + ": " + std::to_string(cnt * stride * sizeof(float)) + " bytes is past the 16 MiB view, use slices");
	return Variant(PackedArray<float>(data + (size_t)from * stride, cnt * stride));
}

Variant int_slice(const char *what, const int32_t *data, size_t n_items, size_t stride, long from, long count) {
	if (!data)
		return fail(std::string(what) + ": no such mesh");
	if (from < 0 || (size_t)from > n_items)
		return fail(std::string(what) + ": from out of range");
	size_t cnt = count <= 0 ? n_items - (size_t)from : (size_t)count;
	if ((size_t)from + cnt > n_items)
		cnt = n_items - (size_t)from;
	if (cnt * stride * sizeof(int32_t) > kWholeLimit)
		return fail(std::string(what) + ": " + std::to_string(cnt * stride * sizeof(int32_t)) + " bytes is past the 16 MiB view, use slices");
	return Variant(PackedArray<int32_t>(data + (size_t)from * stride, cnt * stride));
}

Variant byte_slice(const char *what, const uint8_t *data, size_t n, long from, long count) {
	if (!data)
		return fail(std::string(what) + ": no such texture");
	if (from < 0 || (size_t)from > n)
		return fail(std::string(what) + ": from out of range");
	size_t cnt = count <= 0 ? n - (size_t)from : (size_t)count;
	if ((size_t)from + cnt > n)
		cnt = n - (size_t)from;
	if (cnt > kWholeLimit)
		return fail(std::string(what) + ": " + std::to_string(cnt) + " bytes is past the 16 MiB view, use slices");
	return Variant(PackedArray<uint8_t>(data + (size_t)from, cnt));
}

} // namespace

static Variant usd_init() {
	return guarded("usd_init", [] { return text(usdg::init()); });
}

static Variant usd_open(PackedArray<uint8_t> package) {
	return guarded("usd_open", [&] {
		std::vector<uint8_t> v = package.fetch();
		std::string().swap(g_staged);
		return open_bytes(std::string(v.begin(), v.end()));
	});
}

static Variant usd_push(PackedArray<uint8_t> chunk) {
	return guarded("usd_push", [&] {
		std::vector<uint8_t> v = chunk.fetch();
		g_staged.append(v.begin(), v.end());
		return text("staged=" + std::to_string(g_staged.size()));
	});
}

static Variant usd_open_staged() {
	return guarded("usd_open_staged", [] {
		std::string bytes;
		bytes.swap(g_staged);
		return open_bytes(bytes);
	});
}

static Variant usd_close() {
	return guarded("usd_close", [] {
		usdg::close();
		std::string().swap(g_staged);
		return text("ok");
	});
}

static Variant usd_mesh_count() {
	return Variant(usdg::mesh_count());
}

static Variant usd_material_count() {
	return Variant(usdg::material_count());
}

static Variant usd_texture_count() {
	return Variant(usdg::texture_count());
}

static Variant usd_mesh_info(int i) {
	return guarded("usd_mesh_info", [&] {
		usdg::MeshInfo m;
		if (!usdg::mesh_info(i, m))
			return fail("usd_mesh_info: no mesh " + std::to_string(i) + " (count " + std::to_string(usdg::mesh_count()) + ")");
		Dictionary d = Dictionary::Create();
		d["path"] = text(m.path);
		d["name"] = text(m.name);
		d["points"] = Variant((int64_t)m.points);
		d["triangles"] = Variant((int64_t)m.triangles);
		d["has_normals"] = Variant(m.has_normals);
		d["has_uvs"] = Variant(m.has_uvs);
		d["indexed"] = Variant(m.indexed);
		d["material"] = Variant(m.material);
		d["blake3_points"] = text(m.blake3_points);
		d["blake3_indices"] = text(m.blake3_indices);
		d["xform"] = Variant(PackedArray<float>(m.xform, 16));
		return Variant(d);
	});
}

static Variant usd_mesh_points(int i) {
	return guarded("usd_mesh_points", [&] {
		size_t n = 0;
		const float *p = usdg::mesh_points(i, n);
		return float_slice("usd_mesh_points", p, n, 3, 0, 0);
	});
}

static Variant usd_mesh_points_slice(int i, int from, int count) {
	return guarded("usd_mesh_points_slice", [&] {
		size_t n = 0;
		const float *p = usdg::mesh_points(i, n);
		return float_slice("usd_mesh_points_slice", p, n, 3, from, count);
	});
}

static Variant usd_mesh_normals(int i) {
	return guarded("usd_mesh_normals", [&] {
		size_t n = 0;
		const float *p = usdg::mesh_normals(i, n);
		return float_slice("usd_mesh_normals", p, n, 3, 0, 0);
	});
}

static Variant usd_mesh_normals_slice(int i, int from, int count) {
	return guarded("usd_mesh_normals_slice", [&] {
		size_t n = 0;
		const float *p = usdg::mesh_normals(i, n);
		return float_slice("usd_mesh_normals_slice", p, n, 3, from, count);
	});
}

static Variant usd_mesh_uvs(int i) {
	return guarded("usd_mesh_uvs", [&] {
		size_t n = 0;
		const float *p = usdg::mesh_uvs(i, n);
		return float_slice("usd_mesh_uvs", p, n, 2, 0, 0);
	});
}

static Variant usd_mesh_uvs_slice(int i, int from, int count) {
	return guarded("usd_mesh_uvs_slice", [&] {
		size_t n = 0;
		const float *p = usdg::mesh_uvs(i, n);
		return float_slice("usd_mesh_uvs_slice", p, n, 2, from, count);
	});
}

static Variant usd_mesh_indices(int i) {
	return guarded("usd_mesh_indices", [&] {
		size_t n = 0;
		const int32_t *p = usdg::mesh_indices(i, n);
		return int_slice("usd_mesh_indices", p, n, 3, 0, 0);
	});
}

static Variant usd_mesh_indices_slice(int i, int from, int count) {
	return guarded("usd_mesh_indices_slice", [&] {
		size_t n = 0;
		const int32_t *p = usdg::mesh_indices(i, n);
		return int_slice("usd_mesh_indices_slice", p, n, 3, from, count);
	});
}

static Variant usd_mesh_transform(int i) {
	return guarded("usd_mesh_transform", [&] {
		usdg::MeshInfo m;
		if (!usdg::mesh_info(i, m))
			return fail("usd_mesh_transform: no mesh " + std::to_string(i));
		return Variant(PackedArray<float>(m.xform, 16));
	});
}

static Variant usd_texture_info(int i) {
	return guarded("usd_texture_info", [&] {
		usdg::TextureInfo t;
		if (!usdg::texture_info(i, t))
			return fail("usd_texture_info: no texture " + std::to_string(i));
		Dictionary d = Dictionary::Create();
		d["path"] = text(t.path);
		d["file"] = text(t.file);
		d["size"] = Variant((int64_t)t.size);
		return Variant(d);
	});
}

// BLAKE3 hex of bytes the host hands back: Godot's HashingContext has no
// BLAKE3, so Gate U sums what crossed by sending it through here again.
static Variant usd_blake3(PackedArray<uint8_t> bytes) {
	return guarded("usd_blake3", [&] {
		const std::vector<uint8_t> b = bytes.fetch();
		return text(blake3::hex(b.data(), b.size()));
	});
}

static Variant usd_texture(int i) {
	return guarded("usd_texture", [&] {
		size_t n = 0;
		const uint8_t *p = usdg::texture_bytes(i, n);
		return byte_slice("usd_texture", p, n, 0, 0);
	});
}

static Variant usd_texture_slice(int i, int from, int count) {
	return guarded("usd_texture_slice", [&] {
		size_t n = 0;
		const uint8_t *p = usdg::texture_bytes(i, n);
		return byte_slice("usd_texture_slice", p, n, from, count);
	});
}

static Variant usd_curve_count() {
	return Variant(usdg::curve_count());
}

static Variant usd_curve_info(int i) {
	return guarded("usd_curve_info", [&] {
		usdg::CurveInfo c;
		if (!usdg::curve_info(i, c))
			return fail("usd_curve_info: no curve " + std::to_string(i) + " (count " + std::to_string(usdg::curve_count()) + ")");
		Dictionary d = Dictionary::Create();
		d["path"] = text(c.path);
		d["name"] = text(c.name);
		d["points"] = Variant((int64_t)c.points);
		d["boundary"] = Variant(c.boundary);
		d["blake3_points"] = text(c.blake3_points);
		d["xform"] = Variant(PackedArray<float>(c.xform, 16));
		return Variant(d);
	});
}

static Variant usd_curve_points(int i) {
	return guarded("usd_curve_points", [&] {
		size_t n = 0;
		const float *p = usdg::curve_points(i, n);
		return float_slice("usd_curve_points", p, n, 3, 0, 0);
	});
}

static Variant usd_layer_data() {
	return guarded("usd_layer_data", [] {
		Dictionary d = Dictionary::Create();
		for (const std::pair<std::string, std::string> &kv : usdg::layer_data())
			d[kv.first] = text(kv.second);
		return Variant(d);
	});
}

// Lines of text, one item per line; "k=v" lines for the metadata.
static std::vector<std::string> lines_of(const std::string &s) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < s.size()) {
		size_t nl = s.find('\n', at);
		if (nl == std::string::npos)
			nl = s.size();
		out.push_back(s.substr(at, nl - at));
		at = nl + 1;
	}
	return out;
}

static Variant usd_write_curves(PackedArray<float> points, PackedArray<int32_t> counts, PackedArray<int32_t> boundary,
		String names, String meta) {
	return guarded("usd_write_curves", [&] {
		const std::vector<float> p = points.fetch();
		const std::vector<int32_t> c = counts.fetch();
		const std::vector<int32_t> b = boundary.fetch();
		if (p.size() % 3 != 0)
			return fail("usd_write_curves: " + std::to_string(p.size()) + " floats is not whole xyz points");
		const std::vector<std::string> n = lines_of(names.utf8());
		std::vector<std::pair<std::string, std::string>> m;
		for (const std::string &kv : lines_of(meta.utf8())) {
			const size_t eq = kv.find('=');
			if (eq == std::string::npos)
				return fail("usd_write_curves: meta line '" + kv + "' has no '='");
			m.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
		}
		return text(usdg::write_curves(p.data(), p.size() / 3, c.data(), c.size(), n, b, m));
	});
}

// UsdPreviewSurface as a Dictionary. Each of diffuse / metallic / roughness /
// opacity / normal is a constant plus, when connected, <x>_texture (index
// into the texture table), <x>_channel and <x>_file; the diffuse texture's
// bytes ride along as diffuse_bytes when they fit the 16 MiB view (else the
// host reads usd_texture_slice by the index).
static Variant usd_material(int i) {
	return guarded("usd_material", [&] {
		usdg::MaterialInfo m;
		if (!usdg::material_info(i, m))
			return fail("usd_material: no material " + std::to_string(i) + " (count " + std::to_string(usdg::material_count()) + ")");
		Dictionary d = Dictionary::Create();
		d["path"] = text(m.path);
		d["name"] = text(m.name);
		d["shader_id"] = text(m.shader_id);
		d["diffuse"] = Variant(PackedArray<float>(m.diffuse, 3));
		d["metallic"] = Variant((double)m.metallic);
		d["roughness"] = Variant((double)m.roughness);
		d["opacity"] = Variant((double)m.opacity);
		struct Slot {
			const char *key;
			int tex;
			const std::string *channel;
			const std::string *file;
		} slots[] = { { "diffuse", m.diffuse_tex, &m.diffuse_channel, &m.file[0] },
			{ "metallic", m.metallic_tex, &m.metallic_channel, &m.file[1] },
			{ "roughness", m.roughness_tex, &m.roughness_channel, &m.file[2] },
			{ "opacity", m.opacity_tex, &m.opacity_channel, &m.file[3] }, { "normal", m.normal_tex, &m.normal_channel, &m.file[4] } };
		for (const Slot &s : slots) {
			const std::string k = s.key;
			d[k + "_texture"] = Variant(s.tex);
			d[k + "_channel"] = text(*s.channel);
			d[k + "_file"] = text(*s.file);
		}
		size_t n = 0;
		const uint8_t *p = usdg::texture_bytes(m.diffuse_tex, n);
		if (p && n <= kWholeLimit)
			d["diffuse_bytes"] = Variant(PackedArray<uint8_t>(p, n));
		return Variant(d);
	});
}

int main() {
	ADD_API_FUNCTION(usd_init, "String", "", "Register the embedded plugins and the in-memory resolver");
	ADD_API_FUNCTION(usd_open, "String", "PackedByteArray package",
			"Open a .usdz (or USDA/USDC) from bytes; ok layer=.. meshes=N materials=M textures=T .. or ERR:");
	ADD_API_FUNCTION(usd_push, "String", "PackedByteArray chunk", "Stage a piece of a package above the 16 MiB view");
	ADD_API_FUNCTION(usd_open_staged, "String", "", "Open the staged pieces as one package");
	ADD_API_FUNCTION(usd_close, "String", "", "Drop the document and its tables");
	ADD_API_FUNCTION(usd_mesh_count, "int", "", "UsdGeomMesh prims in the document");
	ADD_API_FUNCTION(usd_material_count, "int", "", "Bound materials in the document");
	ADD_API_FUNCTION(usd_texture_count, "int", "", "Textures read out of the package");
	ADD_API_FUNCTION(usd_mesh_info, "Dictionary", "int i", "path, name, points, triangles, has_normals, has_uvs, indexed, material, blake3_points, blake3_indices, xform");
	ADD_API_FUNCTION(usd_mesh_points, "PackedFloat32Array", "int i", "xyz per point");
	ADD_API_FUNCTION(usd_mesh_points_slice, "PackedFloat32Array", "int i, int from, int count", "points [from, from+count)");
	ADD_API_FUNCTION(usd_mesh_normals, "PackedFloat32Array", "int i", "xyz per point (per-point normals)");
	ADD_API_FUNCTION(usd_mesh_normals_slice, "PackedFloat32Array", "int i, int from, int count", "normals [from, from+count)");
	ADD_API_FUNCTION(usd_mesh_uvs, "PackedFloat32Array", "int i", "st per point (USD's bottom-left origin)");
	ADD_API_FUNCTION(usd_mesh_uvs_slice, "PackedFloat32Array", "int i, int from, int count", "uvs [from, from+count)");
	ADD_API_FUNCTION(usd_mesh_indices, "PackedInt32Array", "int i", "3 point indices per triangle, USD's right-handed order");
	ADD_API_FUNCTION(usd_mesh_indices_slice, "PackedInt32Array", "int i, int from, int count", "triangles [from, from+count)");
	ADD_API_FUNCTION(usd_mesh_transform, "PackedFloat32Array", "int i", "local-to-world 4x4, row-major, row 3 the origin");
	ADD_API_FUNCTION(usd_material, "Dictionary", "int i", "UsdPreviewSurface: diffuse, metallic, roughness, opacity, their textures");
	ADD_API_FUNCTION(usd_texture_info, "Dictionary", "int i", "path, file, size of a texture");
	ADD_API_FUNCTION(usd_texture, "PackedByteArray", "int i", "a texture's bytes as they are in the package");
	ADD_API_FUNCTION(usd_texture_slice, "PackedByteArray", "int i, int from, int count", "texture bytes [from, from+count)");
	ADD_API_FUNCTION(usd_curve_count, "int", "", "Curves (strokes) of the BasisCurves prims in the document");
	ADD_API_FUNCTION(usd_curve_info, "Dictionary", "int i", "path, name, points, boundary, blake3_points, xform");
	ADD_API_FUNCTION(usd_curve_points, "PackedFloat32Array", "int i", "xyz per point of curve i");
	ADD_API_FUNCTION(usd_layer_data, "Dictionary", "", "The document's customLayerData, values as text");
	ADD_API_FUNCTION(usd_write_curves, "String",
			"PackedFloat32Array points, PackedInt32Array counts, PackedInt32Array boundary, String names, String meta",
			"A .usda of linear BasisCurves under /Creation (names and k=v meta one per line), or ERR:");
	ADD_API_FUNCTION(usd_blake3, "String", "PackedByteArray bytes", "BLAKE3 hex of the bytes (the gate's transfer check)");
	halt();
}
