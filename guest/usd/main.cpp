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
#include <map>
#include <string>
#include <vector>

#include "common/blake3.h"
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
	if (!data) {
		return fail(std::string(what) + ": no such mesh or attribute");
	}
	if (from < 0 || (size_t)from > n_items) {
		return fail(std::string(what) + ": from out of range");
	}
	size_t cnt = count <= 0 ? n_items - (size_t)from : (size_t)count;
	if ((size_t)from + cnt > n_items) {
		cnt = n_items - (size_t)from;
	}
	if (cnt * stride * sizeof(float) > kWholeLimit) {
		return fail(std::string(what) + ": " + std::to_string(cnt * stride * sizeof(float)) + " bytes is past the 16 MiB view, use slices");
	}
	return Variant(PackedArray<float>(data + (size_t)from * stride, cnt * stride));
}

Variant int_slice(const char *what, const int32_t *data, size_t n_items, size_t stride, long from, long count) {
	if (!data) {
		return fail(std::string(what) + ": no such mesh");
	}
	if (from < 0 || (size_t)from > n_items) {
		return fail(std::string(what) + ": from out of range");
	}
	size_t cnt = count <= 0 ? n_items - (size_t)from : (size_t)count;
	if ((size_t)from + cnt > n_items) {
		cnt = n_items - (size_t)from;
	}
	if (cnt * stride * sizeof(int32_t) > kWholeLimit) {
		return fail(std::string(what) + ": " + std::to_string(cnt * stride * sizeof(int32_t)) + " bytes is past the 16 MiB view, use slices");
	}
	return Variant(PackedArray<int32_t>(data + (size_t)from * stride, cnt * stride));
}

Variant byte_slice(const char *what, const uint8_t *data, size_t n, long from, long count) {
	if (!data) {
		return fail(std::string(what) + ": no such texture");
	}
	if (from < 0 || (size_t)from > n) {
		return fail(std::string(what) + ": from out of range");
	}
	size_t cnt = count <= 0 ? n - (size_t)from : (size_t)count;
	if ((size_t)from + cnt > n) {
		cnt = n - (size_t)from;
	}
	if (cnt > kWholeLimit) {
		return fail(std::string(what) + ": " + std::to_string(cnt) + " bytes is past the 16 MiB view, use slices");
	}
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

// Streaming input: one file of a loose document at a time, by the path it is
// referenced under, then usd_open_asset names the root.
static Variant usd_asset(String path, PackedArray<uint8_t> bytes) {
	return guarded("usd_asset", [&] {
		std::vector<uint8_t> v = bytes.fetch();
		return text(usdg::asset_put(std::string(path), std::string(v.begin(), v.end())));
	});
}

static Variant usd_asset_push(String path, PackedArray<uint8_t> chunk) {
	return guarded("usd_asset_push", [&] {
		std::vector<uint8_t> v = chunk.fetch();
		return text(usdg::asset_push(std::string(path), std::string(v.begin(), v.end())));
	});
}

static Variant usd_asset_end(String path) {
	return guarded("usd_asset_end", [&] { return text(usdg::asset_end(std::string(path))); });
}

static Variant usd_open_asset(String path) {
	return guarded("usd_open_asset", [&] {
		return text(usdg::open_asset(std::string(path)) + " instr=" + std::to_string(instret()));
	});
}

static Variant usd_asset_list() {
	return guarded("usd_asset_list", [] {
		const std::vector<std::string> v = usdg::asset_list();
		std::string out;
		for (size_t i = 0; i < v.size(); ++i) {
			if (i) {
				out += "\n";
			}
			out += v[i];
		}
		return text(out);
	});
}

static Variant usd_asset_clear() {
	return guarded("usd_asset_clear", [] {
		usdg::asset_clear();
		return text("ok");
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
		if (!usdg::mesh_info(i, m)) {
			return fail("usd_mesh_info: no mesh " + std::to_string(i) + " (count " + std::to_string(usdg::mesh_count()) + ")");
		}
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
		d["skeleton"] = Variant(m.skeleton);
		d["double_sided"] = Variant(m.double_sided);
		d["surfaces"] = Variant(PackedArray<int32_t>(m.surfaces));
		d["shape_first"] = Variant(m.shape_first);
		d["shapes"] = Variant(m.shapes);
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
		if (!usdg::mesh_info(i, m)) {
			return fail("usd_mesh_transform: no mesh " + std::to_string(i));
		}
		return Variant(PackedArray<float>(m.xform, 16));
	});
}

static Variant usd_texture_info(int i) {
	return guarded("usd_texture_info", [&] {
		usdg::TextureInfo t;
		if (!usdg::texture_info(i, t)) {
			return fail("usd_texture_info: no texture " + std::to_string(i));
		}
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
		if (!usdg::curve_info(i, c)) {
			return fail("usd_curve_info: no curve " + std::to_string(i) + " (count " + std::to_string(usdg::curve_count()) + ")");
		}
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
		for (const std::pair<std::string, std::string> &kv : usdg::layer_data()) {
			d[kv.first] = text(kv.second);
		}
		return Variant(d);
	});
}

// Lines of text, one item per line; "k=v" lines for the metadata.
static std::vector<std::string> lines_of(const std::string &s) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < s.size()) {
		size_t nl = s.find('\n', at);
		if (nl == std::string::npos) {
			nl = s.size();
		}
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
		if (p.size() % 3 != 0) {
			return fail("usd_write_curves: " + std::to_string(p.size()) + " floats is not whole xyz points");
		}
		const std::vector<std::string> n = lines_of(names.utf8());
		std::vector<std::pair<std::string, std::string>> m;
		for (const std::string &kv : lines_of(meta.utf8())) {
			const size_t eq = kv.find('=');
			if (eq == std::string::npos) {
				return fail("usd_write_curves: meta line '" + kv + "' has no '='");
			}
			m.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
		}
		return text(usdg::write_curves(p.data(), p.size() / 3, c.data(), c.size(), n, b, m));
	});
}

// One mesh as a .usdz package (usdg::write_mesh): the arrays the usd_mesh_* readers return, written back.
// The host puts a call's arguments in registers a1..a7, so the call packs into four. header: line 1 is
// the mesh name, then k=v lines; np, nuv, nrest, nf and ni (counts of points, uv floats, rest floats,
// faces, corners) size the arrays and every other k=v goes to customLayerData. joints: one "name<TAB>parent"
// line per joint (parent -1 for a root, parents before children), empty for no skin. floats: points (3 np),
// uvs (nuv), rests (J x 24: parent-local rest 12 then world bind 12) and skin weights (np x element_size),
// back to back. ints: face corner counts (nf), corners (ni) and skin indices, back to back.
// Blend shapes (optional, after those blocks): nshapes=K; per shape i shape<i>=<name> (any text, even empty or a
// repeat: the writer makes working names, see usdg::BlendShapeIn), shape<i>_n=<listed points> and
// shape<i>_normals=0|1; per shape i with in-betweens shape<i>_ibs=M and, for j < M, shape<i>_ib<j>=<name> and
// shape<i>_ib<j>_normals=0|1; clip_frames=F and clip_fps=<timeCodesPerSecond>. floats then also hold, per shape
// in order: n x 3 offsets, n x 3 normal offsets when flagged, then per in-between: its weight (1 float, in
// (0, 1)), n x 3 offsets, n x 3 normal offsets when flagged; last F x K clip weights (row per frame, shape
// order, targets only). ints hold, per shape, its n strictly increasing point indices (an in-between uses its
// target's). Keys of the form shape<digits>... listed above are never customLayerData.
struct ShapeHdr {
	struct Ib {
		std::string name;
		bool normals = false, has_name = false;
	};
	std::string name;
	size_t n = 0, nib = 0;
	bool normals = false, has_name = false, has_n = false;
	std::map<size_t, Ib> ib;
};

// Reads "shape<i>", "shape<i>_n", "shape<i>_normals", "shape<i>_ibs", "shape<i>_ib<j>" and "shape<i>_ib<j>_normals";
// kind 0 name, 1 n, 2 normals, 3 ibs, 4 ib name, 5 ib normals.
static bool shape_key(const std::string &k, size_t &idx, size_t &jdx, int &kind) {
	if (k.compare(0, 5, "shape") != 0) {
		return false;
	}
	size_t e = 5;
	while (e < k.size() && k[e] >= '0' && k[e] <= '9') {
		++e;
	}
	if (e == 5 || e - 5 > 6) {
		return false;
	}
	idx = (size_t)std::atoll(k.c_str() + 5);
	const std::string suffix = k.substr(e);
	if (suffix.empty()) {
		kind = 0;
		return true;
	}
	if (suffix == "_n") {
		kind = 1;
		return true;
	}
	if (suffix == "_normals") {
		kind = 2;
		return true;
	}
	if (suffix == "_ibs") {
		kind = 3;
		return true;
	}
	if (suffix.compare(0, 3, "_ib") != 0) {
		return false;
	}
	size_t d = 3;
	while (d < suffix.size() && suffix[d] >= '0' && suffix[d] <= '9') {
		++d;
	}
	if (d == 3 || d - 3 > 6) {
		return false;
	}
	jdx = (size_t)std::atoll(suffix.c_str() + 3);
	const std::string tail = suffix.substr(d);
	if (tail.empty()) {
		kind = 4;
		return true;
	}
	if (tail == "_normals") {
		kind = 5;
		return true;
	}
	return false;
}

static Variant usd_write_mesh(PackedArray<float> floats, PackedArray<int32_t> ints, String header, String joints) {
	return guarded("usd_write_mesh", [&] {
		const std::vector<float> f = floats.fetch();
		const std::vector<int32_t> n = ints.fetch();
		std::string name;
		size_t np = 0, nuv = 0, nrest = 0, nf = 0, ni = 0, nshapes = 0, clip_frames = 0;
		double clip_fps = 0.0;
		std::map<size_t, ShapeHdr> sh;
		std::vector<std::pair<std::string, std::string>> m;
		for (const std::string &ln : lines_of(header.utf8())) {
			if (name.empty()) {
				name = ln;
				continue;
			}
			const size_t eq = ln.find('=');
			if (eq == std::string::npos) {
				return fail("usd_write_mesh: header line '" + ln + "' has no '='");
			}
			const std::string k = ln.substr(0, eq);
			const size_t v = (size_t)std::atoll(ln.c_str() + eq + 1);
			size_t si = 0, sj = 0;
			int kind = 0;
			if (k == "np") {
				np = v;
			} else if (k == "nuv") {
				nuv = v;
			} else if (k == "nrest") {
				nrest = v;
			} else if (k == "nf") {
				nf = v;
			} else if (k == "ni") {
				ni = v;
			} else if (k == "nshapes") {
				nshapes = v;
			} else if (k == "clip_frames") {
				clip_frames = v;
			} else if (k == "clip_fps") {
				clip_fps = std::atof(ln.c_str() + eq + 1);
			} else if (shape_key(k, si, sj, kind)) {
				ShapeHdr &h = sh[si];
				if (kind == 0) {
					h.name = ln.substr(eq + 1);
					h.has_name = true;
				} else if (kind == 1) {
					h.n = v;
					h.has_n = true;
				} else if (kind == 2) {
					h.normals = v != 0;
				} else if (kind == 3) {
					h.nib = v;
				} else if (kind == 4) {
					h.ib[sj].name = ln.substr(eq + 1);
					h.ib[sj].has_name = true;
				} else {
					h.ib[sj].normals = v != 0;
				}
			} else {
				m.emplace_back(k, ln.substr(eq + 1));
			}
		}
		// The blend-shape blocks sit after the skin blocks; their sizes come from the header.
		if (nshapes > 100000) {
			return fail("usd_write_mesh: nshapes=" + std::to_string(nshapes) + " is not plausible");
		}
		if (nshapes == 0 && (!sh.empty() || clip_frames != 0)) {
			return fail("usd_write_mesh: shape or clip keys without nshapes");
		}
		size_t shape_floats = 0, shape_ints = 0;
		for (size_t i = 0; i < nshapes; ++i) {
			const auto it = sh.find(i);
			if (it == sh.end() || !it->second.has_name || !it->second.has_n) {
				return fail("usd_write_mesh: shape" + std::to_string(i) + " needs shape" + std::to_string(i) + " and shape" +
						std::to_string(i) + "_n");
			}
			const ShapeHdr &h = it->second;
			if (h.n > n.size() || h.n > f.size()) {
				return fail("usd_write_mesh: shape" + std::to_string(i) + "_n is longer than the arrays");
			}
			if (h.nib > 10000 || (!h.ib.empty() && h.ib.rbegin()->first >= h.nib)) {
				return fail("usd_write_mesh: shape" + std::to_string(i) + "_ibs=" + std::to_string(h.nib) + " does not match its shape" +
						std::to_string(i) + "_ib<j> keys");
			}
			shape_floats += h.n * 3 * (h.normals ? 2 : 1);
			shape_ints += h.n;
			for (size_t j = 0; j < h.nib; ++j) {
				const auto jt = h.ib.find(j);
				if (jt == h.ib.end() || !jt->second.has_name) {
					return fail("usd_write_mesh: shape" + std::to_string(i) + "_ib" + std::to_string(j) + " is missing");
				}
				shape_floats += 1 + h.n * 3 * (jt->second.normals ? 2 : 1);
			}
		}
		if (!sh.empty() && sh.rbegin()->first >= nshapes) {
			return fail("usd_write_mesh: shape" + std::to_string(sh.rbegin()->first) + " is past nshapes=" + std::to_string(nshapes));
		}
		if (nshapes > 0 && clip_frames > 0) {
			if (clip_frames > f.size() / nshapes) {
				return fail("usd_write_mesh: clip_frames x nshapes is longer than the float array");
			}
			shape_floats += clip_frames * nshapes;
		}
		if (f.size() < np * 3 + nuv + nrest + shape_floats || n.size() < nf + ni + shape_ints) {
			return fail("usd_write_mesh: the arrays are shorter than the header sizes");
		}
		const size_t nw = f.size() - np * 3 - nuv - nrest - shape_floats, nsi = n.size() - nf - ni - shape_ints;
		const float *pts = f.data(), *uv = pts + np * 3, *rests = uv + nuv, *w = rests + nrest;
		std::vector<usdg::BlendShapeIn> shapes;
		usdg::BlendClipIn clip;
		{
			const float *sf = w + nw;
			const int32_t *sidx = n.data() + nf + ni + nsi;
			for (size_t i = 0; i < nshapes; ++i) {
				const ShapeHdr &h = sh[i];
				usdg::BlendShapeIn b;
				b.name = h.name;
				b.n = h.n;
				b.indices = sidx;
				b.offsets = sf;
				sidx += h.n;
				sf += h.n * 3;
				if (h.normals) {
					b.normals = sf;
					sf += h.n * 3;
				}
				for (size_t j = 0; j < h.nib; ++j) {
					const ShapeHdr::Ib &hi = h.ib.at(j);
					usdg::InbetweenIn ib;
					ib.name = hi.name;
					ib.weight = *sf++;
					ib.offsets = sf;
					sf += h.n * 3;
					if (hi.normals) {
						ib.normals = sf;
						sf += h.n * 3;
					}
					b.inbetweens.push_back(ib);
				}
				shapes.push_back(b);
			}
			if (nshapes > 0 && clip_frames > 0) {
				clip.frames = clip_frames;
				clip.fps = clip_fps;
				clip.weights = sf;
			}
		}
		std::vector<std::string> j;
		std::vector<int32_t> pa;
		for (const std::string &ln : lines_of(joints.utf8())) {
			const size_t tab = ln.find('\t');
			if (tab == std::string::npos) {
				return fail("usd_write_mesh: joint line '" + ln + "' has no tab and parent");
			}
			j.push_back(ln.substr(0, tab));
			pa.push_back(std::atoi(ln.c_str() + tab + 1));
		}
		if (nuv != 0 && nuv != np * 2) {
			return fail("usd_write_mesh: " + std::to_string(nuv) + " uv floats for " + std::to_string(np) + " points");
		}
		std::vector<float> rest, bind;
		size_t element_size = 0;
		if (!j.empty()) {
			if (nrest != j.size() * 24) {
				return fail("usd_write_mesh: rests are not J x 24");
			}
			if (np == 0 || nw == 0 || nw % np != 0 || nsi != nw) {
				return fail("usd_write_mesh: skin arrays are not points x element_size");
			}
			element_size = nw / np;
			for (size_t k = 0; k < j.size(); ++k) {
				rest.insert(rest.end(), rests + k * 24, rests + k * 24 + 12);
				bind.insert(bind.end(), rests + k * 24 + 12, rests + k * 24 + 24);
			}
		}
		const std::vector<int32_t> c(n.begin(), n.begin() + nf), ix(n.begin() + nf, n.begin() + nf + ni);
		const std::string z = usdg::write_mesh(name, pts, np, c, ix, nuv ? uv : nullptr, j, pa, rest.data(), bind.data(),
				n.data() + nf + ni, w, element_size, m, shapes, clip.frames ? &clip : nullptr);
		if (z.compare(0, 5, "ERR: ") == 0) {
			return text(z);
		}
		if (z.size() > kWholeLimit) {
			return fail("usd_write_mesh: " + std::to_string(z.size()) + " bytes is past the 16 MiB view");
		}
		return Variant(PackedArray<uint8_t>((const uint8_t *)z.data(), z.size()));
	});
}

// RFD66606-2277 A2: one motion clip as a UsdSkel .usda (usdg::write_skel_clip).
// rests: J x 24 floats, each joint's parent-local rest (12) then its world
// bind (12); anim_local: frames x anim_joints x 12. Names and meta one per line.
// Blend-shape weights in the same SkelAnimation (the call already uses the host's seven arguments, so the
// new data rides in the existing ones): meta keys nshapes=K and shape<i>=<name> for i < K (any text; the
// writer makes working names and keeps the originals, see usdg::BlendShapeIn; they are never
// customLayerData), and anim_local then holds, after the frames x anim_joints x 12 transform floats,
// frames x K weights (row per frame, shape order). frames = len / (anim_joints x 12 + K). With no animated
// joints the clip is weights only and meta clip_frames=F gives the frame count. Without nshapes the call
// is as it was.
static Variant usd_write_skel_clip(String names, PackedArray<int32_t> parents, PackedArray<float> rests,
		PackedArray<int32_t> anim_joints, PackedArray<float> anim_local, double fps, String meta) {
	return guarded("usd_write_skel_clip", [&] {
		const std::vector<std::string> n = lines_of(names.utf8());
		const std::vector<int32_t> p = parents.fetch();
		const std::vector<float> r = rests.fetch();
		const std::vector<int32_t> aj = anim_joints.fetch();
		const std::vector<float> al = anim_local.fetch();
		if (r.size() != n.size() * 24) {
			return fail("usd_write_skel_clip: " + std::to_string(r.size()) + " rest floats for " + std::to_string(n.size()) + " joints");
		}
		std::vector<float> rest_local(n.size() * 12), bind_world(n.size() * 12);
		for (size_t j = 0; j < n.size(); ++j) {
			std::copy(&r[j * 24], &r[j * 24 + 12], &rest_local[j * 12]);
			std::copy(&r[j * 24 + 12], &r[j * 24 + 24], &bind_world[j * 12]);
		}
		std::vector<std::pair<std::string, std::string>> m;
		size_t nshapes = 0, clip_frames = 0;
		std::map<size_t, std::string> shape_names;
		for (const std::string &kv : lines_of(meta.utf8())) {
			const size_t eq = kv.find('=');
			if (eq == std::string::npos) {
				return fail("usd_write_skel_clip: meta line '" + kv + "' has no '='");
			}
			const std::string k = kv.substr(0, eq);
			size_t si = 0, sj = 0;
			int kind = 0;
			if (k == "nshapes") {
				nshapes = (size_t)std::atoll(kv.c_str() + eq + 1);
			} else if (k == "clip_frames") {
				clip_frames = (size_t)std::atoll(kv.c_str() + eq + 1);
			} else if (shape_key(k, si, sj, kind) && kind == 0) {
				shape_names[si] = kv.substr(eq + 1);
			} else {
				m.emplace_back(k, kv.substr(eq + 1));
			}
		}
		if (nshapes > 100000) {
			return fail("usd_write_skel_clip: nshapes=" + std::to_string(nshapes) + " is not plausible");
		}
		if (nshapes == 0 && (!shape_names.empty() || clip_frames != 0)) {
			return fail("usd_write_skel_clip: shape or clip_frames keys without nshapes");
		}
		if (nshapes == 0) {
			if (aj.empty() || al.size() % (aj.size() * 12) != 0) {
				return fail("usd_write_skel_clip: animation floats are not frames x joints x 12");
			}
			return text(usdg::write_skel_clip(n, p, rest_local.data(), bind_world.data(), aj, al.data(),
					al.size() / (aj.size() * 12), fps, m));
		}
		std::vector<std::string> shapes;
		for (size_t i = 0; i < nshapes; ++i) {
			const auto it = shape_names.find(i);
			if (it == shape_names.end()) {
				return fail("usd_write_skel_clip: shape" + std::to_string(i) + " is missing");
			}
			shapes.push_back(it->second);
		}
		if (!shape_names.empty() && shape_names.rbegin()->first >= nshapes) {
			return fail("usd_write_skel_clip: shape" + std::to_string(shape_names.rbegin()->first) + " is past nshapes=" + std::to_string(nshapes));
		}
		const size_t row = aj.size() * 12 + nshapes;
		size_t frames = 0;
		if (aj.empty()) {
			frames = clip_frames;
			if (frames == 0 || al.size() != frames * nshapes) {
				return fail("usd_write_skel_clip: a weights-only clip needs clip_frames and frames x shapes floats");
			}
		} else {
			if (al.size() == 0 || al.size() % row != 0) {
				return fail("usd_write_skel_clip: animation floats are not frames x (joints x 12 + shapes)");
			}
			frames = al.size() / row;
			if (clip_frames != 0 && clip_frames != frames) {
				return fail("usd_write_skel_clip: clip_frames=" + std::to_string(clip_frames) + " but the floats hold " + std::to_string(frames));
			}
		}
		return text(usdg::write_skel_clip(n, p, rest_local.data(), bind_world.data(), aj, al.data(), frames, fps, m, shapes,
				al.data() + frames * aj.size() * 12));
	});
}

static Variant usd_skel_anim_count() {
	return Variant((int64_t)usdg::skel_anim_count());
}

static Variant usd_skel_anim_info(int i) {
	return guarded("usd_skel_anim_info", [&] {
		usdg::SkelAnimInfo a;
		if (!usdg::skel_anim_info(i, a)) {
			return fail("usd_skel_anim_info: no animation " + std::to_string(i));
		}
		Dictionary d = Dictionary::Create();
		d["path"] = text(a.path);
		d["joints"] = text(a.joints);
		d["frames"] = Variant((int64_t)a.frames);
		d["njoints"] = Variant((int64_t)a.njoints);
		d["fps"] = Variant(a.fps);
		d["blend_shapes"] = text(a.shapes);
		d["blend_shape_tokens"] = text(a.tokens);
		d["nshapes"] = Variant((int64_t)a.nshapes);
		d["times"] = text(a.times);
		return Variant(d);
	});
}

static Variant usd_skel_anim_transforms(int i) {
	size_t n = 0;
	const float *p = usdg::skel_anim_transforms(i, n);
	if (!p) {
		return fail("usd_skel_anim_transforms: no animation " + std::to_string(i));
	}
	if (n * sizeof(float) > kWholeLimit) {
		return fail("usd_skel_anim_transforms: past the 16 MiB view");
	}
	return Variant(PackedArray<float>(p, n));
}
static Variant usd_skel_anim_blend_weights(int i) {
	size_t n = 0;
	const float *p = usdg::skel_anim_blend_weights(i, n);
	if (!p) {
		return fail("usd_skel_anim_blend_weights: no animation " + std::to_string(i));
	}
	if (n * sizeof(float) > kWholeLimit) {
		return fail("usd_skel_anim_blend_weights: past the 16 MiB view");
	}
	return Variant(PackedArray<float>(p, n));
}

static Variant usd_blend_shape_count() {
	return Variant(usdg::blend_shape_count());
}

static Variant usd_blend_shape_info(int k) {
	return guarded("usd_blend_shape_info", [&] {
		usdg::BlendShapeInfo b;
		if (!usdg::blend_shape_info(k, b)) {
			return fail("usd_blend_shape_info: no blend shape " + std::to_string(k) + " (count " + std::to_string(usdg::blend_shape_count()) + ")");
		}
		Dictionary d = Dictionary::Create();
		d["name"] = text(b.name);
		d["token"] = text(b.token);
		d["mesh"] = Variant(b.mesh);
		d["primary"] = Variant(b.primary);
		d["position"] = Variant((double)b.position);
		d["has_normals"] = Variant(b.has_normals);
		d["points"] = Variant((int64_t)b.points);
		d["blake3_offsets"] = text(b.blake3_offsets);
		return Variant(d);
	});
}

static Variant usd_blend_shape_offsets_slice(int k, int from, int count) {
	return guarded("usd_blend_shape_offsets_slice", [&] {
		size_t n = 0;
		const float *p = usdg::blend_shape_offsets(k, n);
		return float_slice("usd_blend_shape_offsets_slice", p, n, 3, from, count);
	});
}

static Variant usd_blend_shape_normals_slice(int k, int from, int count) {
	return guarded("usd_blend_shape_normals_slice", [&] {
		size_t n = 0;
		const float *p = usdg::blend_shape_normals(k, n);
		return float_slice("usd_blend_shape_normals_slice", p, n, 3, from, count);
	});
}

static Variant usd_skeleton_count() {
	return Variant(usdg::skeleton_count());
}

static Variant usd_skeleton_info(int i) {
	return guarded("usd_skeleton_info", [&] {
		usdg::SkeletonInfo s;
		if (!usdg::skeleton_info(i, s)) {
			return fail("usd_skeleton_info: no skeleton " + std::to_string(i) + " (count " + std::to_string(usdg::skeleton_count()) + ")");
		}
		Dictionary d = Dictionary::Create();
		d["path"] = text(s.path);
		d["joints"] = text(s.joints);
		d["names"] = text(s.names);
		d["njoints"] = Variant((int64_t)s.parents.size());
		d["parents"] = Variant(PackedArray<int32_t>(s.parents.data(), s.parents.size()));
		d["animation"] = text(s.animation);
		d["rest_from_bind"] = Variant(s.rest_from_bind);
		d["bind_from_rest"] = Variant(s.bind_from_rest);
		d["xform"] = Variant(PackedArray<float>(s.xform, 16));
		return Variant(d);
	});
}

static Variant usd_skeleton_binds(int i) {
	return guarded("usd_skeleton_binds", [&] {
		size_t n = 0;
		const float *p = usdg::skeleton_binds(i, n);
		return float_slice("usd_skeleton_binds", p, n, 12, 0, 0);
	});
}

static Variant usd_skeleton_binds_slice(int i, int from, int count) {
	return guarded("usd_skeleton_binds_slice", [&] {
		size_t n = 0;
		const float *p = usdg::skeleton_binds(i, n);
		return float_slice("usd_skeleton_binds_slice", p, n, 12, from, count);
	});
}

static Variant usd_skeleton_rests(int i) {
	return guarded("usd_skeleton_rests", [&] {
		size_t n = 0;
		const float *p = usdg::skeleton_rests(i, n);
		return float_slice("usd_skeleton_rests", p, n, 12, 0, 0);
	});
}

static Variant usd_skeleton_rests_slice(int i, int from, int count) {
	return guarded("usd_skeleton_rests_slice", [&] {
		size_t n = 0;
		const float *p = usdg::skeleton_rests(i, n);
		return float_slice("usd_skeleton_rests_slice", p, n, 12, from, count);
	});
}

static Variant usd_mesh_skin(int i) {
	return guarded("usd_mesh_skin", [&] {
		usdg::SkinInfo k;
		if (!usdg::mesh_skin(i, k)) {
			return fail("usd_mesh_skin: mesh " + std::to_string(i) + " has no skin (meshes " + std::to_string(usdg::mesh_count()) + ")");
		}
		Dictionary d = Dictionary::Create();
		d["skeleton"] = Variant(k.skeleton);
		d["element_size"] = Variant(k.element_size);
		d["max_nonzero"] = Variant(k.max_nonzero);
		d["interpolation"] = text(k.interpolation);
		d["joints"] = text(k.joints);
		d["method"] = text(k.method);
		d["geom_bind"] = Variant(PackedArray<float>(k.geom_bind, 16));
		d["blake3_indices"] = text(k.blake3_indices);
		d["blake3_weights"] = text(k.blake3_weights);
		return Variant(d);
	});
}

static Variant usd_mesh_skin_indices(int i) {
	return guarded("usd_mesh_skin_indices", [&] {
		size_t n = 0;
		usdg::SkinInfo k;
		const int32_t *p = usdg::mesh_skin_indices(i, n);
		return int_slice("usd_mesh_skin_indices", p, n, usdg::mesh_skin(i, k) ? k.element_size : 1, 0, 0);
	});
}

static Variant usd_mesh_skin_indices_slice(int i, int from, int count) {
	return guarded("usd_mesh_skin_indices_slice", [&] {
		size_t n = 0;
		usdg::SkinInfo k;
		const int32_t *p = usdg::mesh_skin_indices(i, n);
		return int_slice("usd_mesh_skin_indices_slice", p, n, usdg::mesh_skin(i, k) ? k.element_size : 1, from, count);
	});
}

static Variant usd_mesh_skin_weights(int i) {
	return guarded("usd_mesh_skin_weights", [&] {
		size_t n = 0;
		usdg::SkinInfo k;
		const float *p = usdg::mesh_skin_weights(i, n);
		return float_slice("usd_mesh_skin_weights", p, n, usdg::mesh_skin(i, k) ? k.element_size : 1, 0, 0);
	});
}

static Variant usd_mesh_skin_weights_slice(int i, int from, int count) {
	return guarded("usd_mesh_skin_weights_slice", [&] {
		size_t n = 0;
		usdg::SkinInfo k;
		const float *p = usdg::mesh_skin_weights(i, n);
		return float_slice("usd_mesh_skin_weights_slice", p, n, usdg::mesh_skin(i, k) ? k.element_size : 1, from, count);
	});
}

static Variant usd_mesh_skin_pose(int i, PackedArray<float> local, bool normalize) {
	return guarded("usd_mesh_skin_pose", [&] {
		const std::vector<float> l = local.fetch();
		if (l.size() % 12 != 0) {
			return fail("usd_mesh_skin_pose: " + std::to_string(l.size()) + " floats is not whole joints of 12");
		}
		std::string why;
		const std::vector<float> p = usdg::mesh_skin_pose(i, l.data(), l.size() / 12, normalize, why);
		if (p.empty()) {
			return fail("usd_mesh_skin_pose: " + why);
		}
		return float_slice("usd_mesh_skin_pose", p.data(), p.size() / 3, 3, 0, 0);
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
		if (!usdg::material_info(i, m)) {
			return fail("usd_material: no material " + std::to_string(i) + " (count " + std::to_string(usdg::material_count()) + ")");
		}
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
		if (p && n <= kWholeLimit) {
			d["diffuse_bytes"] = Variant(PackedArray<uint8_t>(p, n));
		}
		return Variant(d);
	});
}

int main() {
	ADD_API_FUNCTION(usd_init, "String", "", "Register the embedded plugins and the in-memory resolver");
	ADD_API_FUNCTION(usd_open, "String", "PackedByteArray package",
			"Open a .usdz (or USDA/USDC) from bytes; ok layer=.. meshes=N materials=M textures=T .. or ERR:");
	ADD_API_FUNCTION(usd_push, "String", "PackedByteArray chunk", "Stage a piece of a package above the 16 MiB view");
	ADD_API_FUNCTION(usd_open_staged, "String", "", "Open the staged pieces as one package");
	ADD_API_FUNCTION(usd_asset, "String", "String path, PackedByteArray bytes",
			"Put one file of a loose document (its root layer, a sublayer or a texture) under its path");
	ADD_API_FUNCTION(usd_asset_push, "String", "String path, PackedByteArray chunk",
			"Stage a piece of one streamed file above the 16 MiB view");
	ADD_API_FUNCTION(usd_asset_end, "String", "String path", "Commit a streamed file's staged pieces");
	ADD_API_FUNCTION(usd_open_asset, "String", "String path", "Open the streamed document whose root layer is this path");
	ADD_API_FUNCTION(usd_asset_list, "String", "", "The streamed files' paths, one per line");
	ADD_API_FUNCTION(usd_asset_clear, "String", "", "Drop every streamed file");
	ADD_API_FUNCTION(usd_close, "String", "", "Drop the document and its tables");
	ADD_API_FUNCTION(usd_mesh_count, "int", "", "UsdGeomMesh prims in the document");
	ADD_API_FUNCTION(usd_material_count, "int", "", "Bound materials in the document");
	ADD_API_FUNCTION(usd_texture_count, "int", "", "Textures read out of the package");
	ADD_API_FUNCTION(usd_mesh_info, "Dictionary", "int i", "path, name, points, triangles, has_normals, has_uvs, indexed, material, blake3_points, blake3_indices, xform, skeleton, double_sided, surfaces (material, first triangle, triangles per GeomSubset), shape_first, shapes (blend-shape records)");
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
	ADD_API_FUNCTION(usd_write_mesh, "PackedByteArray",
			"PackedFloat32Array floats, PackedInt32Array ints, String header, String joints",
			"A .usdz package of one mesh with its uvs, skin, optional sparse blend shapes and weight clip (header keys nshapes, shape<i>, shape<i>_n, shape<i>_normals, clip_frames, clip_fps; four packed arguments: the host has seven registers), or an ERR: String");
	ADD_API_FUNCTION(usd_write_skel_clip, "String",
			"String names, PackedInt32Array parents, PackedFloat32Array rests, PackedInt32Array anim_joints, PackedFloat32Array anim_local, float fps, String meta",
			"A .usda of one UsdSkel motion clip (RFD 2277 A2), optionally with blend-shape weights in the same SkelAnimation (meta nshapes, shape<i>, clip_frames; weights after the transform floats), or ERR:");
	ADD_API_FUNCTION(usd_skel_anim_count, "int", "", "UsdSkelAnimation prims in the document");
	ADD_API_FUNCTION(usd_skel_anim_info, "Dictionary", "int i", "path, joints, frames, njoints, fps, blend_shapes (original names when kept, one per line), blend_shape_tokens (as authored), nshapes, times");
	ADD_API_FUNCTION(usd_skel_anim_transforms, "PackedFloat32Array", "int i", "frames x joints x 12: parent-local 3x3 (row-major) then translation");
	ADD_API_FUNCTION(usd_skel_anim_blend_weights, "PackedFloat32Array", "int i", "frames x nshapes blend-shape weights, in the clip's blend_shapes order");
	ADD_API_FUNCTION(usd_blend_shape_count, "int", "", "blend-shape records (targets and in-betweens) of all meshes");
	ADD_API_FUNCTION(usd_blend_shape_info, "Dictionary", "int k", "name (original when kept), token (as authored), mesh, primary, position, has_normals, points, blake3_offsets");
	ADD_API_FUNCTION(usd_blend_shape_offsets_slice, "PackedFloat32Array", "int k, int from, int count", "offsets of vertices [from, from+count), 3 floats a vertex");
	ADD_API_FUNCTION(usd_blend_shape_normals_slice, "PackedFloat32Array", "int k, int from, int count", "normal offsets of vertices [from, from+count)");
	ADD_API_FUNCTION(usd_skeleton_count, "int", "", "UsdSkelSkeleton prims in the document");
	ADD_API_FUNCTION(usd_skeleton_info, "Dictionary", "int i", "path, joints, names, njoints, parents, animation, rest_from_bind, bind_from_rest, xform");
	ADD_API_FUNCTION(usd_skeleton_binds, "PackedFloat32Array", "int i", "joints x 12: bindTransforms, 3x3 (row-major) then translation");
	ADD_API_FUNCTION(usd_skeleton_binds_slice, "PackedFloat32Array", "int i, int from, int count", "binds of joints [from, from+count)");
	ADD_API_FUNCTION(usd_skeleton_rests, "PackedFloat32Array", "int i", "joints x 12: parent-local restTransforms, 3x3 (row-major) then translation");
	ADD_API_FUNCTION(usd_skeleton_rests_slice, "PackedFloat32Array", "int i, int from, int count", "rests of joints [from, from+count)");
	ADD_API_FUNCTION(usd_mesh_skin, "Dictionary", "int i", "skeleton, element_size, max_nonzero, interpolation, joints, method, geom_bind, blake3_indices, blake3_weights");
	ADD_API_FUNCTION(usd_mesh_skin_indices, "PackedInt32Array", "int i", "element_size skeleton joint indices per vertex");
	ADD_API_FUNCTION(usd_mesh_skin_indices_slice, "PackedInt32Array", "int i, int from, int count", "joint indices of vertices [from, from+count)");
	ADD_API_FUNCTION(usd_mesh_skin_weights, "PackedFloat32Array", "int i", "element_size joint weights per vertex");
	ADD_API_FUNCTION(usd_mesh_skin_weights_slice, "PackedFloat32Array", "int i, int from, int count", "joint weights of vertices [from, from+count)");
	ADD_API_FUNCTION(usd_mesh_skin_pose, "PackedFloat32Array", "int i, PackedFloat32Array local, bool normalize",
			"OpenUSD's linear blend skinning of mesh i under joint-local transforms (joints x 12), weights normalized or as authored: world xyz per vertex");
	ADD_API_FUNCTION(usd_blake3, "String", "PackedByteArray bytes", "BLAKE3 hex of the bytes (the gate's transfer check)");
	halt();
}
