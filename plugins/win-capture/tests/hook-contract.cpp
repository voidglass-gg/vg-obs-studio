// Deterministic protocol and filesystem checks. Never load or inject a Hook.
#include <windows.h>
#include <voidglass-publication.h>
#include <util/windows/voidglass-dll-security.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static unsigned checks;
static void check(bool ok, const char *why)
{
	++checks;
	if (!ok)
		throw std::runtime_error(why);
}

static void write_pe(const std::wstring &path, WORD machine, bool dll, bool malformed = false)
{
	std::vector<unsigned char> bytes(512);
	auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(bytes.data());
	dos->e_magic = IMAGE_DOS_SIGNATURE;
	dos->e_lfanew = malformed ? INT32_MAX : 128;
	auto *signature = reinterpret_cast<DWORD *>(bytes.data() + 128);
	*signature = IMAGE_NT_SIGNATURE;
	auto *header = reinterpret_cast<IMAGE_FILE_HEADER *>(signature + 1);
	header->Machine = machine;
	header->SizeOfOptionalHeader = machine == IMAGE_FILE_MACHINE_I386 ? sizeof(IMAGE_OPTIONAL_HEADER32)
									  : sizeof(IMAGE_OPTIONAL_HEADER64);
	header->Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | (dll ? IMAGE_FILE_DLL : 0);
	*reinterpret_cast<WORD *>(header + 1) = machine == IMAGE_FILE_MACHINE_I386 ? IMAGE_NT_OPTIONAL_HDR32_MAGIC
										   : IMAGE_NT_OPTIONAL_HDR64_MAGIC;
	HANDLE file =
		CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	check(file != INVALID_HANDLE_VALUE, "create private PE test data");
	DWORD written;
	const bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) &&
			written == bytes.size();
	CloseHandle(file);
	check(ok, "write private PE test data");
}

int wmain(int argc, wchar_t **argv)
{
	try {
		check(argc == 2 && vg_dll_absolute(argv[1]), "explicit absolute unit directory");
		check(CreateDirectoryW(argv[1], nullptr) != FALSE, "unit directory must be new");
		wchar_t root[VG_DLL_PATH_CAPACITY];
		check(vg_dll_directory(argv[1], root), "canonical directory");
		const std::wstring good = std::wstring(root) + L"\\sample.dll";
		for (const wchar_t *bad : {L"hook.dll", L".\\hook.dll", L"C:hook.dll", L"\\\\host\\share\\hook.dll",
					   L"C:\\a\\..\\hook.dll", L"C:\\a\\hook.dll:stream", L"C:\\a\\hook.dll "})
			check(!vg_dll_absolute(bad), "ambiguous/non-local path rejected");
		check(!vg_dll_beneath(L"C:\\runtime", L"C:\\runtime-other\\hook.dll"), "root boundary");
		check(vg_dll_beneath(L"C:\\runtime", L"c:\\RUNTIME\\hook.dll"), "case-insensitive root");
		vg_dll_file file{};
		for (WORD machine : {WORD(IMAGE_FILE_MACHINE_I386), WORD(IMAGE_FILE_MACHINE_AMD64)}) {
			write_pe(good, machine, true);
			check(vg_dll_open(good.c_str(), root, machine, true, &file),
			      "PE32/PE32+ accepted with correct architecture");
			unsigned char digest[32];
			check(vg_dll_hash(file.handle, digest) && !memcmp(digest, file.sha256, 32),
			      "stable SHA-256 identity");
			HANDLE replace = CreateFileW(good.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
						     OPEN_EXISTING, 0, nullptr);
			check(replace == INVALID_HANDLE_VALUE, "verified file cannot be replaced while leased");
			vg_dll_close(&file);
			check(!vg_dll_open(good.c_str(), root,
					   machine == IMAGE_FILE_MACHINE_I386 ? IMAGE_FILE_MACHINE_AMD64
									      : IMAGE_FILE_MACHINE_I386,
					   true, &file),
			      "architecture mismatch rejected");
			check(!vg_dll_open(good.c_str(), root, machine, false, &file),
			      "DLL cannot masquerade as helper EXE");
		}
		write_pe(good, IMAGE_FILE_MACHINE_AMD64, true, true);
		check(!vg_dll_open(good.c_str(), root, IMAGE_FILE_MACHINE_AMD64, true, &file),
		      "PE offset beyond file rejected");
		DeleteFileW(good.c_str());
		check(RemoveDirectoryW(root) != FALSE, "private unit data cleaned");

		HANDLE absent_mapping = nullptr;
		const vg_pub_shared *absent_view = nullptr;
		check(vg_pub_open_reader(GetCurrentProcessId(), vg_pub_creation(GetCurrentProcess()), &absent_mapping,
					 &absent_view) == VG_PUB_ABSENT &&
			      !absent_mapping && !absent_view,
		      "historical Hook/no channel: explicit absence, no structure read");
		vg_pub_shared wire{};
		wire.data.magic = VG_PUB_MAGIC;
		wire.data.size = sizeof(vg_pub_payload);
		wire.data.major = VG_PUB_MAJOR;
		wire.data.minor = VG_PUB_MINOR;
		wire.data.capability = VG_CAP_SUPPORTED;
		wire.data.submitted = 9;
		wire.data.completed = 7;
		vg_pub_payload out{};
		check(vg_pub_read(&wire, sizeof(wire), &out) == VG_PUB_VALID && out.completed == 7,
		      "coherent snapshot");
		check(vg_pub_read(nullptr, 0, &out) == VG_PUB_INCOMPATIBLE, "absent structure never dereferenced");
		check(vg_pub_read(&wire, sizeof(wire) - 1, &out) == VG_PUB_INCOMPATIBLE, "short view rejected");
		wire.sequence = 1;
		check(vg_pub_read(&wire, sizeof(wire), &out) == VG_PUB_UNAVAILABLE,
		      "writer active rejected without waiting");
		wire.sequence = 2;
		for (uint32_t *field : {&wire.data.magic, &wire.data.size, &wire.data.major, &wire.data.minor}) {
			++*field;
			check(vg_pub_read(&wire, sizeof(wire), &out) == VG_PUB_INCOMPATIBLE, "unknown format rejected");
			--*field;
		}
		wire.data.capability = UINT32_MAX;
		check(vg_pub_read(&wire, sizeof(wire), &out) == VG_PUB_INCOMPATIBLE, "unknown capability rejected");
		wire.data.capability = VG_CAP_SUPPORTED;
		wire.data.completed = 10;
		check(vg_pub_read(&wire, sizeof(wire), &out) == VG_PUB_INCOMPATIBLE,
		      "completed cannot exceed submitted");
		check(vg_dll_harden_process(), "helper loader policy can be applied independently");
		PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy{};
		check(GetProcessMitigationPolicy(GetCurrentProcess(), ProcessImageLoadPolicy, &policy,
						 sizeof(policy)) &&
			      policy.PreferSystem32Images,
		      "helper loader policy actually present");
		std::cout << "PASS hook contract; checks=" << checks << " payload=" << sizeof(vg_pub_payload)
			  << " shared=" << sizeof(vg_pub_shared) << " pointer_bits=" << sizeof(void *) * 8 << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL " << error.what() << std::endl;
		return 1;
	}
}
