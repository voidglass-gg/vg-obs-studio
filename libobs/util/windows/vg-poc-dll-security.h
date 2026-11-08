#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* Experimental P0 only. Inputs are local absolute DOS paths, never CWD/PATH. */
static __inline bool vg_poc_harden_dll_search(void)
{
	PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy = {0}, actual = {0};
	wchar_t marker[32], own[32];
	swprintf_s(own, 32, L"%lu", GetCurrentProcessId());
	if (GetEnvironmentVariableW(L"VG_POC_HARDENED_PID", marker, 32) > 0 && !wcscmp(marker, own))
		return !!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessImageLoadPolicy, &actual,
						    sizeof(actual)) &&
		       actual.PreferSystem32Images;
	if (!SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT) ||
	    !SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) || !SetDllDirectoryW(L""))
		return false;
	policy.PreferSystem32Images = 1;
	return !!SetProcessMitigationPolicy(ProcessImageLoadPolicy, &policy, sizeof(policy)) &&
	       !!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessImageLoadPolicy, &actual, sizeof(actual)) &&
	       actual.PreferSystem32Images && !!SetEnvironmentVariableW(L"VG_POC_HARDENED_PID", own);
}
static __inline bool vg_poc_dos_path(const wchar_t *s)
{
	if (!s || wcslen(s) < 3 || !((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')) ||
	    s[1] != L':' || (s[2] != L'\\' && s[2] != L'/'))
		return false;
	for (const wchar_t *c = s + 2; *c; ++c)
		if (*c == L':' || *c == L'"' || *c < L' ')
			return false;
	const wchar_t *part = s + 3;
	for (const wchar_t *c = part;; ++c) {
		if (!*c || *c == L'\\' || *c == L'/') {
			size_t n = (size_t)(c - part);
			if (!n || part[n - 1] == L'.' || part[n - 1] == L' ')
				return false;
			if (!*c)
				break;
			part = c + 1;
		}
	}
	return true;
}
static __inline bool vg_poc_absolute_path(const char *s)
{
	wchar_t wide[32768];
	return s && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, wide, 32768) > 0 && vg_poc_dos_path(wide);
}
static __inline bool vg_poc_final_path(HANDLE h, wchar_t *out)
{
	DWORD n = GetFinalPathNameByHandleW(h, out, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (!n || n >= 32768 || wcsncmp(out, L"\\\\?\\", 4) || !vg_poc_dos_path(out + 4))
		return false;
	wchar_t drive[] = {out[4], L':', L'\\', 0};
	return GetDriveTypeW(drive) == DRIVE_FIXED;
}
static __inline bool vg_poc_runtime_root(wchar_t *root, wchar_t *final)
{
	DWORD n = GetEnvironmentVariableW(L"VG_POC_RUNTIME_ROOT", root, 32768);
	if (!n || n >= 32768 || !vg_poc_dos_path(root))
		return false;
	HANDLE h = CreateFileW(root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	bool ok = vg_poc_final_path(h, final);
	CloseHandle(h);
	return ok;
}
static __inline bool vg_poc_sha(HANDLE h, wchar_t *hex)
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	PUCHAR object = NULL;
	DWORD bytes = 0, length = 0;
	unsigned char digest[32], buffer[65536];
	bool ok = false;
	LARGE_INTEGER zero = {0};
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
		goto done;
	if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&length, sizeof(length), &bytes, 0) < 0)
		goto done;
	object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, length);
	if (!object)
		goto done;
	if (BCryptCreateHash(alg, &hash, object, length, NULL, 0, 0) < 0)
		goto done;
	if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN))
		goto done;
	for (;;) {
		if (!ReadFile(h, buffer, sizeof(buffer), &bytes, NULL))
			goto done;
		if (!bytes)
			break;
		if (BCryptHashData(hash, buffer, bytes, 0) < 0)
			goto done;
	}
	if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
		goto done;
	for (unsigned i = 0; i < 32; ++i) {
		hex[i * 2] = L"0123456789abcdef"[digest[i] >> 4];
		hex[i * 2 + 1] = L"0123456789abcdef"[digest[i] & 15];
	}
	hex[64] = 0;
	ok = true;
done:
	if (hash)
		BCryptDestroyHash(hash);
	if (object)
		HeapFree(GetProcessHeap(), 0, object);
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}
static __inline HANDLE vg_poc_verified_file(const wchar_t *path, const wchar_t *sha_env)
{
	wchar_t root[32768], root_final[32768], file_final[32768], expected[65], actual[65];
	if (!vg_poc_dos_path(path) || !vg_poc_runtime_root(root, root_final) ||
	    GetEnvironmentVariableW(sha_env, expected, 65) != 64)
		return INVALID_HANDLE_VALUE;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return h;
	IMAGE_DOS_HEADER dos;
	DWORD read = 0;
	IMAGE_NT_HEADERS64 nt;
	LARGE_INTEGER pos, size;
	bool ok = vg_poc_final_path(h, file_final);
	size_t n = wcslen(root_final);
	ok = ok && wcslen(file_final) > n && file_final[n] == L'\\' &&
	     CompareStringOrdinal(root_final, (int)n, file_final, (int)n, TRUE) == CSTR_EQUAL;
	if (!ok || !GetFileSizeEx(h, &size) || !ReadFile(h, &dos, sizeof(dos), &read, NULL) || read != sizeof(dos) ||
	    dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
		goto fail;
	pos.QuadPart = dos.e_lfanew;
	if ((uint64_t)pos.QuadPart + sizeof(nt) > (uint64_t)size.QuadPart ||
	    !SetFilePointerEx(h, pos, NULL, FILE_BEGIN) || !ReadFile(h, &nt, sizeof(nt), &read, NULL) ||
	    read != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
	    nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
		goto fail;
	if (!vg_poc_sha(h, actual) || _wcsicmp(expected, actual))
		goto fail;
	return h; /* Caller retains the no-write/no-delete handle until its use ends. */
fail:
	CloseHandle(h);
	SetLastError(ERROR_ACCESS_DENIED);
	return INVALID_HANDLE_VALUE;
}
static __inline bool vg_poc_bootstrap(const char *role, const wchar_t *sha_env, const void *address)
{
	if (!vg_poc_harden_dll_search())
		return false;
	HMODULE module = NULL;
	if (address &&
	    !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)address, &module))
		return false;
	wchar_t path[32768], root[32768], canonical[32768], audit[32768];
	DWORD n = GetModuleFileNameW(module, path, 32768);
	if (!n || n >= 32768)
		return false;
	HANDLE checked = vg_poc_verified_file(path, sha_env);
	if (checked == INVALID_HANDLE_VALUE)
		return false;
	wchar_t loaded[32768], actual_hash[65];
	bool proven = vg_poc_final_path(checked, loaded) && vg_poc_sha(checked, actual_hash);
	CloseHandle(checked);
	if (!proven)
		return false;
	if (!vg_poc_runtime_root(root, canonical))
		return false;
	if (swprintf_s(audit, 32768, L"%s\\security-processes.tsv", root) < 0)
		return false;
	HANDLE output = CreateFileW(audit, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
				    FILE_ATTRIBUTE_NORMAL, NULL);
	if (output == INVALID_HANDLE_VALUE)
		return false;
	char utf8[32768], hash_utf8[65], line[34000];
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, loaded, -1, utf8, sizeof(utf8), NULL, NULL) ||
	    !WideCharToMultiByte(CP_UTF8, 0, actual_hash, -1, hash_utf8, sizeof(hash_utf8), NULL, NULL)) {
		CloseHandle(output);
		return false;
	}
	FILETIME creation, exit_time, kernel_time, user_time;
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel_time, &user_time)) { CloseHandle(output); return false; }
	uint64_t created = ((uint64_t)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
	int count = snprintf(line, sizeof(line), "%lu\t%s\tpolicy=1\tPreferSystem32Images=1\tarch=AMD64\t%s\t%s\tcreation=%llu\n",
			     GetCurrentProcessId(), role, hash_utf8, utf8, (unsigned long long)created);
	DWORD written = 0;
	bool ok = count > 0 && WriteFile(output, line, (DWORD)count, &written, NULL) && written == (DWORD)count;
	CloseHandle(output);
	return ok;
}
