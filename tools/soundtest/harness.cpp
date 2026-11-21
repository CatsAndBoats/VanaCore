// Tests dsound_follow.cpp with real DirectSound, calling it the way FFXiMain.dll does (see README.md). Plays a tone through a
// wrapped buffer, changes the buffer's contents, volume and position, moves the device to the default output and checks the
// buffer carried all of it. With --flip it also switches Windows' default output to another device and back (this changes
// the machine's sound settings for a few seconds) and checks the move comes by itself.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmdeviceapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dsound_follow.hpp"

#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void log(const wchar_t* text)
{
    std::wprintf(L"  log: %ls\n", text);
}

constexpr DWORD kBytes = 0x30000;
constexpr int kFrames = kBytes / 4;

// A tone with `period` frames to a cycle; kFrames is a whole number of cycles for 96 and 48.
void tone(std::vector<short>& samples, int period, int from_frame, int frames)
{
    for (int i = 0; i < frames; ++i) {
        const short value = static_cast<short>(8000 * std::sin(6.2831853 * ((from_frame + i) % period) / period));
        samples[(from_frame + i) * 2] = value;
        samples[(from_frame + i) * 2 + 1] = value;
    }
}

bool write(IDirectSoundBuffer8* buffer, DWORD offset, const short* samples, DWORD bytes)
{
    LPVOID part1 = nullptr, part2 = nullptr;
    DWORD bytes1 = 0, bytes2 = 0;
    if (FAILED(buffer->Lock(offset, bytes, &part1, &bytes1, &part2, &bytes2, 0)))
        return false;
    std::memcpy(part1, samples, bytes1);
    if (part2 != nullptr)
        std::memcpy(part2, reinterpret_cast<const BYTE*>(samples) + bytes1, bytes2);
    return SUCCEEDED(buffer->Unlock(part1, bytes1, part2, bytes2));
}

bool read(IDirectSoundBuffer8* buffer, std::vector<short>& samples)
{
    LPVOID part1 = nullptr, part2 = nullptr;
    DWORD bytes1 = 0, bytes2 = 0;
    if (FAILED(buffer->Lock(0, 0, &part1, &bytes1, &part2, &bytes2, DSBLOCK_ENTIREBUFFER)))
        return false;
    samples.resize(bytes1 / 2);
    std::memcpy(samples.data(), part1, bytes1);
    return SUCCEEDED(buffer->Unlock(part1, bytes1, part2, bytes2));
}

// Windows' policy interface (no SDK header): the default output is set through it.
struct __declspec(uuid("F8679F50-850A-41CF-9C72-430F290290C8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class __declspec(uuid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9")) PolicyConfigClient;

bool set_default(const std::wstring& id)
{
    IPolicyConfig* policy = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                                reinterpret_cast<void**>(&policy))))
        return false;
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications})
        ok = SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), role)) && ok;
    policy->Release();
    return ok;
}

// The default output's id, and another active output's.
bool outputs(std::wstring& current, std::wstring& other)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return false;
    IMMDevice* device = nullptr;
    LPWSTR id = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && SUCCEEDED(device->GetId(&id))) {
        current = id;
        CoTaskMemFree(id);
        device->Release();
    }
    IMMDeviceCollection* all = nullptr;
    UINT count = 0;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) && SUCCEEDED(all->GetCount(&count)))
        for (UINT i = 0; i < count && other.empty(); ++i)
            if (SUCCEEDED(all->Item(i, &device)) && SUCCEEDED(device->GetId(&id))) {
                if (current != id)
                    other = id;
                CoTaskMemFree(id);
                device->Release();
            }
    if (all != nullptr)
        all->Release();
    enumerator->Release();
    return !current.empty();
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const bool flip = argc > 1 && std::wstring(argv[1]) == L"--flip";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    vanacore::dsfollow::set_functions(&DirectSoundCreate8, &log);
    IDirectSound8* ds = nullptr;
    check(SUCCEEDED(vanacore::dsfollow::create(nullptr, &ds, nullptr)), "DirectSoundCreate8 on the default output");
    if (ds == nullptr)
        return 2;
    HWND window = CreateWindowW(L"STATIC", L"soundtest", 0, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    check(SUCCEEDED(ds->SetCooperativeLevel(window, DSSCL_PRIORITY)), "SetCooperativeLevel(PRIORITY)");

    // As FFXiMain.dll makes its streaming buffer: 0x24-byte description, 0x30000 bytes, pan/volume/pitch, deferred, the
    // description and the format freed once the buffer exists
    IDirectSoundBuffer* plain = nullptr;
    IDirectSoundBuffer8* buffer = nullptr;
    {
        auto* format = new WAVEFORMATEX{};
        format->wFormatTag = WAVE_FORMAT_PCM;
        format->nChannels = 2;
        format->nSamplesPerSec = 44100;
        format->wBitsPerSample = 16;
        format->nBlockAlign = 4;
        format->nAvgBytesPerSec = 44100 * 4;
        auto* desc = new DSBUFFERDESC{};
        desc->dwSize = 0x24;
        desc->dwFlags = 0x500c0 | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLFREQUENCY;
        desc->dwBufferBytes = kBytes;
        desc->lpwfxFormat = format;
        check(SUCCEEDED(ds->CreateSoundBuffer(desc, &plain, nullptr)), "CreateSoundBuffer");
        delete desc;
        delete format;
    }
    if (plain == nullptr)
        return 2;
    check(SUCCEEDED(plain->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&buffer))),
          "QueryInterface(IDirectSoundBuffer8)");
    check(buffer != nullptr && static_cast<void*>(buffer) == static_cast<void*>(plain), "both interfaces are one object");

    std::vector<short> samples(kFrames * 2);
    tone(samples, 96, 0, kFrames);
    check(write(buffer, 0, samples.data(), kBytes), "Lock/Unlock: tone written");
    buffer->SetVolume(-4000);
    buffer->SetPan(-500);
    buffer->SetFrequency(44100);
    check(SUCCEEDED(buffer->Play(0, 0, DSBPLAY_LOOPING)), "Play(LOOPING)");
    Sleep(300);
    // The game's later writes: the first quarter a different tone
    tone(samples, 48, 0, kFrames / 4);
    check(write(buffer, 0, samples.data(), kBytes / 4), "Lock/Unlock: a quarter rewritten");

    DWORD before = 0;
    check(SUCCEEDED(buffer->GetCurrentPosition(&before, nullptr)), "GetCurrentPosition");
    std::printf("  position before: %lu\n", before);

    std::printf("\nMoving to the default output...\n");
    check(vanacore::dsfollow::rebuild_now(), "rebuild_now");
    check(vanacore::dsfollow::last_moved() == 1, "one buffer moved");

    DWORD status = 0, after = 0;
    LONG volume = 0, pan = 0;
    DWORD frequency = 0;
    buffer->GetStatus(&status);
    check((status & DSBSTATUS_PLAYING) && (status & DSBSTATUS_LOOPING), "still playing, looping");
    buffer->GetCurrentPosition(&after, nullptr);
    std::printf("  position after: %lu\n", after);
    const DWORD gone = (after + kBytes - before) % kBytes;
    check(gone < kBytes / 4, "play position carried (not jumped back or far on)");
    buffer->GetVolume(&volume);
    buffer->GetPan(&pan);
    buffer->GetFrequency(&frequency);
    check(volume == -4000 && pan == -500 && frequency == 44100, "volume, pan and pitch carried");
    std::vector<short> now;
    check(read(buffer, now) && now == samples, "contents carried, including the rewritten quarter");
    Sleep(300);
    DWORD later = 0;
    buffer->GetCurrentPosition(&later, nullptr);
    check((later + kBytes - after) % kBytes > 1000 && (later + kBytes - after) % kBytes < kBytes / 2,
          "the new buffer is playing (its position moves)");

    if (flip) {
        std::printf("\nSwitching Windows' default output and back...\n");
        std::wstring current, other;
        if (!outputs(current, other) || other.empty()) {
            check(false, "a second active output to switch to");
        } else {
            const int before_moves = vanacore::dsfollow::rebuilds();
            check(set_default(other), "default output switched");
            for (int i = 0; i < 100 && vanacore::dsfollow::rebuilds() == before_moves; ++i)
                Sleep(100);
            check(vanacore::dsfollow::rebuilds() > before_moves, "the buffers moved by themselves");
            buffer->GetStatus(&status);
            check(status & DSBSTATUS_PLAYING, "playing on the other output");
            Sleep(2000);
            const int middle = vanacore::dsfollow::rebuilds();
            check(set_default(current), "default output put back");
            for (int i = 0; i < 100 && vanacore::dsfollow::rebuilds() == middle; ++i)
                Sleep(100);
            check(vanacore::dsfollow::rebuilds() > middle, "the buffers moved back by themselves");
            buffer->GetStatus(&status);
            check(status & DSBSTATUS_PLAYING, "playing on the first output again");
            Sleep(1500);
        }
    }

    buffer->Stop();
    buffer->Release();
    plain->Release();
    ds->Release();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
