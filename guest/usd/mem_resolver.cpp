// See mem_resolver.h. Moved here from guest/usd_probe/usd_probe_core.cpp
// (Gate 0G) unchanged in behaviour so usd.elf (Cut U) shares it.

#include "mem_resolver.h"

#include "pxr/pxr.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/type.h"
#include "pxr/base/work/threadLimits.h"
#include "pxr/usd/ar/defineResolver.h"
#include "pxr/usd/ar/inMemoryAsset.h"
#include "pxr/usd/ar/resolver.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

// { path, bytes, size } for every embedded resource, generated at build time
// by gates/0g-openusd/gen_resources.py from the OpenUSD build tree.
struct UsdProbeResource {
	const char *path;
	const char *data;
	size_t size;
};
#include "usd_resources.inc"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

std::mutex g_mu;
std::map<std::string, std::shared_ptr<const char>> g_assets; // path -> bytes (NUL-terminated)
std::map<std::string, size_t> g_sizes;

const char *plug_hook(const char *path) {
	std::lock_guard<std::mutex> lk(g_mu);
	auto it = g_assets.find(path);
	return it == g_assets.end() ? nullptr : it->second.get();
}

} // namespace

PXR_NAMESPACE_OPEN_SCOPE
extern PLUG_API const char *(*Plug_InMemoryPlugInfoHook)(const char *pathname);
PXR_NAMESPACE_CLOSE_SCOPE

// Every identifier is its own resolved path; only the in-memory table exists.
// Package-relative paths (/mem/pkg.usdz[textures/x.png]) never reach _Resolve
// or _OpenAsset: Ar splits them and asks this resolver for the outer package
// only, then Sdf_UsdzResolver reads the member out of that asset.
class UsdProbe_MemResolver : public ArResolver {
public:
	UsdProbe_MemResolver() = default;

protected:
	std::string _CreateIdentifier(const std::string &assetPath, const ArResolvedPath &anchor) const override {
		if (assetPath.empty() || assetPath[0] == '/' || anchor.empty())
			return assetPath;
		std::string a = anchor.GetPathString();
		return a.substr(0, a.rfind('/') + 1) + assetPath;
	}
	std::string _CreateIdentifierForNewAsset(const std::string &assetPath, const ArResolvedPath &anchor) const override {
		return _CreateIdentifier(assetPath, anchor);
	}
	ArResolvedPath _Resolve(const std::string &assetPath) const override {
		std::lock_guard<std::mutex> lk(g_mu);
		return g_assets.count(assetPath) ? ArResolvedPath(assetPath) : ArResolvedPath();
	}
	ArResolvedPath _ResolveForNewAsset(const std::string &assetPath) const override {
		return ArResolvedPath(assetPath);
	}
	std::shared_ptr<ArAsset> _OpenAsset(const ArResolvedPath &resolvedPath) const override {
		std::lock_guard<std::mutex> lk(g_mu);
		auto it = g_assets.find(resolvedPath.GetPathString());
		if (it == g_assets.end())
			return nullptr;
		return ArInMemoryAsset::FromBuffer(std::shared_ptr<const char>(it->second), g_sizes[it->first]);
	}
	std::shared_ptr<ArWritableAsset> _OpenAssetForWrite(const ArResolvedPath &, WriteMode) const override {
		return nullptr;
	}
};

AR_DEFINE_RESOLVER(UsdProbe_MemResolver, ArResolver);

namespace usdmem {

static const char *kProbePlugInfo = R"({"Plugins":[{"Info":{"Types":{"UsdProbe_MemResolver":{"bases":["ArResolver"]}}},
"LibraryPath":"","Name":"usdProbe","ResourcePath":"resources","Root":"..","Type":"library"}]})";

void put_asset(const std::string &path, const char *data, size_t n) {
	std::shared_ptr<char> buf(new char[n + 1], std::default_delete<char[]>());
	std::memcpy(buf.get(), data, n);
	buf.get()[n] = 0;
	std::lock_guard<std::mutex> lk(g_mu);
	g_assets[path] = buf;
	g_sizes[path] = n;
}

void erase_asset(const std::string &path) {
	std::lock_guard<std::mutex> lk(g_mu);
	g_assets.erase(path);
	g_sizes.erase(path);
}

long asset_size(const std::string &path) {
	std::lock_guard<std::mutex> lk(g_mu);
	auto it = g_sizes.find(path);
	return it == g_sizes.end() ? -1 : (long)it->second;
}

static std::string first_error(const TfErrorMark &m) {
	for (auto it = m.GetBegin(); it != m.GetEnd(); ++it)
		return it->GetCommentary();
	return "unknown";
}

static std::string g_init;

std::string init() {
	if (!g_init.empty())
		return g_init;
	TfErrorMark mark;
	WorkSetConcurrencyLimit(1);
	std::vector<std::string> plugs;
	for (const UsdProbeResource &r : kUsdResources) {
		put_asset(r.path, r.data, r.size);
		std::string p = r.path;
		if (p.size() > 14 && p.compare(p.size() - 14, 14, "/plugInfo.json") == 0)
			plugs.push_back(p);
	}
	put_asset("/usd/usdProbe/resources/plugInfo.json", kProbePlugInfo, std::strlen(kProbePlugInfo));
	plugs.push_back("/usd/usdProbe/resources/plugInfo.json");
	Plug_InMemoryPlugInfoHook = plug_hook;
	ArSetPreferredResolver("UsdProbe_MemResolver");
	PlugPluginPtrVector added = PlugRegistry::GetInstance().RegisterPlugins(plugs);
	std::set<TfType> resolvers;
	PlugRegistry::GetAllDerivedTypes(TfType::Find<ArResolver>(), &resolvers);
	char buf[256];
	std::snprintf(buf, sizeof buf, "ok plugins=%zu resolvers=%zu resources=%zu errors=%zu", added.size(),
			resolvers.size(), sizeof(kUsdResources) / sizeof(kUsdResources[0]), (size_t)(mark.IsClean() ? 0 : 1));
	g_init = mark.IsClean() ? std::string(buf) : std::string(buf) + " first_error=" + first_error(mark);
	return g_init;
}

} // namespace usdmem
