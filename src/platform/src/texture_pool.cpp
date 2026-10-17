#define CT_LOG_CATEGORY "d3d"
#include <ct/platform/texture_pool.h>

#include <ct/core/assert.h>
#include <ct/core/error.h>
#include <ct/core/log.h>

#include <cstdio>

namespace ct::platform {

TexturePool::~TexturePool() { Shutdown(); }

HRESULT TexturePool::Init(D3DDevice& dev, const D3D11_TEXTURE2D_DESC& desc, uint32_t count, const char* debug_name) {
    if (!dev.Device() || count == 0 || count > 64) return E_INVALIDARG;
    Shutdown();
    m_desc = desc;
    m_textures.resize(count);
    m_user.assign(count, nullptr);
    m_refs = std::make_unique<std::atomic<uint32_t>[]>(count);
    for (uint32_t i = 0; i < count; ++i) {
        m_refs[i].store(0, std::memory_order_relaxed);
        const HRESULT hr = dev.Device()->CreateTexture2D(&desc, nullptr, m_textures[i].GetAddressOf());
        if (FAILED(hr)) {
            CT_LOG_ERROR("TexturePool %s: CreateTexture2D(%ux%u, format %d) failed: %s", debug_name ? debug_name : "",
                         desc.Width, desc.Height, static_cast<int>(desc.Format), HrToString(hr).c_str());
            Shutdown();
            return hr;
        }
        if (debug_name) {
            char name[96];
            const int n = std::snprintf(name, sizeof(name), "%s[%u]", debug_name, i);
            if (n > 0) m_textures[i]->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(n), name);
        }
    }
    m_in_use_count.store(0, std::memory_order_release);
    m_next = 0;
    return S_OK;
}

void TexturePool::Shutdown() {
    m_textures.clear();
    m_user.clear();
    m_refs.reset();
    m_in_use_count.store(0, std::memory_order_release);
    m_next = 0;
}

ID3D11Texture2D* TexturePool::Acquire(uint32_t& index) {
    const uint32_t n = Count();
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t i = (m_next + k) % n;
        if (m_refs[i].load(std::memory_order_acquire) != 0) continue;
        if (m_can_reuse && !m_can_reuse(m_can_reuse_user, i)) continue;
        // Only this (producer) thread raises a count from 0, so nothing can race us here.
        m_refs[i].store(1, std::memory_order_relaxed);
        m_in_use_count.fetch_add(1, std::memory_order_acq_rel);
        m_next = (i + 1) % n;
        index = i;
        return m_textures[i].Get();
    }
    return nullptr;
}

void TexturePool::Release(uint32_t index) {
    if (index >= Count()) {
        CT_ASSERT(index < Count());
        return;
    }
    uint32_t refs = m_refs[index].load(std::memory_order_acquire);
    do {
        if (refs == 0) {
            CT_ASSERT(refs != 0);   // released more often than handed out
            return;
        }
    } while (!m_refs[index].compare_exchange_weak(refs, refs - 1, std::memory_order_acq_rel));
    if (refs == 1) m_in_use_count.fetch_sub(1, std::memory_order_acq_rel);
}

void TexturePool::AddRef(uint32_t index) {
    if (index >= Count()) {
        CT_ASSERT(index < Count());
        return;
    }
    if (m_refs[index].fetch_add(1, std::memory_order_acq_rel) == 0) m_in_use_count.fetch_add(1, std::memory_order_acq_rel);
}

void TexturePool::SetReusePredicate(ReusePredicate can_reuse, void* user) {
    m_can_reuse = can_reuse;
    m_can_reuse_user = user;
}

ID3D11Texture2D* TexturePool::At(uint32_t index) const { return index < Count() ? m_textures[index].Get() : nullptr; }

void* TexturePool::UserData(uint32_t index) { return index < Count() ? m_user[index] : nullptr; }

void TexturePool::SetUserData(uint32_t index, void* user) {
    if (index < Count()) m_user[index] = user;
}

} // namespace ct::platform
