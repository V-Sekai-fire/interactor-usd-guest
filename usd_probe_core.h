// Gate 0G: OpenUSD stage reading without a filesystem. Plain std types only,
// so the same TU builds into the guest (usd_probe.elf) and natively
// (tests/native/usd_probe, the flat control).
#pragma once

#include <string>

namespace usdp {

// Register the embedded plugInfo.json / generatedSchema.usda resources and the
// in-memory resolver; limit Work to one thread. Idempotent. Returns a status
// line ("ok plugins=N types=M" or "ERR: ...").
std::string init();

// Open USD bytes (USDA text or USDC crate, told apart by the PXR-USDC magic),
// traverse UsdGeomMesh and UsdSkel prims. path_mode: 0 = USDA through
// SdfLayer::ImportFromString on an anonymous layer, 1 = through the in-memory
// resolver (UsdStage::Open("mem:/input.<ext>")). Returns one line:
//   "ok fmt=usda prims=P meshes=M skels=S skelroots=R points=N fvi=F cksum=<hex>"
// or "ERR: <first error>".
std::string load(const std::string &bytes, int path_mode);

} // namespace usdp
