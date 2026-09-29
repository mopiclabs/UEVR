#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace utility {
// A game can reach the same GPU device through different COM objects (interposer/proxy layers such as
// frame generation or overlay wrappers), so resources can report a different ID3D12Device than the one
// the swapchain returns. Treat devices that are the same object, or that live on the same adapter, as one.
inline bool is_same_d3d12_device(ID3D12Device* a, ID3D12Device* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }

    if (a == b) {
        return true;
    }

    Microsoft::WRL::ComPtr<IUnknown> identity_a{};
    Microsoft::WRL::ComPtr<IUnknown> identity_b{};

    if (SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&identity_a))) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&identity_b))) &&
        identity_a != nullptr && identity_a.Get() == identity_b.Get())
    {
        return true;
    }

    const auto luid_a = a->GetAdapterLuid();
    const auto luid_b = b->GetAdapterLuid();

    return luid_a.LowPart == luid_b.LowPart && luid_a.HighPart == luid_b.HighPart;
}
}
