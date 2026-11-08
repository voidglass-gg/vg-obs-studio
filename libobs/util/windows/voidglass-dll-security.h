#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* Paths originate in the installed module, never in environment variables or CWD.
 * Hashes identify files; trust in the installation still depends on its ACLs. */
#define VG_DLL_PATH_CAPACITY 32768
typedef struct vg_dll_file {
	HANDLE handle;
	wchar_t path[VG_DLL_PATH_CAPACITY];
	WORD machine;
	unsigned char sha256[32];
} vg_dll_file;

static inline bool vg_dll_absolute(const wchar_t *path)
{
	if (!path || wcslen(path) < 3 ||
	    !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' ||
	    (path[2] != L'\\' && path[2] != L'/'))
		return false;
	const wchar_t *part = path + 3;
	for (const wchar_t *c = part;; ++c) {
		if (*c == L':' || *c == L'"' || (*c && *c < L' '))
			return false;
		if (!*c || *c == L'\\' || *c == L'/') {
			const size_t length = (size_t)(c - part);
			if (!length || part[length - 1] == L'.' || part[length - 1] == L' ')
				return false;
			if (!*c)
				return true;
			part = c + 1;
		}
	}
}

static inline bool vg_dll_final_path(HANDLE handle, wchar_t *path)
{
	DWORD count =
		GetFinalPathNameByHandleW(handle, path, VG_DLL_PATH_CAPACITY, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (!count || count >= VG_DLL_PATH_CAPACITY || wcsncmp(path, L"\\\\?\\", 4) || !vg_dll_absolute(path + 4))
		return false;
	wchar_t drive[] = {path[4], L':', L'\\', 0};
	if (GetDriveTypeW(drive) != DRIVE_FIXED)
		return false;
	memmove(path, path + 4, (wcslen(path + 4) + 1) * sizeof(wchar_t));
	return true;
}

static inline bool vg_dll_directory(const wchar_t *path, wchar_t *canonical)
{
	wchar_t normalized[VG_DLL_PATH_CAPACITY];
	if (!path || wcscpy_s(normalized, VG_DLL_PATH_CAPACITY, path))
		return false;
	size_t length = wcslen(normalized);
	while (length > 3 && (normalized[length - 1] == L'\\' || normalized[length - 1] == L'/'))
		normalized[--length] = 0;
	if (!vg_dll_absolute(normalized))
		return false;
	HANDLE handle = CreateFileW(normalized, FILE_READ_ATTRIBUTES,
				    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
				    FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return false;
	BY_HANDLE_FILE_INFORMATION info;
	bool ok = GetFileInformationByHandle(handle, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		  vg_dll_final_path(handle, canonical);
	CloseHandle(handle);
	return ok;
}

static inline bool vg_dll_beneath(const wchar_t *directory, const wchar_t *file)
{
	size_t length = wcslen(directory);
	return wcslen(file) > length && file[length] == L'\\' &&
	       CompareStringOrdinal(directory, (int)length, file, (int)length, TRUE) == CSTR_EQUAL;
}

static inline bool vg_dll_hash(HANDLE file, unsigned char *digest)
{
	BCRYPT_ALG_HANDLE algorithm = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	unsigned char buffer[65536];
	DWORD bytes = 0;
	LARGE_INTEGER zero = {0};
	bool ok = false;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
	    BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0 ||
	    !SetFilePointerEx(file, zero, NULL, FILE_BEGIN))
		goto done;
	for (;;) {
		if (!ReadFile(file, buffer, sizeof(buffer), &bytes, NULL))
			goto done;
		if (!bytes)
			break;
		if (BCryptHashData(hash, buffer, bytes, 0) < 0)
			goto done;
	}
	ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
done:
	if (hash)
		BCryptDestroyHash(hash);
	if (algorithm)
		BCryptCloseAlgorithmProvider(algorithm, 0);
	return ok;
}

static inline void vg_dll_hex(const unsigned char *digest, char *hex)
{
	for (size_t i = 0; i < 32; ++i) {
		hex[2 * i] = "0123456789abcdef"[digest[i] >> 4];
		hex[2 * i + 1] = "0123456789abcdef"[digest[i] & 15];
	}
	hex[64] = 0;
}

static inline void vg_dll_close(vg_dll_file *file)
{
	if (file->handle && file->handle != INVALID_HANDLE_VALUE)
		CloseHandle(file->handle);
	file->handle = NULL;
}

static inline bool vg_dll_open(const wchar_t *path, const wchar_t *directory, WORD machine, bool dll, vg_dll_file *file)
{
	file->handle = NULL;
	if (!vg_dll_absolute(path))
		return false;
	HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
				    FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return false;
	IMAGE_DOS_HEADER dos;
	struct {
		DWORD signature;
		IMAGE_FILE_HEADER header;
		WORD magic;
	} nt;
	LARGE_INTEGER size, offset;
	DWORD bytes;
	bool ok = vg_dll_final_path(handle, file->path) && vg_dll_beneath(directory, file->path) &&
		  GetFileSizeEx(handle, &size) && ReadFile(handle, &dos, sizeof(dos), &bytes, NULL) &&
		  bytes == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= (LONG)sizeof(dos);
	if (!ok)
		goto fail;
	offset.QuadPart = dos.e_lfanew;
	if ((uint64_t)offset.QuadPart + sizeof(nt) > (uint64_t)size.QuadPart ||
	    !SetFilePointerEx(handle, offset, NULL, FILE_BEGIN) || !ReadFile(handle, &nt, sizeof(nt), &bytes, NULL) ||
	    bytes != sizeof(nt) || nt.signature != IMAGE_NT_SIGNATURE || nt.header.Machine != machine ||
	    !(nt.header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
	    !!(nt.header.Characteristics & IMAGE_FILE_DLL) != dll ||
	    nt.header.SizeOfOptionalHeader < (machine == IMAGE_FILE_MACHINE_I386 ? sizeof(IMAGE_OPTIONAL_HEADER32)
										 : sizeof(IMAGE_OPTIONAL_HEADER64)) ||
	    (uint64_t)offset.QuadPart + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.header.SizeOfOptionalHeader >
		    (uint64_t)size.QuadPart ||
	    nt.magic != (machine == IMAGE_FILE_MACHINE_I386 ? IMAGE_NT_OPTIONAL_HDR32_MAGIC
							    : IMAGE_NT_OPTIONAL_HDR64_MAGIC) ||
	    !vg_dll_hash(handle, file->sha256))
		goto fail;
	file->machine = machine;
	file->handle = handle; /* Deny replacement until caller has finished using this identity. */
	return true;
fail:
	CloseHandle(handle);
	return false;
}

static inline bool vg_dll_process_directory(wchar_t *directory)
{
	DWORD count = GetModuleFileNameW(NULL, directory, VG_DLL_PATH_CAPACITY);
	if (!count || count >= VG_DLL_PATH_CAPACITY)
		return false;
	wchar_t *slash = wcsrchr(directory, L'\\');
	if (!slash)
		return false;
	*slash = 0;
	/* Caller keeps its executable mapped. Resolve the directory independently of CWD. */
	wchar_t canonical[VG_DLL_PATH_CAPACITY];
	if (!vg_dll_directory(directory, canonical))
		return false;
	wcscpy_s(directory, VG_DLL_PATH_CAPACITY, canonical);
	return true;
}

/* Executable helpers only. Never change the game's DLL search policy from a Hook. */
static inline bool vg_dll_harden_process(void)
{
	PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy = {0};
	policy.PreferSystem32Images = 1;
	return SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT) &&
	       SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) && SetDllDirectoryW(L"") &&
	       SetProcessMitigationPolicy(ProcessImageLoadPolicy, &policy, sizeof(policy));
}
