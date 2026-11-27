// Exercise the two production callbacks with a synthetic present chain.
#include "../graphics-hook/dxgi-capture.cpp"
#include <array>
#include <cstdio>
#include <thread>

static hook_info info{};
hook_info *global_hook_info = &info;
volatile bool active = true;
__declspec(thread) int vk_presenting;
char system_path[MAX_PATH]{};
void hlog(const char *, ...) {}
void hlog_hr(const char *, HRESULT) {}
void d3d10_capture(void *, void *) {}
void d3d10_free() {}
void d3d11_capture(void *, void *) {}
void d3d11_free() {}
void vg_pub_dxgi_installed() {}

static bool inner, inner_passthrough;
static int independent_thread;
static std::array<int, 3> depth;
static HRESULT dispatch(bool present1)
{
	if (inner) {
		depth[1] = dxgi_presenting;
		return E_FAIL;
	}
	depth[0] = dxgi_presenting;
	std::thread([] { independent_thread = dxgi_presenting; }).join();
	inner = true;
	const int saved = vk_presenting;
	vk_presenting = inner_passthrough;
	const HRESULT status = present1 ? hook_present(nullptr, 0, DXGI_PRESENT_TEST)
					: hook_present1(nullptr, 0, DXGI_PRESENT_TEST, nullptr);
	vk_presenting = saved;
	inner = false;
	depth[2] = dxgi_presenting;
	return status;
}
static HRESULT STDMETHODCALLTYPE present(IDXGISwapChain *, UINT, UINT) { return dispatch(false); }
static HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *)
{
	return dispatch(true);
}

int main()
{
	RealPresent = present;
	RealPresent1 = present1;
	for (bool first_is_present1 : {false, true}) {
		for (bool outer_pass : {false, true}) {
			for (bool inner_pass : {false, true}) {
				vk_presenting = outer_pass;
				inner_passthrough = inner_pass;
				depth = {};
				independent_thread = -1;
				const HRESULT hr = first_is_present1 ? hook_present1(nullptr, 0, 0, nullptr)
								     : hook_present(nullptr, 0, 0);
				if (hr != E_FAIL || depth != std::array<int, 3>{1, 2, 1} ||
				    dxgi_presenting != 0 || independent_thread != 0)
					return 1;
			}
		}
	}
	std::puts("PASS both Present nesting orders, normal/passthrough combinations, failed HRESULT and TLS isolation");
}
