#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include "dxgi-helpers.hpp"
#include "graphics-hook.h"

struct d3d11_data {
	ID3D11Device *device;           /* do not release */
	ID3D11DeviceContext *context;   /* do not release */
	ID3D11DeviceContext4 *context4; /* release on free */
	ID3D11Fence *fence = nullptr;
	HANDLE fence_handle = nullptr;
	uint64_t fence_value = 0;
	bool fence_active = false;
	bool fence_failed_logged = false;

	uint32_t cx;
	uint32_t cy;
	DXGI_FORMAT format;
	bool using_shtex;
	bool multisampled;

	union {
		/* shared texture */
		struct {
			struct shtex_data *shtex_info;
			ID3D11Texture2D *texture;
			HANDLE handle;
		};
		/* shared memory */
		struct {
			ID3D11Texture2D *copy_surfaces[NUM_BUFFERS];
			bool texture_ready[NUM_BUFFERS];
			bool texture_mapped[NUM_BUFFERS];
			uint32_t pitch;
			struct shmem_data *shmem_info;
			int cur_tex;
			int copy_wait;
		};
	};
};

static struct d3d11_data data = {};

static inline void set_hook_reserved_u64(size_t lo_idx, uint64_t value)
{
	global_hook_info->reserved[lo_idx] = (uint32_t)(value & 0xffffffff);
	global_hook_info->reserved[lo_idx + 1] = (uint32_t)(value >> 32);
}

static inline void clear_d3d11_fence_sync_info(void)
{
	global_hook_info->reserved[HOOK_INFO_RESERVED_FENCE_HANDLE_LO] = 0;
	global_hook_info->reserved[HOOK_INFO_RESERVED_FENCE_HANDLE_HI] = 0;
}

void d3d11_free(void)
{
	clear_d3d11_fence_sync_info();
	capture_free();

	if (data.fence_handle) {
		CloseHandle(data.fence_handle);
		data.fence_handle = nullptr;
	}

	if (data.fence) {
		data.fence->Release();
		data.fence = nullptr;
	}

	if (data.context4) {
		data.context4->Release();
		data.context4 = nullptr;
	}

	if (data.using_shtex) {
		if (data.texture)
			data.texture->Release();
	} else {
		for (size_t i = 0; i < NUM_BUFFERS; i++) {
			if (data.copy_surfaces[i]) {
				if (data.texture_mapped[i])
					data.context->Unmap(data.copy_surfaces[i], 0);
				data.copy_surfaces[i]->Release();
			}
		}
	}

	memset(&data, 0, sizeof(data));

	hlog("----------------- d3d11 capture freed ----------------");
}

static bool create_d3d11_stage_surface(ID3D11Texture2D **tex)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = data.cx;
	desc.Height = data.cy;
	desc.Format = data.format;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	hr = data.device->CreateTexture2D(&desc, nullptr, tex);
	if (FAILED(hr)) {
		hlog_hr("create_d3d11_stage_surface: failed to create texture", hr);
		return false;
	}

	return true;
}

static bool create_d3d11_tex(uint32_t cx, uint32_t cy, ID3D11Texture2D **tex, HANDLE *handle)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = cx;
	desc.Height = cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = apply_dxgi_format_typeless(data.format, global_hook_info->allow_srgb_alias);
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	hr = data.device->CreateTexture2D(&desc, nullptr, tex);
	if (FAILED(hr)) {
		hlog_hr("create_d3d11_tex: failed to create texture", hr);
		return false;
	}

	if (!!handle) {
		IDXGIResource *dxgi_res;
		hr = (*tex)->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgi_res);
		if (FAILED(hr)) {
			hlog_hr("create_d3d11_tex: failed to query "
				"IDXGIResource interface from texture",
				hr);
			return false;
		}

		hr = dxgi_res->GetSharedHandle(handle);
		dxgi_res->Release();
		if (FAILED(hr)) {
			hlog_hr("create_d3d11_tex: failed to get shared handle", hr);
			return false;
		}
	}

	return true;
}

static inline bool d3d11_init_format(IDXGISwapChain *swap, HWND &window)
{
	DXGI_SWAP_CHAIN_DESC desc;
	HRESULT hr;

	hr = swap->GetDesc(&desc);
	if (FAILED(hr)) {
		hlog_hr("d3d11_init_format: swap->GetDesc failed", hr);
		return false;
	}

	print_swap_desc(&desc);

	data.format = strip_dxgi_format_srgb(desc.BufferDesc.Format);
	data.multisampled = desc.SampleDesc.Count > 1;
	window = desc.OutputWindow;
	data.cx = desc.BufferDesc.Width;
	data.cy = desc.BufferDesc.Height;

	return true;
}

static bool d3d11_shmem_init_buffers(size_t idx)
{
	bool success;

	success = create_d3d11_stage_surface(&data.copy_surfaces[idx]);
	if (!success) {
		hlog("d3d11_shmem_init_buffers: failed to create copy surface");
		return false;
	}

	if (idx == 0) {
		D3D11_MAPPED_SUBRESOURCE map = {};
		HRESULT hr;

		hr = data.context->Map(data.copy_surfaces[idx], 0, D3D11_MAP_READ, 0, &map);
		if (FAILED(hr)) {
			hlog_hr("d3d11_shmem_init_buffers: failed to get "
				"pitch",
				hr);
			return false;
		}

		data.pitch = map.RowPitch;
		data.context->Unmap(data.copy_surfaces[idx], 0);
	}

	return true;
}

static bool d3d11_shmem_init(HWND window)
{
	data.using_shtex = false;

	for (size_t i = 0; i < NUM_BUFFERS; i++) {
		if (!d3d11_shmem_init_buffers(i)) {
			return false;
		}
	}
	if (!capture_init_shmem(&data.shmem_info, window, data.cx, data.cy, data.pitch, data.format, false)) {
		return false;
	}

	hlog("d3d11 memory capture successful");
	return true;
}

static void d3d11_publish_fence_sync_info(void)
{
	clear_d3d11_fence_sync_info();

	if (!data.fence_active || !data.fence)
		return;

	const DWORD desired_access = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
	HRESULT hr = data.fence->CreateSharedHandle(nullptr, desired_access, nullptr, &data.fence_handle);

	if (FAILED(hr)) {
		hlog_hr("d3d11_publish_fence_sync_info: failed to create shared fence handle", hr);
		data.fence_active = false;
		return;
	}

	DWORD obs_pid = global_hook_info->reserved[HOOK_INFO_RESERVED_OBS_PID];
	if (!obs_pid) {
		hlog("d3d11_publish_fence_sync_info: missing OBS pid; disabling fence sync");
		CloseHandle(data.fence_handle);
		data.fence_handle = nullptr;
		data.fence_active = false;
		clear_d3d11_fence_sync_info();
		return;
	}

	HANDLE obs_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, obs_pid);
	if (!obs_process) {
		hlog("d3d11_publish_fence_sync_info: failed to open OBS process for handle duplication: %lu",
		     GetLastError());
		CloseHandle(data.fence_handle);
		data.fence_handle = nullptr;
		data.fence_active = false;
		clear_d3d11_fence_sync_info();
		return;
	}

	HANDLE obs_fence_handle = nullptr;
	BOOL duplicated = DuplicateHandle(GetCurrentProcess(), data.fence_handle, obs_process, &obs_fence_handle, 0,
					 FALSE, DUPLICATE_SAME_ACCESS);
	CloseHandle(obs_process);

	if (!duplicated || !obs_fence_handle) {
		hlog("d3d11_publish_fence_sync_info: failed to duplicate fence handle to OBS process: %lu",
		     GetLastError());
		CloseHandle(data.fence_handle);
		data.fence_handle = nullptr;
		data.fence_active = false;
		clear_d3d11_fence_sync_info();
		return;
	}

	set_hook_reserved_u64(HOOK_INFO_RESERVED_FENCE_HANDLE_LO, (uint64_t)(uintptr_t)obs_fence_handle);
}

static bool d3d11_shtex_init(HWND window)
{
	bool success;

	data.using_shtex = true;

	success = create_d3d11_tex(data.cx, data.cy, &data.texture, &data.handle);

	if (!success) {
		hlog("d3d11_shtex_init: failed to create texture");
		return false;
	}
	if (!capture_init_shtex(&data.shtex_info, window, data.cx, data.cy, data.format, false,
				(uintptr_t)data.handle)) {
		return false;
	}

	d3d11_publish_fence_sync_info();

	hlog("d3d11 shared texture capture successful");
	return true;
}

static void d3d11_init(IDXGISwapChain *swap)
{
	HWND window;
	HRESULT hr;
	ID3D11Device5 *device5 = nullptr;

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&data.device);
	if (FAILED(hr)) {
		hlog_hr("d3d11_init: failed to get device from swap", hr);
		return;
	}

	hr = data.device->QueryInterface(__uuidof(ID3D11Device5), (void **)&device5);
	if (FAILED(hr))
		hlog("d3d11_init: ID3D11Device5 unavailable; fence sync disabled");

	data.device->GetImmediateContext(&data.context);

	if (device5 &&
	    SUCCEEDED(data.context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&data.context4))) {
		hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
					  reinterpret_cast<void **>(&data.fence));
		if (SUCCEEDED(hr)) {
			data.fence_value = 0;
			data.fence_active = true;
			hlog("d3d11_init: created fence (GPU-only)");
		} else {
			hlog_hr("d3d11_init: failed to create fence", hr);
		}
	}

	if (device5)
		device5->Release();
	data.device->Release();
	data.context->Release();

	if (!d3d11_init_format(swap, window)) {
		return;
	}

	const bool success = global_hook_info->force_shmem ? d3d11_shmem_init(window) : d3d11_shtex_init(window);
	if (!success)
		d3d11_free();
}

static inline void d3d11_copy_texture(ID3D11Resource *dst, ID3D11Resource *src)
{
	if (data.multisampled) {
		data.context->ResolveSubresource(dst, 0, src, 0, data.format);
	} else {
		data.context->CopyResource(dst, src);
	}
}

static inline void d3d11_shtex_capture(ID3D11Resource *backbuffer)
{
	if (!data.texture)
		return;

	d3d11_copy_texture(data.texture, backbuffer);

	if (!data.fence_active)
		return;

	if (!data.context4 || !data.fence) {
		data.fence_active = false;
		clear_d3d11_fence_sync_info();
		return;
	}

	HRESULT hr = data.context4->Signal(data.fence, ++data.fence_value);
	if (FAILED(hr)) {
		if (!data.fence_failed_logged) {
			hlog("d3d11_shtex_capture: failed to signal fence; disabling fence sync (hr: 0x%08lX, fence: 0x%p)", hr, data.fence);
			data.fence_failed_logged = true;
		}
		data.fence_active = false;
		clear_d3d11_fence_sync_info();
	}
}

static void d3d11_shmem_capture_copy(int i)
{
	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hr;

	if (data.texture_ready[i]) {
		data.texture_ready[i] = false;

		hr = data.context->Map(data.copy_surfaces[i], 0, D3D11_MAP_READ, 0, &map);
		if (SUCCEEDED(hr)) {
			data.texture_mapped[i] = true;
			shmem_copy_data(i, map.pData);
		}
	}
}

static inline void d3d11_shmem_capture(ID3D11Resource *backbuffer)
{
	int next_tex;

	next_tex = (data.cur_tex + 1) % NUM_BUFFERS;
	d3d11_shmem_capture_copy(next_tex);

	if (data.copy_wait < NUM_BUFFERS - 1) {
		data.copy_wait++;
	} else {
		if (shmem_texture_data_lock(data.cur_tex)) {
			data.context->Unmap(data.copy_surfaces[data.cur_tex], 0);
			data.texture_mapped[data.cur_tex] = false;
			shmem_texture_data_unlock(data.cur_tex);
		}

		d3d11_copy_texture(data.copy_surfaces[data.cur_tex], backbuffer);
		data.texture_ready[data.cur_tex] = true;
	}

	data.cur_tex = next_tex;
}

void d3d11_capture(void *swap_ptr, void *backbuffer_ptr)
{
	IDXGIResource *dxgi_backbuffer = (IDXGIResource *)backbuffer_ptr;
	IDXGISwapChain *swap = (IDXGISwapChain *)swap_ptr;

	HRESULT hr;
	if (capture_should_stop()) {
		d3d11_free();
	}
	if (capture_should_init()) {
		d3d11_init(swap);
	}
	if (data.handle != nullptr && capture_ready()) {
		ID3D11Resource *backbuffer;

		hr = dxgi_backbuffer->QueryInterface(__uuidof(ID3D11Resource), (void **)&backbuffer);
		if (FAILED(hr)) {
			hlog_hr("d3d11_shtex_capture: failed to get "
				"backbuffer",
				hr);
			return;
		}

		if (data.using_shtex)
			d3d11_shtex_capture(backbuffer);
		else
			d3d11_shmem_capture(backbuffer);

		backbuffer->Release();
	}
}
