// OpenUSD without a filesystem (Gate 0G, shared by usd_probe.elf and usd.elf):
// plugInfo.json through the Plug_InMemoryPlugInfoHook patch of the org's
// OpenUSD (V-Sekai-fire/OpenUSD, branch riscv64-sandbox), every other asset
// (generatedSchema.usda, the layers and packages the host pushes, the textures
// inside them) through UsdProbe_MemResolver, an ArResolver whose every asset
// is an ArInMemoryAsset over a buffer this table holds. Plain std types in the
// interface; the .cpp is a pxr-facing TU (gnu++17).
#pragma once

#include <cstddef>
#include <string>

namespace usdmem {

// Copy n bytes in as the asset at path (overwrites).
void put_asset(const std::string &path, const char *data, size_t n);
// Drop an asset (a package after usd_close). No-op when absent.
void erase_asset(const std::string &path);
// Size of an asset, or -1 when absent.
long asset_size(const std::string &path);

// Register the embedded resources and the resolver; Work limited to one
// thread. Idempotent. "ok plugins=N resolvers=M resources=R errors=E" or the
// same with " first_error=..." when the TfErrorMark was not clean.
std::string init();

} // namespace usdmem
