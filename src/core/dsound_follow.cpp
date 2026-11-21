#include "dsound_follow.hpp"

#include <windows.h>

#include <mmdeviceapi.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace vanacore::dsfollow {
namespace {

CreateFunction g_create = nullptr;
LogFunction g_log = nullptr;

// Every call the game makes on a wrapper takes this, and a move of the devices holds it all along: no call runs while
// the buffers are made again. It is never held while the game's own code runs.
std::mutex g_lock;

std::atomic<int> g_rebuilds{0};
std::atomic<int> g_last_moved{0};

void say(const wchar_t* format, ...)
{
    if (g_log == nullptr)
        return;
    wchar_t text[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    g_log(text);
}

// What a call returned that isn't an everyday result, each kind once: how a device that has gone away answers is what
// decides whether a move is needed, and the log is where it can be seen.
void note_failure(const char* method, HRESULT hr)
{
    static std::set<std::pair<std::string, HRESULT>> seen;
    if (SUCCEEDED(hr) || seen.size() >= 24 || !seen.insert({method, hr}).second)
        return;
    say(L"Sound: %hs returned 0x%08X", method, static_cast<unsigned>(hr));
}

// Calls on a device that has gone away can fault inside the driver; a move must get past them.
void safe_release(IUnknown* object)
{
    if (object == nullptr)
        return;
    __try {
        object->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void safe_stop(IDirectSoundBuffer* buffer)
{
    if (buffer == nullptr)
        return;
    __try {
        buffer->Stop();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

HRESULT safe_position(IDirectSoundBuffer* buffer, DWORD* play)
{
    __try {
        return buffer->GetCurrentPosition(play, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

HRESULT safe_status(IDirectSoundBuffer* buffer, DWORD* status)
{
    __try {
        return buffer->GetStatus(status);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

// A result worth remembering as what the game asked for: it worked, or the device (not the call) was what failed.
bool tracked(HRESULT hr)
{
    return SUCCEEDED(hr) || hr == DSERR_BUFFERLOST || hr == DSERR_NODRIVER || hr == DSERR_UNINITIALIZED;
}

class Device;

// A sound buffer as the game sees it. `real_` is the DirectSound buffer on the current device; a move swaps it.
// IDirectSoundBuffer and IDirectSoundBuffer8 are this one object (the second only adds three methods at the end of the
// first's table).
class Buffer final : public IDirectSoundBuffer8 {
public:
    Buffer(Device* device, IDirectSoundBuffer* real, const DSBUFFERDESC& desc);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* object) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (object == nullptr)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectSoundBuffer) ||
            IsEqualIID(riid, IID_IDirectSoundBuffer8)) {
            ++refs_;
            *object = static_cast<IDirectSoundBuffer8*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetCaps(LPDSBCAPS caps) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->GetCaps(caps);
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD play, LPDWORD write) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->GetCurrentPosition(play, write);
        if (SUCCEEDED(hr) && play != nullptr) {
            play_ = *play;
            play_tick_ = GetTickCount();
        } else
            note_failure("GetCurrentPosition", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX format, DWORD size, LPDWORD written) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->GetFormat(format, size, written);
    }
    HRESULT STDMETHODCALLTYPE GetVolume(LPLONG volume) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->GetVolume(volume);
    }
    HRESULT STDMETHODCALLTYPE GetPan(LPLONG pan) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->GetPan(pan);
    }
    HRESULT STDMETHODCALLTYPE GetFrequency(LPDWORD frequency) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->GetFrequency(frequency);
    }
    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD status) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->GetStatus(status);
        if (SUCCEEDED(hr) && status != nullptr) {
            // A sound that played to its end isn't started again by a move. A looping one only stops when the game says so:
            // a dead device may report it stopped
            if ((play_flags_ & DSBPLAY_LOOPING) == 0)
                playing_ = (*status & DSBSTATUS_PLAYING) != 0;
        } else
            note_failure("GetStatus", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTSOUND direct_sound, LPCDSBUFFERDESC desc) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->Initialize(direct_sound, desc);
    }
    HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, LPVOID* part1, LPDWORD bytes1, LPVOID* part2,
                                   LPDWORD bytes2, DWORD flags) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        DWORD start = offset;
        if (flags & DSBLOCK_FROMWRITECURSOR) {
            DWORD write = 0;
            start = SUCCEEDED(real_->GetCurrentPosition(nullptr, &write)) ? write : 0;
        }
        const HRESULT hr = real_->Lock(offset, bytes, part1, bytes1, part2, bytes2, flags);
        if (SUCCEEDED(hr)) {
            locked_ = true;
            lock_start_ = (flags & DSBLOCK_ENTIREBUFFER) ? 0 : start;
        } else
            note_failure("Lock", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Play(DWORD reserved, DWORD priority, DWORD flags) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->Play(reserved, priority, flags);
        if (tracked(hr)) {
            playing_ = true;
            priority_ = priority;
            play_flags_ = flags;
        }
        note_failure("Play", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD position) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->SetCurrentPosition(position);
        if (SUCCEEDED(hr)) {
            play_ = position;
            play_tick_ = GetTickCount();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetFormat(LPCWAVEFORMATEX format) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->SetFormat(format);
    }
    HRESULT STDMETHODCALLTYPE SetVolume(LONG volume) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->SetVolume(volume);
        if (tracked(hr)) {
            volume_ = volume;
            volume_set_ = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPan(LONG pan) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->SetPan(pan);
        if (tracked(hr)) {
            pan_ = pan;
            pan_set_ = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD frequency) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->SetFrequency(frequency);
        if (tracked(hr)) {
            frequency_ = frequency;
            frequency_set_ = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Stop() override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->Stop();
        if (tracked(hr))
            playing_ = false;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID part1, DWORD bytes1, LPVOID part2, DWORD bytes2) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        // What the game wrote, kept before the buffer takes it back
        if (part1 != nullptr && lock_start_ + bytes1 <= shadow_.size())
            std::memcpy(shadow_.data() + lock_start_, part1, bytes1);
        if (part2 != nullptr && bytes2 <= shadow_.size())
            std::memcpy(shadow_.data(), part2, bytes2);
        written_ = true;
        locked_ = false;
        const HRESULT hr = real_->Unlock(part1, bytes1, part2, bytes2);
        note_failure("Unlock", hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Restore() override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->Restore();
    }

    HRESULT STDMETHODCALLTYPE SetFX(DWORD count, LPDSEFFECTDESC effects, LPDWORD results) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        IDirectSoundBuffer8* real8 = nullptr;
        if (FAILED(real_->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&real8))))
            return E_NOINTERFACE;
        const HRESULT hr = real8->SetFX(count, effects, results);
        real8->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE AcquireResources(DWORD flags, DWORD count, LPDWORD results) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        IDirectSoundBuffer8* real8 = nullptr;
        if (FAILED(real_->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&real8))))
            return E_NOINTERFACE;
        const HRESULT hr = real8->AcquireResources(flags, count, results);
        real8->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetObjectInPath(REFGUID object, DWORD index, REFGUID iid, LPVOID* result) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        IDirectSoundBuffer8* real8 = nullptr;
        if (FAILED(real_->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&real8))))
            return E_NOINTERFACE;
        const HRESULT hr = real8->GetObjectInPath(object, index, iid, result);
        real8->Release();
        return hr;
    }

    // ---- A move to a new device (all under g_lock) ----

    // The same buffer on `device`, empty.
    IDirectSoundBuffer* make_on(IDirectSound8* device)
    {
        IDirectSoundBuffer* made = nullptr;
        const HRESULT hr = device->CreateSoundBuffer(&desc_, &made, nullptr);
        if (FAILED(hr)) {
            say(L"Sound: a buffer can't be made on the new device (0x%08X, %lu bytes, flags 0x%X)",
                static_cast<unsigned>(hr), desc_.dwBufferBytes, desc_.dwFlags);
            return nullptr;
        }
        return made;
    }

    // Puts what the game had in the buffer back into `made`: the contents, volume, pan, pitch and the play position; and
    // works out whether it was playing. It isn't started yet.
    void restore_into(IDirectSoundBuffer* made)
    {
        position_ = play_;
        resume_ = playing_;
        // A buffer the game is polling has its last position from the game's own call, which keeps the game's count of
        // what has played in step; one it isn't (a short sound) is asked
        DWORD play = 0, status = 0;
        const bool polled = GetTickCount() - play_tick_ < 500;
        if (!polled && SUCCEEDED(safe_position(real_, &play)))
            position_ = play;
        if ((play_flags_ & DSBPLAY_LOOPING) == 0 && SUCCEEDED(safe_status(real_, &status)))
            resume_ = (status & DSBSTATUS_PLAYING) != 0;

        if (written_) {
            LPVOID part1 = nullptr, part2 = nullptr;
            DWORD bytes1 = 0, bytes2 = 0;
            HRESULT hr = made->Lock(0, 0, &part1, &bytes1, &part2, &bytes2, DSBLOCK_ENTIREBUFFER);
            if (SUCCEEDED(hr)) {
                std::memcpy(part1, shadow_.data(), std::min<size_t>(bytes1, shadow_.size()));
                if (part2 != nullptr && bytes1 + bytes2 <= shadow_.size())
                    std::memcpy(part2, shadow_.data() + bytes1, bytes2);
                made->Unlock(part1, bytes1, part2, bytes2);
            } else
                say(L"Sound: a buffer's contents can't be put back (0x%08X)", static_cast<unsigned>(hr));
        }
        if (volume_set_)
            made->SetVolume(volume_);
        if (pan_set_)
            made->SetPan(pan_);
        if (frequency_set_)
            made->SetFrequency(frequency_);
        made->SetCurrentPosition(position_);
    }

    // The new buffer takes over: the old one stopped, the new one started if the old was playing. Returns the old one.
    IDirectSoundBuffer* switch_to(IDirectSoundBuffer* made)
    {
        IDirectSoundBuffer* old = real_;
        safe_stop(old);
        if (resume_)
            made->Play(0, priority_, play_flags_);
        real_ = made;
        return old;
    }

    bool locked() const { return locked_; }
    IDirectSoundBuffer* real() const { return real_; }

private:
    friend class Device;

    LONG refs_ = 1;
    Device* device_;
    IDirectSoundBuffer* real_;

    // What the game made it with (the format copied: the game frees its own)
    DSBUFFERDESC desc_ = {};
    std::vector<BYTE> format_;

    // What the game has done to it
    std::vector<BYTE> shadow_; // the contents, as written through Unlock
    bool written_ = false;
    bool locked_ = false;
    DWORD lock_start_ = 0;
    DWORD play_ = 0; // the play cursor as the game last saw it
    DWORD play_tick_ = 0;
    bool playing_ = false;
    DWORD priority_ = 0;
    DWORD play_flags_ = 0;
    LONG volume_ = 0, pan_ = 0;
    DWORD frequency_ = 0;
    bool volume_set_ = false, pan_set_ = false, frequency_set_ = false;

    // During a move
    DWORD position_ = 0;
    bool resume_ = false;
};

std::vector<Device*> g_devices; // under g_lock

class Device final : public IDirectSound8 {
public:
    explicit Device(IDirectSound8* real) : real_(real) { g_devices.push_back(this); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* object) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (object == nullptr)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectSound) || IsEqualIID(riid, IID_IDirectSound8)) {
            ++refs_;
            *object = static_cast<IDirectSound8*>(this);
            return S_OK;
        }
        return real_->QueryInterface(riid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return release_locked();
    }

    HRESULT STDMETHODCALLTYPE CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        // A primary buffer, or one with effects or 3D, isn't something a move could carry over: handed back unwrapped
        const bool wrap = desc != nullptr && out != nullptr && outer == nullptr && desc->dwSize >= sizeof(DSBUFFERDESC1) &&
                          desc->lpwfxFormat != nullptr && desc->dwBufferBytes != 0 &&
                          (desc->dwFlags & (DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLFX | DSBCAPS_CTRL3D)) == 0;
        if (!wrap)
            return real_->CreateSoundBuffer(desc, out, outer);
        IDirectSoundBuffer* real = nullptr;
        const HRESULT hr = real_->CreateSoundBuffer(desc, &real, nullptr);
        if (FAILED(hr)) {
            note_failure("CreateSoundBuffer", hr);
            return hr;
        }
        auto* buffer = new Buffer(this, real, *desc);
        buffers_.push_back(buffer);
        *out = buffer;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS caps) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->GetCaps(caps);
    }
    HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER* duplicate) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        // The duplicate is a plain DirectSound buffer, which a move leaves behind
        IDirectSoundBuffer* source = original;
        for (Buffer* buffer : buffers_)
            if (static_cast<IDirectSoundBuffer*>(static_cast<IDirectSoundBuffer8*>(buffer)) == original)
                source = buffer->real();
        return real_->DuplicateSoundBuffer(source, duplicate);
    }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD level) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->SetCooperativeLevel(window, level);
        if (SUCCEEDED(hr)) {
            window_ = window;
            level_ = level;
            level_set_ = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Compact() override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->Compact();
    }
    HRESULT STDMETHODCALLTYPE GetSpeakerConfig(LPDWORD config) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->GetSpeakerConfig(config);
    }
    HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD config) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const HRESULT hr = real_->SetSpeakerConfig(config);
        if (SUCCEEDED(hr)) {
            speakers_ = config;
            speakers_set_ = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPCGUID device) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->Initialize(device);
    }
    HRESULT STDMETHODCALLTYPE VerifyCertification(LPDWORD certified) override
    {
        std::lock_guard<std::mutex> hold(g_lock);
        return real_->VerifyCertification(certified);
    }

    ULONG release_locked()
    {
        const ULONG left = --refs_;
        if (left == 0) {
            g_devices.erase(std::remove(g_devices.begin(), g_devices.end(), this), g_devices.end());
            safe_release(real_);
            delete this;
        }
        return left;
    }

    // All the device's buffers made again on `fresh`, the default output now; false (nothing changed) if one can't be.
    bool move_to(IDirectSound8* fresh)
    {
        if (level_set_ && FAILED(fresh->SetCooperativeLevel(window_, level_)))
            say(L"Sound: the new device won't take the game's cooperative level");
        if (speakers_set_)
            fresh->SetSpeakerConfig(speakers_);

        std::vector<IDirectSoundBuffer*> made;
        for (Buffer* buffer : buffers_) {
            IDirectSoundBuffer* one = buffer->make_on(fresh);
            if (one == nullptr) {
                for (IDirectSoundBuffer* each : made)
                    safe_release(each);
                return false;
            }
            made.push_back(one);
        }
        for (size_t i = 0; i < buffers_.size(); ++i)
            buffers_[i]->restore_into(made[i]);
        std::vector<IDirectSoundBuffer*> old;
        for (size_t i = 0; i < buffers_.size(); ++i)
            old.push_back(buffers_[i]->switch_to(made[i]));
        IDirectSound8* old_device = real_;
        real_ = fresh;
        for (IDirectSoundBuffer* each : old)
            safe_release(each);
        safe_release(old_device);
        return true;
    }

    std::vector<Buffer*> buffers_;
    LONG refs_ = 1;

private:
    IDirectSound8* real_;
    HWND window_ = nullptr;
    DWORD level_ = 0;
    bool level_set_ = false;
    DWORD speakers_ = 0;
    bool speakers_set_ = false;
};

Buffer::Buffer(Device* device, IDirectSoundBuffer* real, const DSBUFFERDESC& desc) : device_(device), real_(real)
{
    ++device->refs_; // a buffer keeps its device alive, as DirectSound's do
    std::memcpy(&desc_, &desc, std::min<size_t>(desc.dwSize, sizeof(desc_)));
    const WAVEFORMATEX* format = desc.lpwfxFormat;
    const size_t size = format->wFormatTag == WAVE_FORMAT_PCM ? sizeof(PCMWAVEFORMAT) : sizeof(WAVEFORMATEX) + format->cbSize;
    format_.assign(std::max(size, sizeof(WAVEFORMATEX)), 0);
    std::memcpy(format_.data(), format, size);
    desc_.lpwfxFormat = reinterpret_cast<WAVEFORMATEX*>(format_.data());
    shadow_.assign(desc.dwBufferBytes, 0);
}

ULONG STDMETHODCALLTYPE Buffer::Release()
{
    std::lock_guard<std::mutex> hold(g_lock);
    const ULONG left = --refs_;
    if (left == 0) {
        Device* device = device_;
        device->buffers_.erase(std::remove(device->buffers_.begin(), device->buffers_.end(), this),
                               device->buffers_.end());
        safe_release(real_);
        delete this;
        device->release_locked();
    }
    return left;
}

// ---- Moving the devices ----------------------------------------------------------------------------------------

bool move_devices()
{
    for (int attempt = 0;; ++attempt) {
        std::unique_lock<std::mutex> hold(g_lock);
        if (g_devices.empty())
            return true;
        bool locked = false;
        for (Device* device : g_devices)
            for (Buffer* buffer : device->buffers_)
                locked = locked || buffer->locked();
        if (!locked) {
            const DWORD start = GetTickCount();
            int moved = 0;
            for (Device* device : g_devices) {
                IDirectSound8* fresh = nullptr;
                const HRESULT hr = g_create(nullptr, &fresh, nullptr);
                if (FAILED(hr)) {
                    say(L"Sound: no device on the new output (0x%08X)", static_cast<unsigned>(hr));
                    return false;
                }
                const size_t count = device->buffers_.size();
                if (!device->move_to(fresh)) {
                    safe_release(fresh);
                    return false;
                }
                moved += static_cast<int>(count);
            }
            g_last_moved = moved;
            ++g_rebuilds;
            say(L"Sound: %d buffers moved to the new output in %lu ms", moved, GetTickCount() - start);
            return true;
        }
        hold.unlock();
        // The game is between a Lock and its Unlock, a moment
        if (attempt >= 40) {
            say(L"Sound: a buffer stayed locked for a second, the move is tried again later");
            return false;
        }
        Sleep(25);
    }
}

// ---- Watching the default output --------------------------------------------------------------------------------

HANDLE g_wake = nullptr;  // something about the outputs changed
HANDLE g_ready = nullptr; // the watcher has started, or couldn't
std::atomic<bool> g_watching{false};
std::mutex g_events_lock;
std::vector<std::wstring> g_events; // devices that were added, removed or changed state

void note(const wchar_t* id)
{
    {
        std::lock_guard<std::mutex> hold(g_events_lock);
        g_events.emplace_back(id != nullptr ? id : L"");
    }
    SetEvent(g_wake);
}

// Windows tells of changes from threads of its own: all this does is leave a note for the watcher's thread.
class Notifier final : public IMMNotificationClient {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
            *object = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override
    {
        note(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override
    {
        note(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override
    {
        note(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eConsole)
            note(nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
Notifier g_notifier;

// The id of the default output, empty if there is none.
std::wstring default_output(IMMDeviceEnumerator* enumerator)
{
    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || device == nullptr)
        return L"";
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
        result = id;
        CoTaskMemFree(id);
    }
    device->Release();
    return result;
}

std::wstring output_name(IMMDeviceEnumerator* enumerator, const std::wstring& id)
{
    if (id.empty())
        return L"none";
    std::wstring name = L"?";
    IMMDevice* device = nullptr;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)) && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            name = value.pwszVal;
        PropVariantClear(&value);
    }
    if (store != nullptr)
        store->Release();
    if (device != nullptr)
        device->Release();
    return name;
}

DWORD WINAPI watch(void*)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr || FAILED(enumerator->RegisterEndpointNotificationCallback(&g_notifier))) {
        say(L"Sound: Windows' audio device list can't be watched (0x%08X), the game keeps the output it started on",
            static_cast<unsigned>(hr));
        SetEvent(g_ready);
        return 0;
    }
    std::wstring bound = default_output(enumerator); // the output the game's devices are on
    say(L"Sound: following the default output (now %ls)", output_name(enumerator, bound).c_str());
    g_watching = true;
    SetEvent(g_ready);

    bool retry = false;
    int tries = 0;
    for (;;) {
        const DWORD waited = WaitForSingleObject(g_wake, retry ? 3000 : INFINITE);
        if (waited == WAIT_OBJECT_0) { // an output that's coming or going takes a moment to finish
            tries = 0;
            while (WaitForSingleObject(g_wake, 1500) == WAIT_OBJECT_0) {
            }
        }
        std::vector<std::wstring> events;
        {
            std::lock_guard<std::mutex> hold(g_events_lock);
            events.swap(g_events);
        }
        const std::wstring now = default_output(enumerator);
        if (now.empty()) { // nothing to play on: the next change brings one
            retry = false;
            continue;
        }
        // The output in use changing state or coming back (a headset unplugged, a driver reset) leaves the game's device
        // dead even when the default is the same output again
        const bool touched = std::find(events.begin(), events.end(), bound) != events.end();
        if (!retry && now == bound && !touched)
            continue;
        say(L"Sound: %ls -> %ls (%ls)", output_name(enumerator, bound).c_str(), output_name(enumerator, now).c_str(),
            now != bound ? L"default output changed" : L"output reset");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); // the game's sound threads wait on it
        const bool moved = move_devices();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        if (moved) {
            bound = now;
            retry = false;
        } else if (++tries < 6) {
            retry = true;
        } else {
            retry = false;
            say(L"Sound: giving up on the new output until the outputs change again");
        }
    }
}

bool ensure_watching()
{
    static std::once_flag once;
    std::call_once(once, [] {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_wake == nullptr || g_ready == nullptr)
            return;
        HANDLE thread = CreateThread(nullptr, 0, &watch, nullptr, 0, nullptr);
        if (thread == nullptr)
            return;
        CloseHandle(thread); // runs as long as the process does
        WaitForSingleObject(g_ready, 5000);
    });
    return g_watching.load();
}

} // namespace

void set_functions(CreateFunction create_function, LogFunction log)
{
    g_create = create_function;
    g_log = log;
}

HRESULT WINAPI create(LPCGUID device, LPDIRECTSOUND8* out, LPUNKNOWN outer)
{
    const bool follows = device == nullptr || IsEqualGUID(*device, GUID_NULL) ||
                         IsEqualGUID(*device, DSDEVID_DefaultPlayback) || IsEqualGUID(*device, DSDEVID_DefaultVoicePlayback);
    const HRESULT hr = g_create(device, out, outer);
    if (FAILED(hr) || out == nullptr || *out == nullptr || outer != nullptr || !follows || !ensure_watching())
        return hr;
    std::lock_guard<std::mutex> hold(g_lock);
    *out = new Device(*out);
    return hr;
}

bool rebuild_now()
{
    return move_devices();
}

int rebuilds()
{
    return g_rebuilds;
}

int last_moved()
{
    return g_last_moved;
}

} // namespace vanacore::dsfollow
