// Gate 0G: see usd_probe_core.h. No file is ever opened: plugInfo.json comes
// through the Plug_InMemoryPlugInfoHook patch (gates/0g-openusd/patch_usd.py),
// generatedSchema.usda and the input bytes through UsdProbe_MemResolver
// (guest/usd/mem_resolver.cpp since Cut U, where usd.elf shares it).

#include "usd_probe_core.h"

#include "mem_resolver.h"
#include "../common/blake3.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdSkel/root.h"
#include "pxr/usd/usdSkel/skeleton.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdp {

static std::string first_error(const TfErrorMark &m) {
	for (auto it = m.GetBegin(); it != m.GetEnd(); ++it)
		return it->GetCommentary();
	return "unknown";
}

std::string init() {
	return usdmem::init();
}


std::string load(const std::string &bytes, int path_mode) {
	init();
	TfErrorMark mark;
	const bool crate = bytes.size() >= 8 && std::memcmp(bytes.data(), "PXR-USDC", 8) == 0;
	const char *fmt = crate ? "usdc" : "usda";
	UsdStageRefPtr stage;
	try {
		if (!crate && path_mode == 0) {
			SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
			if (!layer->ImportFromString(bytes))
				return "ERR: fmt=usda ImportFromString failed: " + first_error(mark);
			stage = UsdStage::Open(layer, UsdStage::LoadAll);
		} else {
			// A fresh path per load: the layer registry never hands back a
			// cached layer for new bytes.
			static int serial = 0;
			std::string path = "/mem/input" + std::to_string(++serial) + "." + fmt;
			usdmem::put_asset(path, bytes.data(), bytes.size());
			SdfLayerRefPtr layer = SdfLayer::FindOrOpen(path);
			if (!layer)
				return std::string("ERR: fmt=") + fmt + " open failed: " + first_error(mark);
			stage = UsdStage::Open(layer, UsdStage::LoadAll);
		}
	} catch (const std::exception &e) {
		return std::string("ERR: fmt=") + fmt + " exception: " + e.what();
	}
	if (!stage)
		return std::string("ERR: fmt=") + fmt + " no stage: " + first_error(mark);

	size_t prims = 0, meshes = 0, skels = 0, roots = 0, npts = 0, nfvi = 0;
	blake3::Ctx h; // over each mesh's path, points and face-vertex indices, in Traverse() order
	for (const UsdPrim &prim : stage->Traverse()) {
		++prims;
		if (prim.IsA<UsdSkelSkeleton>())
			++skels;
		if (prim.IsA<UsdSkelRoot>())
			++roots;
		if (!prim.IsA<UsdGeomMesh>())
			continue;
		++meshes;
		UsdGeomMesh mesh(prim);
		VtVec3fArray pts;
		VtIntArray fvi;
		mesh.GetPointsAttr().Get(&pts, UsdTimeCode::Default());
		mesh.GetFaceVertexIndicesAttr().Get(&fvi, UsdTimeCode::Default());
		npts += pts.size();
		nfvi += fvi.size();
		h.update(prim.GetPath().GetString().data(), prim.GetPath().GetString().size());
		if (!pts.empty())
			h.update(pts.cdata(), pts.size() * sizeof(GfVec3f));
		if (!fvi.empty())
			h.update(fvi.cdata(), fvi.size() * sizeof(int));
	}
	char buf[256];
	std::snprintf(buf, sizeof buf, "ok fmt=%s prims=%zu meshes=%zu skels=%zu skelroots=%zu points=%zu fvi=%zu mesh_blake3=%.12s",
			fmt, prims, meshes, skels, roots, npts, nfvi, h.hex().c_str());
	std::string out = buf;
	if (!mark.IsClean())
		out += " warn=" + first_error(mark);
	return out;
}

} // namespace usdp
