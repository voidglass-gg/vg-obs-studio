#include "../capture-mapping.h"
#include <cstdio>
#include <cwchar>

static constexpr uint32_t child_bits = 0xC1234567;
static constexpr uint32_t root_bits = 0xA235059B;
static unsigned lookups, roots;

static HWND WINAPI fixture_root(HWND child, UINT which)
{
	++roots;
	return child == (HWND)(intptr_t)(int32_t)child_bits && which == GA_ROOT
		       ? (HWND)(intptr_t)(int32_t)root_bits : nullptr;
}

static HANDLE lookup(void *, const wchar_t *name)
{
	++lookups;
	return OpenFileMappingW(FILE_MAP_READ, FALSE, name);
}

static bool check(bool producer64, const wchar_t *expected)
{
	const uint32_t id = GetCurrentProcessId();
	wchar_t published[64], reference[64];
	swprintf(reference, 64, L"CaptureHook_Texture_%ls_%u", expected, id);
	const uint64_t producer_root = producer64 ? UINT64_C(0xffffffffa235059b) : root_bits;
	capture_mapping_name(published, producer_root, id);
	if (wcscmp(published, reference) != 0)
		return false;
	HANDLE owner = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, published);
	if (!owner)
		return false;
	DWORD first_error = ERROR_SUCCESS;
	lookups = roots = 0;
	HANDLE recovered = open_capture_mapping((HWND)(uintptr_t)0x10001, child_bits, id, producer64,
						nullptr, lookup, fixture_root, &first_error);
	const bool retried = recovered && first_error == ERROR_FILE_NOT_FOUND && lookups == 2 && roots == 1;
	if (recovered)
		CloseHandle(recovered);
	lookups = roots = 0;
	HANDLE direct = open_capture_mapping((HWND)(intptr_t)(int32_t)root_bits, child_bits, id, producer64,
					     nullptr, lookup, fixture_root, &first_error);
	const bool immediate = direct && lookups == 1 && roots == 0;
	if (direct)
		CloseHandle(direct);
	CloseHandle(owner);
	return retried && immediate;
}

int main()
{
	if (!check(false, L"2721383835") || !check(true, L"18446744072135968155"))
		return 1;
	std::printf("PASS %zu-bit reader: x86 and x64 root mappings, failed first lookup, direct lookup\n",
		    sizeof(void *) * 8);
	return 0;
}
