#pragma once
// ct/audio/devices.h
// Audio endpoint enumeration (microphones and render endpoints) and default-device / removal notifications.
// Thread model: every method is thread-safe. Listener callbacks arrive on a COM notification thread; implementers
// only set flags / post (never block, never call back into AudioDevices from the callback).
// Lifetime: Init on a thread with COM initialized (MTA preferred); Shutdown (or the destructor) unregisters the
// notification client. Listeners must be removed (or Shutdown called) before they are destroyed.

#include <ct/core/sync.h>

#include <memory>
#include <string>
#include <vector>

typedef long HRESULT;   // same type as <winnt.h>; keeps <windows.h> out of this header

namespace ct::audio {

struct DeviceInfo {
    std::wstring id;
    std::string name;   // PKEY_Device_FriendlyName, UTF-8
    bool is_default = false;
};

namespace detail { class NotificationClient; }

class AudioDevices {   // owns IMMDeviceEnumerator; thread-safe
public:
    AudioDevices();
    ~AudioDevices();
    AudioDevices(const AudioDevices&) = delete;
    AudioDevices& operator=(const AudioDevices&) = delete;

    HRESULT Init();
    // Active endpoints only. capture = true lists microphones, false lists render endpoints.
    std::vector<DeviceInfo> List(bool capture);
    std::wstring DefaultId(bool capture);          // eConsole role; empty when there is no such endpoint
    std::string NameOf(const std::wstring& id);    // friendly name; empty when unknown

    struct Listener {
        virtual void OnDefaultDeviceChanged(bool capture, const std::wstring& new_id) = 0;   // empty id = none left
        virtual void OnDeviceRemoved(const std::wstring& id) = 0;   // also sent when an endpoint leaves ACTIVE
        virtual ~Listener() = default;
    };
    // The application's listener (one slot; nullptr clears it). IMMNotificationClient impl; only eConsole default
    // changes are forwarded. Capture objects use Add/RemoveListener so several streams and the app can listen at
    // once. SetListener(nullptr)/RemoveListener return only after any running callback into that listener finished.
    void SetListener(Listener* listener);
    void AddListener(Listener* listener);
    void RemoveListener(Listener* listener);
    void Shutdown();

private:
    friend class detail::NotificationClient;
    void DispatchDefaultChanged(bool capture, const std::wstring& id);
    void DispatchRemoved(const std::wstring& id);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    SrwLock m_lock;   // guards m_impl's enumerator pointer
    SrwLock m_listeners_lock;
    Listener* m_app_listener = nullptr;
    std::vector<Listener*> m_listeners;
};

} // namespace ct::audio
