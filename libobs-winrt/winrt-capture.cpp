#include "winrt-capture.h"

#include <memory>

/* Only window capture opts in. Read once per capture to allow a process-wide rollback. */
static bool use_free_threaded_window(HWND window)
{
	char value[2]{};
	return window &&
	       !(GetEnvironmentVariableA("VG_WGC_FORCE_LEGACY", value, sizeof(value)) == 1 && value[0] == '1');
}

class capture_graphics_scope {
	bool entered;

public:
	explicit capture_graphics_scope(bool enter) : entered(enter)
	{
		if (entered)
			obs_enter_graphics();
	}
	~capture_graphics_scope()
	{
		if (entered)
			obs_leave_graphics();
	}
};

template<typename T> static void close_capture_resource(T &resource) noexcept
{
	/* Clear ownership before Close, including when Close throws. */
	T owned = std::move(resource);
	resource = nullptr;
	if (owned) {
		try {
			owned.Close();
		} catch (...) {
			blog(LOG_WARNING, "WGC resource Close failed (0x%08X)", winrt::to_hresult().value);
		}
	}
}

extern "C" EXPORT BOOL winrt_capture_supported()
try {
	/* no contract for IGraphicsCaptureItemInterop, verify 10.0.18362.0 */
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
		L"Windows.Foundation.UniversalApiContract", 8);
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

extern "C" EXPORT BOOL winrt_capture_cursor_toggle_supported()
try {
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
		L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled");
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_cursor_toggle_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_cursor_toggle_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

template<typename T>
static winrt::com_ptr<T> GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const &object)
{
	auto access = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
	winrt::com_ptr<T> result;
	winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
	return result;
}

static bool get_client_box(HWND window, uint32_t width, uint32_t height, D3D11_BOX *client_box)
{
	RECT client_rect{}, window_rect{};
	POINT upper_left{};

	/* check iconic (minimized) twice, ABA is very unlikely */
	bool client_box_available = !IsIconic(window) && GetClientRect(window, &client_rect) && !IsIconic(window) &&
				    (client_rect.right > 0) && (client_rect.bottom > 0) &&
				    (DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect,
							   sizeof(window_rect)) == S_OK) &&
				    ClientToScreen(window, &upper_left);
	if (client_box_available) {
		const uint32_t left = (upper_left.x > window_rect.left) ? (upper_left.x - window_rect.left) : 0;
		client_box->left = left;

		const uint32_t top = (upper_left.y > window_rect.top) ? (upper_left.y - window_rect.top) : 0;
		client_box->top = top;

		uint32_t texture_width = 1;
		if (width > left) {
			texture_width = std::min(width - left, (uint32_t)client_rect.right);
		}

		uint32_t texture_height = 1;
		if (height > top) {
			texture_height = std::min(height - top, (uint32_t)client_rect.bottom);
		}

		client_box->right = left + texture_width;
		client_box->bottom = top + texture_height;

		client_box->front = 0;
		client_box->back = 1;

		client_box_available = (client_box->right <= width) && (client_box->bottom <= height);
	}

	return client_box_available;
}

static DXGI_FORMAT get_pixel_format(HWND window, HMONITOR monitor, BOOL force_sdr)
{
	static constexpr DXGI_FORMAT sdr_format = DXGI_FORMAT_B8G8R8A8_UNORM;

	if (force_sdr)
		return sdr_format;

	if (window)
		monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);

	return (monitor && gs_is_monitor_hdr(monitor)) ? DXGI_FORMAT_R16G16B16A16_FLOAT : sdr_format;
}

struct winrt_capture {
	/* FreeThreaded has no FrameArrived/Closed handler holding a raw capture pointer.
	 * All application access, including teardown, is serialized by OBS graphics.
	 * Poll only on the graphics task thread; taking its lock on another thread
	 * does not make that thread the capture owner. */
	bool free_threaded = false;
	bool polling = false;
	uint64_t generation = 0;
	DXGI_FORMAT pool_format = DXGI_FORMAT_UNKNOWN;
	DWORD window_pid = 0;
	DWORD window_tid = 0;

	bool start_window_capture(ID3D11Device *d3d_device) noexcept;
	void poll_frame() noexcept;
	void close_window_capture() noexcept;
	bool needs_texture(uint32_t width, uint32_t height, DXGI_FORMAT pixel_format) const;
	bool copy_texture(ID3D11Texture2D *surface, const D3D11_BOX &box, DXGI_FORMAT pixel_format);

	~winrt_capture()
	{
		if (free_threaded)
			close_window_capture();
	}

	HWND window;
	BOOL client_area;
	BOOL force_sdr;
	HMONITOR monitor;
	DXGI_FORMAT format;

	bool capture_cursor;
	BOOL cursor_visible;

	gs_texture_t *texture;
	bool texture_written;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{nullptr};
	ComPtr<ID3D11DeviceContext> context;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool{nullptr};
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
	winrt::Windows::Graphics::SizeInt32 last_size;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker closed;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived;

	uint32_t texture_width;
	uint32_t texture_height;
	D3D11_BOX client_box;

	BOOL active;
	struct winrt_capture *next;

	void on_closed(winrt::Windows::Graphics::Capture::GraphicsCaptureItem const &,
		       winrt::Windows::Foundation::IInspectable const &)
	{
		active = FALSE;
	}

	void on_frame_arrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender,
			      winrt::Windows::Foundation::IInspectable const &)
	{
		const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
		const winrt::Windows::Graphics::SizeInt32 frame_content_size = frame.ContentSize();

		winrt::com_ptr<ID3D11Texture2D> frame_surface =
			GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

		/* need GetDesc because ContentSize is not reliable */
		D3D11_TEXTURE2D_DESC desc;
		frame_surface->GetDesc(&desc);

		obs_enter_graphics();

		if (desc.Format == get_pixel_format(window, monitor, force_sdr)) {
			if (!client_area || get_client_box(window, desc.Width, desc.Height, &client_box)) {
				if (client_area) {
					texture_width = client_box.right - client_box.left;
					texture_height = client_box.bottom - client_box.top;
				} else {
					texture_width = desc.Width;
					texture_height = desc.Height;
				}

				if (texture) {
					if (texture_width != gs_texture_get_width(texture) ||
					    texture_height != gs_texture_get_height(texture)) {
						gs_texture_destroy(texture);
						texture = nullptr;
					}
				}

				if (!texture) {
					const gs_color_format color_format =
						desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? GS_RGBA16F : GS_BGRA;
					texture = gs_texture_create(texture_width, texture_height, color_format, 1,
								    NULL, 0);
				}

				if (client_area) {
					context->CopySubresourceRegion((ID3D11Texture2D *)gs_texture_get_obj(texture),
								       0, 0, 0, 0, frame_surface.get(), 0, &client_box);
				} else {
					/* if they gave an SRV, we could avoid this copy */
					context->CopyResource((ID3D11Texture2D *)gs_texture_get_obj(texture),
							      frame_surface.get());
				}

				texture_written = true;
			}

			if (frame_content_size.Width != last_size.Width ||
			    frame_content_size.Height != last_size.Height) {
				format = desc.Format;
				frame_pool.Recreate(
					device,
					static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(format), 2,
					frame_content_size);

				last_size = frame_content_size;
			}
		} else {
			active = FALSE;
		}

		obs_leave_graphics();
	}
};

void winrt_capture::close_window_capture() noexcept
{
	active = FALSE;
	texture_written = false;
	texture_width = texture_height = 0;
	++generation;
	/* Stop the producer before releasing its frame pool. Neither has an
	 * application callback, so Close cannot wait for OBS graphics to be entered. */
	close_capture_resource(session);
	close_capture_resource(frame_pool);
	context = nullptr;
	device = nullptr;
	item = nullptr;
}

static gs_color_format window_texture_format(DXGI_FORMAT pixel_format)
{
	return pixel_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? GS_RGBA16F : GS_BGRA;
}

bool winrt_capture::needs_texture(uint32_t width, uint32_t height, DXGI_FORMAT pixel_format) const
{
	return !texture || width != gs_texture_get_width(texture) || height != gs_texture_get_height(texture) ||
	       window_texture_format(pixel_format) != gs_texture_get_color_format(texture);
}

/* If allocation is needed, surface must be owned independently of the WGC pool.
 * No checked-out frame may cross an OBS texture allocation or a device rebuild. */
bool winrt_capture::copy_texture(ID3D11Texture2D *surface, const D3D11_BOX &box, DXGI_FORMAT pixel_format)
{
	const uint32_t width = box.right - box.left;
	const uint32_t height = box.bottom - box.top;
	if (needs_texture(width, height, pixel_format)) {
		const uint64_t epoch = generation;
		texture_written = false;
		texture_width = texture_height = 0;
		gs_texture_destroy(texture);
		texture = nullptr;
		gs_texture_t *replacement =
			gs_texture_create(width, height, window_texture_format(pixel_format), 1, nullptr, 0);
		if (generation != epoch || !active) {
			gs_texture_destroy(replacement);
			return false;
		}
		texture = replacement;
		if (!texture)
			winrt::throw_hresult(E_OUTOFMEMORY);
	}
	context->CopySubresourceRegion((ID3D11Texture2D *)gs_texture_get_obj(texture), 0, 0, 0, 0, surface, 0, &box);
	format = pixel_format;
	texture_width = width;
	texture_height = height;
	texture_written = true;
	return true;
}

void winrt_capture::poll_frame() noexcept
{
	if (!free_threaded || !obs_in_task_thread(OBS_TASK_GRAPHICS))
		return;
	capture_graphics_scope graphics(true);
	if (polling || !active || !frame_pool)
		return;
	struct polling_scope {
		bool &flag;
		explicit polling_scope(bool &value) : flag(value) { flag = true; }
		~polling_scope() { flag = false; }
	} poll(polling);
	try {
		DWORD pid = 0;
		const DWORD tid = GetWindowThreadProcessId(window, &pid);
		if (!tid || pid != window_pid || tid != window_tid) {
			close_window_capture();
			return;
		}
		struct scoped_frame {
			winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame value{nullptr};
			~scoped_frame() { close_capture_resource(value); }
		} frame{frame_pool.TryGetNextFrame()};
		if (!frame.value)
			return;
		const auto size = frame.value.ContentSize();
		if (size.Width <= 0 || size.Height <= 0)
			return;
		const DXGI_FORMAT requested_format = get_pixel_format(window, nullptr, force_sdr);
		/* A transition frame can still have the old pool allocation. Do not copy
		 * undefined pixels on shrink, or crop an old allocation as the new size. */
		if (size.Width != last_size.Width || size.Height != last_size.Height ||
		    requested_format != pool_format) {
			close_capture_resource(frame.value);
			frame_pool.Recreate(
				device,
				static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(requested_format), 2,
				size);
			last_size = size;
			pool_format = requested_format;
			return;
		}
		auto surface = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.value.Surface());
		D3D11_TEXTURE2D_DESC desc{};
		surface->GetDesc(&desc);
		if (desc.Format != pool_format)
			winrt::throw_hresult(E_UNEXPECTED);
		/* ContentSize bounds valid pixels; GetDesc bounds allocated pixels. */
		const uint32_t width = std::min(desc.Width, (uint32_t)size.Width);
		const uint32_t height = std::min(desc.Height, (uint32_t)size.Height);
		D3D11_BOX box{0, 0, 0, width, height, 1};
		if (client_area && !get_client_box(window, width, height, &box))
			return;
		if (!width || !height)
			return;
		if (needs_texture(box.right - box.left, box.bottom - box.top, desc.Format)) {
			/* Keep the first usable frame, but return its borrowed surface before
			 * calling the OBS allocator. This temporary exists only on transitions. */
			D3D11_TEXTURE2D_DESC owned_desc{};
			owned_desc.Width = box.right - box.left;
			owned_desc.Height = box.bottom - box.top;
			owned_desc.MipLevels = owned_desc.ArraySize = owned_desc.SampleDesc.Count = 1;
			owned_desc.Format = desc.Format;
			owned_desc.Usage = D3D11_USAGE_DEFAULT;
			ComPtr<ID3D11Device> d3d_device;
			surface->GetDevice(&d3d_device);
			winrt::com_ptr<ID3D11Texture2D> owned;
			winrt::check_hresult(d3d_device->CreateTexture2D(&owned_desc, nullptr, owned.put()));
			context->CopySubresourceRegion(owned.get(), 0, 0, 0, 0, surface.get(), 0, &box);
			surface = nullptr;
			close_capture_resource(frame.value);
			box = {0, 0, 0, owned_desc.Width, owned_desc.Height, 1};
			copy_texture(owned.get(), box, desc.Format);
		} else {
			copy_texture(surface.get(), box, desc.Format);
		}
	} catch (...) {
		blog(LOG_WARNING, "WGC window frame failed (0x%08X)", winrt::to_hresult().value);
		close_window_capture();
	}
}

static struct winrt_capture *capture_list;

static void winrt_capture_device_loss_release(void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);
	if (capture->free_threaded) {
		capture_graphics_scope graphics(true);
		capture->close_window_capture();
		return;
	}
	capture->active = FALSE;

	capture->frame_arrived.revoke();

	try {
		capture->frame_pool.Close();
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X)", winrt::to_hresult().value);
	}

	try {
		capture->session.Close();
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X)", winrt::to_hresult().value);
	}

	capture->session = nullptr;
	capture->frame_pool = nullptr;
	capture->context = nullptr;
	capture->device = nullptr;
	capture->item = nullptr;
}

static bool winrt_capture_border_toggle_supported()
try {
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
		L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_border_toggle_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_border_toggle_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

static winrt::Windows::Graphics::Capture::GraphicsCaptureItem
winrt_capture_create_item(IGraphicsCaptureItemInterop *const interop_factory, HWND window, HMONITOR monitor)
{
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item = {nullptr};
	if (window) {
		try {
			const HRESULT hr = interop_factory->CreateForWindow(
				window, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
				reinterpret_cast<void **>(winrt::put_abi(item)));
			if (FAILED(hr))
				blog(LOG_ERROR, "CreateForWindow (0x%08X)", hr);
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "CreateForWindow (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "CreateForWindow (0x%08X)", winrt::to_hresult().value);
		}
	} else {
		assert(monitor);

		try {
			const HRESULT hr = interop_factory->CreateForMonitor(
				monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
				reinterpret_cast<void **>(winrt::put_abi(item)));
			if (FAILED(hr))
				blog(LOG_ERROR, "CreateForMonitor (0x%08X)", hr);
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "CreateForMonitor (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "CreateForMonitor (0x%08X)", winrt::to_hresult().value);
		}
	}

	return item;
}

bool winrt_capture::start_window_capture(ID3D11Device *d3d_device) noexcept
{
	close_window_capture();
	try {
		DWORD pid = 0;
		const DWORD tid = GetWindowThreadProcessId(window, &pid);
		if (!tid || tid != window_tid || pid != window_pid)
			return false;
		auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
		auto interop = factory.as<IGraphicsCaptureItemInterop>();
		item = winrt_capture_create_item(interop.get(), window, nullptr);
		if (!item)
			return false;
		ComPtr<IDXGIDevice> dxgi_device;
		winrt::check_hresult(d3d_device->QueryInterface(&dxgi_device));
		winrt::com_ptr<IInspectable> inspectable;
		winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put()));
		device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
		d3d_device->GetImmediateContext(&context);
		last_size = item.Size();
		if (last_size.Width <= 0 || last_size.Height <= 0)
			winrt::throw_hresult(E_INVALIDARG);
		pool_format = get_pixel_format(window, nullptr, force_sdr);
		frame_pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
			device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(pool_format), 2,
			last_size);
		session = frame_pool.CreateCaptureSession(item);
		if (winrt_capture_border_toggle_supported()) {
			winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
				winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
				.get();
			session.IsBorderRequired(false);
		}
		if (capture_cursor)
			session.IsCursorCaptureEnabled(cursor_visible);
		session.StartCapture();
		active = TRUE;
		return true;
	} catch (...) {
		blog(LOG_WARNING, "WGC window start/rebuild failed (0x%08X)", winrt::to_hresult().value);
		close_window_capture();
		return false;
	}
}

static void winrt_capture_device_loss_rebuild(void *device_void, void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);
	if (capture->free_threaded) {
		capture_graphics_scope graphics(true);
		capture->start_window_capture((ID3D11Device *)device_void);
		return;
	}

	auto activation_factory =
		winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
	auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item =
		winrt_capture_create_item(interop_factory.get(), capture->window, capture->monitor);
	if (!item)
		return;

	ID3D11Device *const d3d_device = (ID3D11Device *)device_void;
	ComPtr<IDXGIDevice> dxgi_device;
	if (FAILED(d3d_device->QueryInterface(&dxgi_device)))
		blog(LOG_ERROR, "Failed to get DXGI device");

	winrt::com_ptr<IInspectable> inspectable;
	if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put())))
		blog(LOG_ERROR, "Failed to get WinRT device");

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device =
		inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool =
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
			device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(capture->format), 2,
			capture->last_size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session = frame_pool.CreateCaptureSession(item);

	if (winrt_capture_border_toggle_supported()) {
		winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
			winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
			.get();
		session.IsBorderRequired(false);
	}

	if (winrt_capture_cursor_toggle_supported())
		session.IsCursorCaptureEnabled(capture->capture_cursor && capture->cursor_visible);

	capture->item = item;
	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->frame_arrived =
		frame_pool.FrameArrived(winrt::auto_revoke, {capture, &winrt_capture::on_frame_arrived});

	try {
		session.StartCapture();
		capture->active = TRUE;
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "StartCapture (0x%08X): %s", err.code().value, winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "StartCapture (0x%08X)", winrt::to_hresult().value);
	}
}

static struct winrt_capture *winrt_capture_init_internal(BOOL cursor, HWND window, BOOL client_area, BOOL force_sdr,
							 HMONITOR monitor)
try {
	if (use_free_threaded_window(window)) {
		capture_graphics_scope graphics(true);
		auto capture = std::make_unique<winrt_capture>();
		capture->free_threaded = true;
		capture->window = window;
		capture->window_tid = GetWindowThreadProcessId(window, &capture->window_pid);
		capture->client_area = client_area;
		capture->force_sdr = force_sdr;
		capture->capture_cursor = winrt_capture_cursor_toggle_supported();
		capture->cursor_visible = cursor;
		if (!capture->start_window_capture((ID3D11Device *)gs_get_device_obj()))
			return nullptr;
		gs_device_loss callbacks{winrt_capture_device_loss_release, winrt_capture_device_loss_rebuild,
					 capture.get()};
		gs_register_loss_callbacks(&callbacks);
		capture->next = capture_list;
		capture_list = capture.get();
		return capture.release();
	}
	ID3D11Device *const d3d_device = (ID3D11Device *)gs_get_device_obj();
	ComPtr<IDXGIDevice> dxgi_device;

	HRESULT hr = d3d_device->QueryInterface(&dxgi_device);
	if (FAILED(hr)) {
		blog(LOG_ERROR, "Failed to get DXGI device");
		return nullptr;
	}

	winrt::com_ptr<IInspectable> inspectable;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put());
	if (FAILED(hr)) {
		blog(LOG_ERROR, "Failed to get WinRT device");
		return nullptr;
	}

	auto activation_factory =
		winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
	auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item =
		winrt_capture_create_item(interop_factory.get(), window, monitor);
	if (!item)
		return nullptr;

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device =
		inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::SizeInt32 size = item.Size();
	const DXGI_FORMAT format = get_pixel_format(window, monitor, force_sdr);
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool =
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
			device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(format), 2, size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session = frame_pool.CreateCaptureSession(item);

	if (winrt_capture_border_toggle_supported()) {
		winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
			winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
			.get();
		session.IsBorderRequired(false);
	}

	/* disable cursor capture if possible since ours performs better */
	const BOOL cursor_toggle_supported = winrt_capture_cursor_toggle_supported();
	if (cursor_toggle_supported)
		session.IsCursorCaptureEnabled(cursor);

	struct winrt_capture *capture = new winrt_capture{};
	capture->window = window;
	capture->client_area = client_area;
	capture->force_sdr = force_sdr;
	capture->monitor = monitor;
	capture->format = format;
	capture->capture_cursor = cursor && cursor_toggle_supported;
	capture->cursor_visible = cursor;
	capture->item = item;
	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->last_size = size;
	capture->closed = item.Closed(winrt::auto_revoke, {capture, &winrt_capture::on_closed});
	capture->frame_arrived =
		frame_pool.FrameArrived(winrt::auto_revoke, {capture, &winrt_capture::on_frame_arrived});
	capture->next = capture_list;
	capture_list = capture;

	session.StartCapture();
	capture->active = TRUE;

	gs_device_loss callbacks;
	callbacks.device_loss_release = winrt_capture_device_loss_release;
	callbacks.device_loss_rebuild = winrt_capture_device_loss_rebuild;
	callbacks.data = capture;
	gs_register_loss_callbacks(&callbacks);

	return capture;

} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_init (0x%08X): %s", err.code().value, winrt::to_string(err.message()).c_str());
	return nullptr;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_init (0x%08X)", winrt::to_hresult().value);
	return nullptr;
}

extern "C" EXPORT struct winrt_capture *winrt_capture_init_window(BOOL cursor, HWND window, BOOL client_area,
								  BOOL force_sdr)
{
	return winrt_capture_init_internal(cursor, window, client_area, force_sdr, NULL);
}

extern "C" EXPORT struct winrt_capture *winrt_capture_init_monitor(BOOL cursor, HMONITOR monitor, BOOL force_sdr)
{
	return winrt_capture_init_internal(cursor, NULL, false, force_sdr, monitor);
}

extern "C" EXPORT void winrt_capture_free(struct winrt_capture *capture)
{
	if (capture) {
		capture_graphics_scope graphics(capture->free_threaded);
		struct winrt_capture *current = capture_list;
		if (current == capture) {
			capture_list = capture->next;
		} else {
			struct winrt_capture *previous;
			do {
				previous = current;
				current = current->next;
			} while (current != capture);

			previous->next = current->next;
		}

		obs_enter_graphics();
		gs_unregister_loss_callbacks(capture);
		gs_texture_destroy(capture->texture);
		obs_leave_graphics();

		if (capture->free_threaded) {
			capture->close_window_capture();
			delete capture;
			return;
		}
		capture->frame_arrived.revoke();
		capture->closed.revoke();

		try {
			if (capture->frame_pool)
				capture->frame_pool.Close();
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X)", winrt::to_hresult().value);
		}

		try {
			if (capture->session)
				capture->session.Close();
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X)", winrt::to_hresult().value);
		}

		delete capture;
	}
}

extern "C" EXPORT BOOL winrt_capture_active(const struct winrt_capture *capture)
{
	capture_graphics_scope graphics(capture->free_threaded);
	const_cast<winrt_capture *>(capture)->poll_frame();
	return capture->active;
}

extern "C" EXPORT BOOL winrt_capture_show_cursor(struct winrt_capture *capture, BOOL visible)
{
	capture_graphics_scope graphics(capture->free_threaded);
	BOOL success = FALSE;

	try {
		if (capture->capture_cursor) {
			if (capture->cursor_visible != visible) {
				capture->session.IsCursorCaptureEnabled(visible);
				capture->cursor_visible = visible;
			}
		}

		success = TRUE;
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "GraphicsCaptureSession::IsCursorCaptureEnabled (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "GraphicsCaptureSession::IsCursorCaptureEnabled (0x%08X)", winrt::to_hresult().value);
	}

	return success;
}

extern "C" EXPORT enum gs_color_space winrt_capture_get_color_space(const struct winrt_capture *capture)
{
	capture_graphics_scope graphics(capture->free_threaded);
	const_cast<winrt_capture *>(capture)->poll_frame();
	return (capture->format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? GS_CS_709_EXTENDED : GS_CS_SRGB;
}

extern "C" EXPORT void winrt_capture_render(struct winrt_capture *capture)
{
	capture_graphics_scope graphics(capture->free_threaded);
	capture->poll_frame();
	if (capture->texture_written) {
		const char *tech_name = "Draw";
		float multiplier = 1.f;
		const gs_color_space current_space = gs_get_color_space();
		if (capture->format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawMultiplyTonemap";
				multiplier = 80.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_EXTENDED:
				tech_name = "DrawMultiply";
				multiplier = 80.f / obs_get_video_sdr_white_level();
			}
		} else if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.f;
		}

		gs_effect_t *const effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_technique_t *tech = gs_effect_get_technique(effect, tech_name);

		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		gs_texture_t *const texture = capture->texture;
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);

		const size_t passes = gs_technique_begin(tech);
		for (size_t i = 0; i < passes; i++) {
			if (gs_technique_begin_pass(tech, i)) {
				gs_draw_sprite(texture, 0, 0, 0);

				gs_technique_end_pass(tech);
			}
		}
		gs_technique_end(tech);

		gs_blend_state_pop();

		gs_enable_framebuffer_srgb(previous);
	}
}

extern "C" EXPORT uint32_t winrt_capture_width(const struct winrt_capture *capture)
{
	capture_graphics_scope graphics(capture && capture->free_threaded);
	if (capture)
		const_cast<winrt_capture *>(capture)->poll_frame();
	return capture ? capture->texture_width : 0;
}

extern "C" EXPORT uint32_t winrt_capture_height(const struct winrt_capture *capture)
{
	capture_graphics_scope graphics(capture && capture->free_threaded);
	if (capture)
		const_cast<winrt_capture *>(capture)->poll_frame();
	return capture ? capture->texture_height : 0;
}

extern "C" EXPORT void winrt_capture_thread_start()
{
	struct winrt_capture *capture = capture_list;
	void *const device = gs_get_device_obj();
	while (capture) {
		winrt_capture_device_loss_rebuild(device, capture);
		capture = capture->next;
	}
}

extern "C" EXPORT void winrt_capture_thread_stop()
{
	struct winrt_capture *capture = capture_list;
	while (capture) {
		winrt_capture_device_loss_release(capture);
		capture = capture->next;
	}
}
