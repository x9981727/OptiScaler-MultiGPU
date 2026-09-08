// SPDX-License-Identifier: MIT
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include "sl.h"

namespace NativeBridgeCapture {
// These hooks are intentionally thin. All resource/camera pairing, transport,
// slot lifetime, and control-thread work stays in NativeBridgeCapture.cpp.
void OnConstants(const sl::Constants& values, uint32_t frame, uint64_t viewport) noexcept;
void OnTags(uint32_t frame, uint64_t viewport, const sl::ResourceTag* tags,
            uint32_t count, ID3D12GraphicsCommandList* gameCommandList) noexcept;
void OnPresentStart(uint32_t frame, ID3D12CommandQueue* gameQueue) noexcept;

// Diagnostic state for OptiScaler logging/UI. Never implies a frame was valid.
bool Enabled() noexcept;
bool Connected() noexcept;
uint64_t PublishedFrames() noexcept;
uint64_t RejectedFrames() noexcept;
}
