#pragma once
// ct/platform/texture_pool.h
// A fixed set of identical GPU textures handed out by index. Pool exhaustion is a counted drop, never an allocation.
// Thread model: Acquire/AddRef/SetReusePredicate/SetUserData on one thread (the producer, e.g. ct-capture); Release on
// any one other thread (the consumer, e.g. ct-encode). At/Count/Desc/InUse/UserData from any thread.
// Each texture has a hand-out count: Acquire hands out a free texture (count 0 -> 1), AddRef hands the same texture out
// again (a repeated frame), and every hand-out is returned with one Release.
// Lifetime: Init() creates every texture; Shutdown() (or the destructor) releases them. Re-Init() is allowed after
// Shutdown(). The reuse predicate lets P04 keep a texture out of circulation while an IMFMediaBuffer still holds it.

#include <ct/platform/d3d_device.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace ct::platform {

class TexturePool {
public:
    using ReusePredicate = bool (*)(void* user, uint32_t index);

    TexturePool() = default;
    ~TexturePool();
    TexturePool(const TexturePool&) = delete;
    TexturePool& operator=(const TexturePool&) = delete;

    // Creates `count` textures with `desc` (count 1..64); sets WKPDID_D3DDebugObjectName to "<debug_name>[i]".
    HRESULT Init(D3DDevice&, const D3D11_TEXTURE2D_DESC& desc, uint32_t count, const char* debug_name);
    void Shutdown();

    // A free texture (round robin, so the longest-released one comes first), or nullptr if none is free or the
    // reuse predicate refuses every free one.
    ID3D11Texture2D* Acquire(uint32_t& index);
    void Release(uint32_t index);
    // Hands `index` out once more without writing it (ScreenCapturer re-sends the previous frame this way). Works on a
    // texture that is in use or already released (its content is unchanged: only the producer writes textures).
    void AddRef(uint32_t index);
    void SetReusePredicate(ReusePredicate can_reuse, void* user);

    ID3D11Texture2D* At(uint32_t index) const;
    uint32_t Count() const { return static_cast<uint32_t>(m_textures.size()); }
    uint32_t InUse() const { return m_in_use_count.load(std::memory_order_acquire); }
    const D3D11_TEXTURE2D_DESC& Desc() const { return m_desc; }
    void* UserData(uint32_t index);
    void SetUserData(uint32_t index, void* user);

private:
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_textures;
    std::unique_ptr<std::atomic<uint32_t>[]> m_refs;   // hand-outs not yet released; 0 = free
    std::vector<void*> m_user;
    D3D11_TEXTURE2D_DESC m_desc{};
    std::atomic<uint32_t> m_in_use_count{0};
    uint32_t m_next = 0;
    ReusePredicate m_can_reuse = nullptr;
    void* m_can_reuse_user = nullptr;
};

} // namespace ct::platform
