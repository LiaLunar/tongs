#include "game_present.hpp"

#include "game_symbols.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace tongs {
namespace {

std::atomic<bool> g_hooked{false};
std::atomic<DWORD> g_present_thread{0};  // the thread that last presented (the main thread)

// One request at a time; the hook takes it on the main thread.
std::mutex g_capture;            // serializes CapturePresentedFrame
std::mutex g_mutex;              // guards the members below
std::condition_variable g_done;
std::atomic<bool> g_pending{false};  // read by the hook every frame without the lock
std::uint64_t g_request = 0;          // the id of the request in flight
bool g_answered = false;
std::string g_error;
FramePixels g_pixels;

template <typename T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
};

std::string HresultText(const char* step, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lX)", step, static_cast<unsigned long>(hr));
    return buf;
}

// Copies the back buffer into system memory: resolved first when it is
// multisampled, since GetRenderTargetData reads only plain surfaces.
std::string ReadBackBuffer(IDirect3DDevice9* device, FramePixels& out) {
    Ref<IDirect3DSurface9> back;
    HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back.p);
    if (FAILED(hr)) return HresultText("GetBackBuffer", hr);
    D3DSURFACE_DESC desc{};
    if (FAILED(hr = back.p->GetDesc(&desc))) return HresultText("back buffer GetDesc", hr);
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
        return "the back buffer format " + std::to_string(static_cast<int>(desc.Format)) + " is not 32-bit RGB";
    }
    IDirect3DSurface9* source = back.p;
    Ref<IDirect3DSurface9> resolved;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        hr = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                        &resolved.p, nullptr);
        if (FAILED(hr)) return HresultText("CreateRenderTarget", hr);
        if (FAILED(hr = device->StretchRect(back.p, nullptr, resolved.p, nullptr, D3DTEXF_NONE))) {
            return HresultText("StretchRect", hr);
        }
        source = resolved.p;
    }
    Ref<IDirect3DSurface9> copy;
    hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy.p,
                                             nullptr);
    if (FAILED(hr)) return HresultText("CreateOffscreenPlainSurface", hr);
    if (FAILED(hr = device->GetRenderTargetData(source, copy.p))) return HresultText("GetRenderTargetData", hr);
    D3DLOCKED_RECT locked{};
    if (FAILED(hr = copy.p->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return HresultText("LockRect", hr);
    out.width = static_cast<int>(desc.Width);
    out.height = static_cast<int>(desc.Height);
    const std::size_t row = static_cast<std::size_t>(desc.Width) * 4;
    out.bgra.resize(row * desc.Height);
    const auto* src = static_cast<const std::uint8_t*>(locked.pBits);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(out.bgra.data() + row * y, src + static_cast<std::size_t>(locked.Pitch) * y, row);
    }
    copy.p->UnlockRect();
    for (std::size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 0xFF;  // X8R8G8B8 leaves it undefined
    return {};
}

// Main thread, right before the game's Present: answers a waiting request.
void OnPresent(std::uintptr_t device) {
    g_present_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!g_pending.load(std::memory_order_acquire)) return;
    std::uint64_t request = 0;
    {
        std::lock_guard lock(g_mutex);
        if (!g_pending.load(std::memory_order_relaxed)) return;
        request = g_request;
    }
    FramePixels pixels;
    std::string error = device != 0 ? ReadBackBuffer(reinterpret_cast<IDirect3DDevice9*>(device), pixels)
                                    : std::string("no Direct3D device at the Present call");
    std::lock_guard lock(g_mutex);
    if (!g_pending.load(std::memory_order_relaxed) || g_request != request) return;  // the waiter gave up
    g_error = std::move(error);
    g_pixels = std::move(pixels);
    g_answered = true;
    g_pending.store(false, std::memory_order_release);
    g_done.notify_all();
}

}  // namespace

std::string HookPresent() {
    try {
        // The hook lives as long as the process.
        anvil::Host{}.mid_hook(sym::present_call, [](anvil::Cpu& cpu) { OnPresent(cpu.rcx); }).release();
    } catch (const anvil::Error& e) {
        return e.what();
    }
    g_hooked.store(true, std::memory_order_release);
    return {};
}

bool PresentHooked() { return g_hooked.load(std::memory_order_acquire); }

std::string CapturePresentedFrame(int timeout_ms, FramePixels& out) {
    if (!PresentHooked()) return "the Present hook is not installed";
    if (g_present_thread.load(std::memory_order_relaxed) == GetCurrentThreadId()) {
        return "the main thread cannot wait for its own Present";
    }
    std::lock_guard serial(g_capture);
    std::unique_lock lock(g_mutex);
    ++g_request;
    g_answered = false;
    g_error.clear();
    g_pixels = {};
    g_pending.store(true, std::memory_order_release);
    if (!g_done.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] { return g_answered; })) {
        g_pending.store(false, std::memory_order_release);
        return "the game presented no frame within " + std::to_string(timeout_ms) + " ms";
    }
    if (!g_error.empty()) return g_error;
    out = std::move(g_pixels);
    g_pixels = {};
    return {};
}

}  // namespace tongs
