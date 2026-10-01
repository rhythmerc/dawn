// Copyright 2019 The Dawn & Tint Authors
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// VulkanBackend.cpp: contains the definition of symbols exported by VulkanBackend.h so that they
// can be compiled twice: once export (shared library), once not exported (static library)

#include <cstring>
#include <utility>

// Include vulkan_platform.h before VulkanBackend.h includes vulkan.h so that we use our version
// of the non-dispatchable handles.
#include "src/dawn/common/vulkan_platform.h"

// Must be after vulkan_platform
#include "dawn/native/VulkanBackend.h"
#include "src/dawn/native/vulkan/BackendVk.h"
#include "src/dawn/native/vulkan/DeviceVk.h"
#include "src/dawn/native/vulkan/PhysicalDeviceVk.h"
#include "src/dawn/native/vulkan/QueueVk.h"
#include "src/dawn/native/vulkan/SharedTextureMemoryVk.h"
#include "src/dawn/native/vulkan/TextureVk.h"

namespace dawn::native::vulkan {

namespace {
ExternalVulkanHooks gExternalHooks;
bool gHaveExternalHooks = false;
}  // namespace

void SetExternalVulkanHooks(const ExternalVulkanHooks* hooks) {
    gHaveExternalHooks = hooks != nullptr;
    gExternalHooks = hooks != nullptr ? *hooks : ExternalVulkanHooks{};
}

const ExternalVulkanHooks* GetExternalVulkanHooks() {
    return gHaveExternalHooks ? &gExternalHooks : nullptr;
}

DeviceVkHandles GetDeviceVkHandles(WGPUDevice device) {
    Device* backendDevice = ToBackend(FromAPI(device));
    DeviceVkHandles handles;
    handles.instance = backendDevice->GetVkInstance();
    handles.physicalDevice = ToBackend(backendDevice->GetPhysicalDevice())->GetVkPhysicalDevice();
    handles.device = backendDevice->GetVkDevice();
    handles.queueFamilyIndex = backendDevice->GetGraphicsQueueFamily();
    handles.queue = ToBackend(backendDevice->GetQueue())->GetVkQueue();
    handles.extraQueueCount = backendDevice->GetExtraQueueCount();
    handles.getInstanceProcAddr = backendDevice->fn.GetInstanceProcAddr;
    return handles;
}

VkQueue GetExtraQueue(WGPUDevice device, uint32_t index) {
    Device* backendDevice = ToBackend(FromAPI(device));
    if (index >= backendDevice->GetExtraQueueCount()) {
        return VK_NULL_HANDLE;
    }
    VkQueue queue = VK_NULL_HANDLE;
    backendDevice->fn.GetDeviceQueue(backendDevice->GetVkDevice(),
                                     backendDevice->GetGraphicsQueueFamily(), 1 + index, &queue);
    return queue;
}

WGPUSharedTextureMemory CreateSharedTextureMemoryFromVkImage(WGPUDevice device,
                                                             uint64_t image,
                                                             const VkImageCreateInfo* info,
                                                             const char* label) {
    Device* backendDevice = ToBackend(FromAPI(device));
    auto deviceGuard = backendDevice->GetGuard();
    VkImage vkImage;
    static_assert(sizeof(*vkImage) == sizeof(image));
    std::memcpy(&*vkImage, &image, sizeof(image));  // a pointer or a uint64_t, per platform
    Ref<SharedTextureMemory> memory;
    if (backendDevice->ConsumedError(
            SharedTextureMemory::CreateFromVkImage(backendDevice,
                                                   StringView(label != nullptr ? label : ""),
                                                   vkImage, info),
            &memory, "calling CreateSharedTextureMemoryFromVkImage")) {
        return nullptr;
    }
    Ref<SharedTextureMemoryBase> base = std::move(memory);
    return ToAPI(ReturnToAPI(std::move(base)));
}

VkInstance GetInstance(WGPUDevice device) {
    Device* backendDevice = ToBackend(FromAPI(device));
    return backendDevice->GetVkInstance();
}

VkDevice GetVkDevice(WGPUDevice device) {
    Device* backendDevice = ToBackend(FromAPI(device));
    return backendDevice->GetVkDevice();
}

DAWN_NATIVE_EXPORT PFN_vkVoidFunction GetInstanceProcAddr(WGPUDevice device, const char* pName) {
    Device* backendDevice = ToBackend(FromAPI(device));
    return (*backendDevice->fn.GetInstanceProcAddr)(backendDevice->GetVkInstance(), pName);
}

#if DAWN_PLATFORM_IS(LINUX)
ExternalImageDescriptorOpaqueFD::ExternalImageDescriptorOpaqueFD()
    : ExternalImageDescriptorFD(ExternalImageType::OpaqueFD) {}

ExternalImageDescriptorDmaBuf::ExternalImageDescriptorDmaBuf()
    : ExternalImageDescriptorFD(ExternalImageType::DmaBuf) {}

ExternalImageExportInfoOpaqueFD::ExternalImageExportInfoOpaqueFD()
    : ExternalImageExportInfoFD(ExternalImageType::OpaqueFD) {}

ExternalImageExportInfoDmaBuf::ExternalImageExportInfoDmaBuf()
    : ExternalImageExportInfoFD(ExternalImageType::DmaBuf) {}
#endif  // DAWN_PLATFORM_IS(LINUX)

#if DAWN_PLATFORM_IS(ANDROID)
ExternalImageDescriptorAHardwareBuffer::ExternalImageDescriptorAHardwareBuffer()
    : ExternalImageDescriptorVk(ExternalImageType::AHardwareBuffer) {}

ExternalImageExportInfoAHardwareBuffer::ExternalImageExportInfoAHardwareBuffer()
    : ExternalImageExportInfoFD(ExternalImageType::AHardwareBuffer) {}
#endif

WGPUTexture WrapVulkanImage(WGPUDevice device, const ExternalImageDescriptorVk* descriptor) {
    Device* backendDevice = ToBackend(FromAPI(device));
    auto deviceGuard = backendDevice->GetGuard();
    switch (descriptor->GetType()) {
#if DAWN_PLATFORM_IS(ANDROID)
        case ExternalImageType::AHardwareBuffer: {
            const ExternalImageDescriptorAHardwareBuffer* ahbDescriptor =
                static_cast<const ExternalImageDescriptorAHardwareBuffer*>(descriptor);
            Ref<TextureBase> texture = backendDevice->CreateTextureWrappingVulkanImage(
                ahbDescriptor, ahbDescriptor->handle, ahbDescriptor->waitFDs);
            return ToAPI(ReturnToAPI(std::move(texture)));
        }
#elif DAWN_PLATFORM_IS(LINUX)
        case ExternalImageType::OpaqueFD:
        case ExternalImageType::DmaBuf: {
            const ExternalImageDescriptorFD* fdDescriptor =
                static_cast<const ExternalImageDescriptorFD*>(descriptor);
            Ref<TextureBase> texture = backendDevice->CreateTextureWrappingVulkanImage(
                fdDescriptor, fdDescriptor->memoryFD, fdDescriptor->waitFDs);
            return ToAPI(ReturnToAPI(std::move(texture)));
        }
#endif  // DAWN_PLATFORM_IS(LINUX)

        default:
            return nullptr;
    }
}

bool ExportVulkanImage(WGPUTexture texture,
                       VkImageLayout desiredLayout,
                       ExternalImageExportInfoVk* info) {
    if (texture == nullptr) {
        return false;
    }
    Texture* backendTexture = ToBackend(FromAPI(texture));
    Device* device = ToBackend(backendTexture->GetDevice());
    auto deviceGuard = device->GetGuard();
#if DAWN_PLATFORM_IS(ANDROID) || DAWN_PLATFORM_IS(LINUX)
    switch (info->GetType()) {
        case ExternalImageType::AHardwareBuffer:
        case ExternalImageType::OpaqueFD:
        case ExternalImageType::DmaBuf: {
            ExternalImageExportInfoFD* fdInfo = static_cast<ExternalImageExportInfoFD*>(info);

            return device->SignalAndExportExternalTexture(backendTexture, desiredLayout, fdInfo,
                                                          &fdInfo->semaphoreHandles);
        }
        default:
            return false;
    }
#else
    return false;
#endif  // DAWN_PLATFORM_IS(LINUX)
}

bool ExportVulkanImage(WGPUTexture texture, ExternalImageExportInfoVk* info) {
    return ExportVulkanImage(texture, VK_IMAGE_LAYOUT_UNDEFINED, info);
}

}  // namespace dawn::native::vulkan
