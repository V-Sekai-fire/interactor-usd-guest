// usd_probe.elf -- Gate 0G: OpenUSD 26.05 (static, riscv64) reading a stage
// from bytes the host hands over, without a filesystem. The sandbox API is
// seen here only; usd_probe_core.cpp sees OpenUSD and std types only, and is
// built natively as the flat control (tests/native/usd_probe).
// Every ADD_API_FUNCTION has a no-argument wrapper in project/main.gd
// (AGENTS.md rule 8).

#include <api.hpp>

#include <string>
#include <vector>

#include "usd_probe_core.h"

static Variant usd_init() {
	return Variant(String(usdp::init()));
}

static Variant usd_load(PackedArray<uint8_t> bytes, int path_mode) {
	std::vector<uint8_t> v = bytes.fetch();
	return Variant(String(usdp::load(std::string(v.begin(), v.end()), path_mode)));
}

int main() {
	ADD_API_FUNCTION(usd_init, "String", "", "Register the embedded plugins and the in-memory resolver");
	ADD_API_FUNCTION(usd_load, "String", "PackedByteArray bytes, int path_mode",
			"Open USDA/USDC bytes (0: ImportFromString, 1: in-memory resolver); counts + checksum");
	halt();
}
