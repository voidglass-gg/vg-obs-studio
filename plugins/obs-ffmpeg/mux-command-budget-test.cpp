#include <util/pipe.h>
#include <cstdio>
#include <cwchar>
#include <string>

static size_t padded_size(size_t length)
{
	auto *args = os_process_args_create("m");
	os_process_args_add_arg(args, std::string(length, 'x').c_str());
	const size_t units = os_process_args_windows_command_line_size(args);
	os_process_args_destroy(args);
	return units;
}

int main()
{
	auto *args = os_process_args_create(R"(C:\Test folder\m.exe)");
	os_process_args_add_arg(args, "");
	os_process_args_add_arg(args, u8"\u00e9\U0001f3ae");
	os_process_args_add_arg(args, "a b\\");
	os_process_args_add_arg(args, "\"q\"");
	const wchar_t expected[] = L"\"C:\\Test folder\\m.exe\" \"\" \u00e9\U0001f3ae \"a b\\\\\" \\\"q\\\"";
	const size_t units = os_process_args_windows_command_line_size(args);
	os_process_args_destroy(args);
	if (units != sizeof(expected) / sizeof(wchar_t) ||
	    padded_size(32764) != OS_WINDOWS_COMMAND_LINE_LIMIT || padded_size(32765) != OS_WINDOWS_COMMAND_LINE_LIMIT + 1)
		return 1;
	std::puts("PASS UTF-16, empty argument, escapes, trailing slash, 32767/32768 including NUL");
}
