// audio_device_wasapi.cpp — GEO-10: the Windows render device — WASAPI shared-mode, EVENT-driven, float32.
// The <windows.h> family is QUARANTINED in this TU (the win32_test_window precedent). The render thread is a
// dedicated OS thread (the correct primitive for a device callback — crd-jobs fibers are for work, not
// latency-critical device service). Xruns count event timeouts and fully-starved periods — the soak metric.
// On non-Windows hosts start() returns false (the Linux backend rides the cross-platform sweep with hardware
// to verify on — the interface is the contract).

#include <crd/audio/audio_realtime.hpp>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <thread>

namespace crd::audio
{

struct AudioDevice::Impl
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioRenderClient*  render     = nullptr;
    HANDLE               event      = nullptr;
    WAVEFORMATEX*        mix        = nullptr;
    std::thread          thread;
    std::atomic<bool>    running{false};
    std::atomic<crd::u32> xruns{0};
    std::atomic<crd::u64> frames_done{0};
    crd::u32             buffer_frames = 0;
    AudioRenderFn        callback      = nullptr;
    void*                user          = nullptr;
    bool                 com_owner     = false;

    static void run(Impl* impl); // the render service loop (a static member sees the private struct)

    void release() noexcept
    {
        if (render != nullptr) { render->Release(); render = nullptr; }
        if (client != nullptr) { client->Release(); client = nullptr; }
        if (mix != nullptr) { CoTaskMemFree(mix); mix = nullptr; }
        if (device != nullptr) { device->Release(); device = nullptr; }
        if (enumerator != nullptr) { enumerator->Release(); enumerator = nullptr; }
        if (event != nullptr) { CloseHandle(event); event = nullptr; }
        if (com_owner)
        {
            CoUninitialize();
            com_owner = false;
        }
    }
};

// the render service loop — waits the period event, fills the free space through the user callback
void AudioDevice::Impl::run(Impl* impl)
{
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the render thread has its own COM apartment
        const crd::u32 channels = impl->mix->nChannels;
        while (impl->running.load(std::memory_order_acquire))
        {
            const DWORD wait = WaitForSingleObject(impl->event, 2000);
            if (!impl->running.load(std::memory_order_acquire)) { break; }
            if (wait != WAIT_OBJECT_0)
            {
                impl->xruns.fetch_add(1, std::memory_order_relaxed); // the device stopped signalling
                continue;
            }
            UINT32 padding = 0;
            if (FAILED(impl->client->GetCurrentPadding(&padding))) { continue; }
            if (padding == 0 && impl->frames_done.load(std::memory_order_relaxed) > 0)
            {
                impl->xruns.fetch_add(1, std::memory_order_relaxed); // fully starved — a glitch happened
            }
            const UINT32 avail = impl->buffer_frames - padding;
            if (avail == 0) { continue; }
            BYTE* raw = nullptr;
            if (FAILED(impl->render->GetBuffer(avail, &raw))) { continue; }
            auto* out = reinterpret_cast<float*>(raw);
            for (UINT32 i = 0; i < avail * channels; ++i) { out[i] = 0.0F; }
            if (impl->callback != nullptr && channels >= 2)
            {
                impl->callback(impl->user, out, avail, impl->mix->nSamplesPerSec);
            }
            (void)impl->render->ReleaseBuffer(avail, 0);
            impl->frames_done.fetch_add(avail, std::memory_order_relaxed);
        }
        CoUninitialize();
}

AudioDevice::AudioDevice() : m_impl(new Impl) {}

AudioDevice::~AudioDevice()
{
    stop();
    m_impl->release();
    delete m_impl;
}

bool AudioDevice::start(AudioRenderFn fn, void* user)
{
    Impl& im = *m_impl;
    if (im.running.load(std::memory_order_relaxed)) { return false; }

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    im.com_owner     = SUCCEEDED(co) && co != S_FALSE ? true : SUCCEEDED(co);
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&im.enumerator))))
    {
        im.release();
        return false;
    }
    if (FAILED(im.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &im.device)))
    {
        im.release();
        return false; // no endpoint (headless CI) — the honest skip
    }
    if (FAILED(im.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&im.client))) ||
        FAILED(im.client->GetMixFormat(&im.mix)))
    {
        im.release();
        return false;
    }
    // shared-mode mix format on Win10+ is float32 (plain or extensible); anything else refuses honestly
    const bool is_float =
        im.mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (im.mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(im.mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    if (!is_float || im.mix->wBitsPerSample != 32 || im.mix->nChannels < 2)
    {
        im.release();
        return false;
    }
    constexpr REFERENCE_TIME k_buffer_100ns = 40 * 10000; // 40 ms — 2× safety over the shared period
    if (FAILED(im.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     k_buffer_100ns, 0, im.mix, nullptr)))
    {
        im.release();
        return false;
    }
    im.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (im.event == nullptr || FAILED(im.client->SetEventHandle(im.event)) ||
        FAILED(im.client->GetBufferSize(&im.buffer_frames)) ||
        FAILED(im.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&im.render))))
    {
        im.release();
        return false;
    }

    im.callback = fn;
    im.user     = user;
    im.running.store(true, std::memory_order_release);
    im.thread = std::thread(Impl::run, m_impl);
    if (FAILED(im.client->Start()))
    {
        im.running.store(false, std::memory_order_release);
        SetEvent(im.event);
        im.thread.join();
        im.release();
        return false;
    }
    return true;
}

void AudioDevice::stop()
{
    Impl& im = *m_impl;
    if (!im.running.load(std::memory_order_relaxed)) { return; }
    im.running.store(false, std::memory_order_release);
    if (im.event != nullptr) { SetEvent(im.event); }
    if (im.thread.joinable()) { im.thread.join(); }
    if (im.client != nullptr) { (void)im.client->Stop(); }
}

crd::u32 AudioDevice::sample_rate() const noexcept
{
    return m_impl->mix != nullptr ? m_impl->mix->nSamplesPerSec : 0;
}
crd::u32 AudioDevice::xrun_count() const noexcept { return m_impl->xruns.load(std::memory_order_relaxed); }
crd::u64 AudioDevice::frames_rendered() const noexcept
{
    return m_impl->frames_done.load(std::memory_order_relaxed);
}

} // namespace crd::audio

#else // !_WIN32

namespace crd::audio
{
struct AudioDevice::Impl
{
};
AudioDevice::AudioDevice() : m_impl(nullptr) {}
AudioDevice::~AudioDevice() = default;
bool AudioDevice::start(AudioRenderFn, void*) { return false; } // the Linux backend rides the platform sweep
void AudioDevice::stop() {}
crd::u32 AudioDevice::sample_rate() const noexcept { return 0; }
crd::u32 AudioDevice::xrun_count() const noexcept { return 0; }
crd::u64 AudioDevice::frames_rendered() const noexcept { return 0; }
} // namespace crd::audio

#endif
