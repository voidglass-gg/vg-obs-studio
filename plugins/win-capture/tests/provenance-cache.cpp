// Exercise the exact scheduler used by the plugin; no Toolhelp, OBS or injection.
#include "voidglass-provenance-cache.h"
#include <iostream>
#include <stdexcept>

static unsigned checks;
static void check(bool value, const char *message)
{
	++checks;
	if (!value)
		throw std::runtime_error(message);
}
struct diagnostic {
	unsigned hashes = 0, inventories = 0;
	bool reference_available = true;
	int observed = VG_HOOK_UNKNOWN;
	static bool prepare(void *data)
	{
		auto &self = *static_cast<diagnostic *>(data);
		++self.hashes;
		return self.reference_available;
	}
	static int observe(void *data)
	{
		auto &self = *static_cast<diagnostic *>(data);
		++self.inventories;
		return self.observed;
	}
};
static int poll(vg_provenance_cache &cache, diagnostic &fake, uint64_t now)
{
	return vg_provenance_poll(&cache, now, &fake, diagnostic::prepare, diagnostic::observe);
}
int main()
{
	try {
		vg_provenance_cache cache{};
		vg_provenance_key key{42, 1000, 99, 12, 0, false, false};
		diagnostic fake;
		check(vg_provenance_select(&cache, &key), "first target invalidates observations");
		check(poll(cache, fake, 0) == VG_HOOK_UNKNOWN, "first inventory unknown");
		for (unsigned i = 0; i < 10000; ++i)
			poll(cache, fake, 249);
		check(fake.hashes == 1 && fake.inventories == 1, "repeated calls before retry do no IO");
		poll(cache, fake, 250);
		poll(cache, fake, 1249);
		check(fake.inventories == 2, "second delay respected");
		poll(cache, fake, 1250);
		for (unsigned i = 0; i < 10000; ++i)
			poll(cache, fake, 1000000 + i);
		check(fake.hashes == 1 && fake.inventories == 3 && cache.result == VG_HOOK_UNKNOWN,
		      "unknown exhausted: three inventories total, one reference hash, never promoted");
		fake.observed = VG_HOOK_RUNTIME;
		check(poll(cache, fake, 2000000) == VG_HOOK_UNKNOWN, "elapsed time alone does not restore trust");
		key.channel = true; // A Hook can arrive well after the first retry window.
		check(vg_provenance_select(&cache, &key), "late first channel rearms bounded retries");
		check(poll(cache, fake, 2000000) == VG_HOOK_RUNTIME && fake.hashes == 1 && fake.inventories == 4,
		      "late runtime Hook recognized without rehash");
		for (unsigned i = 0; i < 10000; ++i)
			poll(cache, fake, 3000000 + i);
		check(fake.inventories == 4, "runtime result cached");
		check(!vg_provenance_select(&cache, &key), "same identity leaves cache intact");
		key.capturing = true;
		key.map_id = 7;
		check(vg_provenance_select(&cache, &key) && cache.result == VG_HOOK_UNKNOWN,
		      "native capture change invalidates runtime observation");
		fake.observed = VG_HOOK_EXTERNAL;
		check(poll(cache, fake, 3000000) == VG_HOOK_EXTERNAL && fake.hashes == 1,
		      "external confirmed, reference reused");
		for (unsigned i = 0; i < 10000; ++i)
			poll(cache, fake, 4000000 + i);
		check(fake.inventories == 5 && cache.result == VG_HOOK_EXTERNAL,
		      "external cached, never treated as runtime");
		for (unsigned field = 0; field < 6; ++field) {
			auto next = key;
			if (field == 0)
				++next.pid;
			if (field == 1)
				++next.creation;
			if (field == 2)
				++next.process;
			if (field == 3)
				++next.window;
			if (field == 4)
				++next.map_id;
			if (field == 5)
				next.capturing = !next.capturing;
			check(vg_provenance_select(&cache, &next) && cache.result == VG_HOOK_UNKNOWN && !cache.attempts,
			      "target/capture component change invalidates observations");
			const auto before = fake.hashes;
			poll(cache, fake, 5000000 + field);
			check(fake.hashes == before + (field < 3 ? 1 : 0),
			      "only target change refreshes pinned reference");
			key = next;
		}
		cache = {}; // Exact cache reset performed by stop_capture/destruction.
		fake = {};
		key = {42, 2000, 99, 12, 0, false, false};
		vg_provenance_select(&cache, &key);
		for (auto now : {0, 250, 1250})
			poll(cache, fake, now);
		key.capturing = true;
		key.map_id = 8; // Historical Hook, no additional channel.
		fake.observed = VG_HOOK_EXTERNAL;
		vg_provenance_select(&cache, &key);
		check(poll(cache, fake, 10000) == VG_HOOK_EXTERNAL && fake.hashes == 1,
		      "late historical capture recognized after budget exhaustion without a channel");
		cache = {};
		check(poll(cache, fake, 20000) == VG_HOOK_UNKNOWN,
		      "stop/invalid identity cannot reuse external observation");
		vg_provenance_select(&cache, &key);
		fake.reference_available = false;
		const auto hashes = fake.hashes, inventories = fake.inventories;
		for (unsigned i = 0; i < 10000; ++i)
			poll(cache, fake, 20000 + i * 1000);
		check(fake.hashes == hashes + 1 && fake.inventories == inventories && cache.result == VG_HOOK_UNKNOWN,
		      "unavailable reference stays unknown without repeated file attempts or inventories");
		fake.reference_available = true;
		fake.observed = VG_HOOK_RUNTIME;
		key.channel = true;
		vg_provenance_select(&cache, &key);
		check(poll(cache, fake, 20000000) == VG_HOOK_RUNTIME && fake.hashes == hashes + 2,
		      "a later real phase retries a missing reference without sticky failure");

		// Test the real file comparator using this executable: hash once, compare by pinned identity.
		wchar_t directory[VG_DLL_PATH_CAPACITY], path[VG_DLL_PATH_CAPACITY];
		check(vg_dll_process_directory(directory) && GetModuleFileNameW(nullptr, path, VG_DLL_PATH_CAPACITY),
		      "unit PE paths");
		vg_dll_file reference{};
		const WORD machine = sizeof(void *) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
		check(vg_dll_open(path, directory, machine, false, &reference), "pin real PE reference");
		check(vg_provenance_file(&reference, path) == VG_HOOK_RUNTIME,
		      "same canonical file identity confirmed");
		check(vg_provenance_file(&reference, L"relative.dll") == VG_HOOK_UNKNOWN,
		      "ambiguous observed path stays unknown");
		wchar_t kernel[VG_DLL_PATH_CAPACITY];
		check(GetModuleFileNameW(GetModuleHandleW(L"kernel32.dll"), kernel, VG_DLL_PATH_CAPACITY) != 0,
		      "system module path");
		check(vg_provenance_file(&reference, kernel) == VG_HOOK_EXTERNAL,
		      "different canonical file confirmed external");
		vg_dll_close(&reference);
		std::cout << "PASS provenance cache; checks=" << checks << " pointer_bits=" << sizeof(void *) * 8
			  << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL " << error.what() << std::endl;
		return 1;
	}
}
