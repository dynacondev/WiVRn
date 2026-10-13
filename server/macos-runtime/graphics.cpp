// Vulkan graphics requirements (v1 string queries + requirements, and
// the enable2 create-functions + requirements2). Extension lists are the
// MoltenVK truth: portability enumeration/subset for instance and device
// creation, metal surface for display-capable apps. v1 advertises
// XR_KHR_vulkan_enable, enable2 adds the create-functions; both are
// advertised (see instance.cpp) so DIY and runtime-created devices work.

#include "runtime.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{

wivrn_macos::Instance * lookup(XrInstance instance)
{
	if (instance == nullptr)
		return nullptr;
	wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
	std::lock_guard lock(runtime.mutex);
	if (runtime.instance != &instance->data)
		return nullptr;
	return &instance->data;
}

XrResult check_system(wivrn_macos::Instance * self, XrSystemId systemId)
{
	if (self == nullptr)
		return XR_ERROR_HANDLE_INVALID;
	if (systemId != self->system.id)
		return XR_ERROR_SYSTEM_INVALID;
	return XR_SUCCESS;
}

// Space-joined extension list, two-call idiom (0 capacity -> byte count
// including the trailing NUL position, matching the spec).
template <size_t N>
XrResult emit_extensions(const char * const (&names)[N], uint32_t capacity, uint32_t * count, char * buffer)
{
	if (count == nullptr)
		return XR_ERROR_VALIDATION_FAILURE;
	uint32_t bytes = 0;
	for (size_t i = 0; i < N; ++i)
		bytes += (uint32_t)std::strlen(names[i]) + 1;
	if (capacity == 0)
	{
		*count = bytes;
		return XR_SUCCESS;
	}
	if (buffer == nullptr)
		return XR_ERROR_VALIDATION_FAILURE;
	if (capacity < bytes)
		return XR_ERROR_SIZE_INSUFFICIENT;
	char * out = buffer;
	for (size_t i = 0; i < N; ++i)
	{
		size_t len = std::strlen(names[i]);
		std::memcpy(out, names[i], len);
		out += len;
		*out++ = ' ';
	}
	out[-1] = '\0';
	*count = bytes;
	return XR_SUCCESS;
}

void fill_requirements(XrGraphicsRequirementsVulkanKHR * requirements)
{
	// NOTE: these are XrVersion (OpenXR packing), not Vk versions.
	requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
	requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
}

} // namespace

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanInstanceExtensionsKHR(
	        XrInstance instance,
	        XrSystemId systemId,
	        uint32_t bufferCapacityInput,
	        uint32_t * bufferCountOutput,
	        char * buffer)
	{
		wivrn_macos::Instance * self = lookup(instance);
		XrResult res = check_system(self, systemId);
		if (res != XR_SUCCESS)
			return res;
		static constexpr const char * names[] = {
		        "VK_KHR_surface",
		        "VK_EXT_metal_surface",
		        "VK_KHR_portability_enumeration",
		};
		return emit_extensions(names, bufferCapacityInput, bufferCountOutput, buffer);
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanDeviceExtensionsKHR(
	        XrInstance instance,
	        XrSystemId systemId,
	        uint32_t bufferCapacityInput,
	        uint32_t * bufferCountOutput,
	        char * buffer)
	{
		wivrn_macos::Instance * self = lookup(instance);
		XrResult res = check_system(self, systemId);
		if (res != XR_SUCCESS)
			return res;
		static constexpr const char * names[] = {
		        "VK_KHR_portability_subset",
		};
		return emit_extensions(names, bufferCapacityInput, bufferCountOutput, buffer);
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanGraphicsRequirementsKHR(
	        XrInstance instance,
	        XrSystemId systemId,
	        XrGraphicsRequirementsVulkanKHR * graphicsRequirements)
	{
		wivrn_macos::Instance * self = lookup(instance);
		XrResult res = check_system(self, systemId);
		if (res != XR_SUCCESS)
			return res;
		if (graphicsRequirements == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (graphicsRequirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR)
			return XR_ERROR_VALIDATION_FAILURE;
		if (graphicsRequirements->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		fill_requirements(graphicsRequirements);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanGraphicsRequirements2KHR(
	        XrInstance instance,
	        XrSystemId systemId,
	        XrGraphicsRequirementsVulkan2KHR * graphicsRequirements)
	{
		// In 1.1 the v2 requirements struct is an alias of the v1 one.
		return wivrn_macos_GetVulkanGraphicsRequirementsKHR(
		        instance,
		        systemId,
		        reinterpret_cast<XrGraphicsRequirementsVulkanKHR *>(graphicsRequirements));
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateVulkanInstanceKHR(
	        XrInstance instance,
	        const XrVulkanInstanceCreateInfoKHR * createInfo,
	        VkInstance * vulkanInstance,
	        VkResult * vulkanResult)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || createInfo == nullptr || vulkanInstance == nullptr || vulkanResult == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		(void)createInfo->pfnGetInstanceProcAddr; // app-side dispatch; the runtime links libvulkan directly
		static const char * extensions[] = {
		        "VK_KHR_surface",
		        "VK_EXT_metal_surface",
		        "VK_KHR_portability_enumeration",
		};
		VkApplicationInfo appInfo{
		        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		        .pApplicationName = "WiVRn-macOS",
		        .applicationVersion = 1,
		        .pEngineName = "WiVRn-macOS",
		        .engineVersion = 1,
		        // 1.1 (not 1.0): makes get_physical_device_properties2 core
		        // so portability_subset devices validate clean (see above).
		        .apiVersion = VK_API_VERSION_1_1,
		};
		VkInstanceCreateInfo vkCreateInfo{
		        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
		        .pApplicationInfo = &appInfo,
		        .enabledLayerCount = 0,
		        .ppEnabledLayerNames = nullptr,
		        .enabledExtensionCount = 3,
		        .ppEnabledExtensionNames = extensions,
		};
		*vulkanResult = vkCreateInstance(&vkCreateInfo, createInfo->vulkanAllocator, vulkanInstance);
		if (*vulkanResult != VK_SUCCESS)
		{
			wivrn_macos::log("CreateVulkanInstance: vkCreateInstance -> %d", (int)*vulkanResult);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateVulkanDeviceKHR(
	        XrInstance instance,
	        const XrVulkanDeviceCreateInfoKHR * createInfo,
	        VkDevice * vulkanDevice,
	        VkResult * vulkanResult)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || createInfo == nullptr || vulkanDevice == nullptr || vulkanResult == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->vulkanPhysicalDevice == VK_NULL_HANDLE || createInfo->vulkanCreateInfo == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		// Append the portability subset to whatever the app requested. NOTE:
		// do NOT also append VK_KHR_get_physical_device_properties2: MoltenVK
		// does not offer it at device level (enabling it fails), and with a
		// 1.1+ instance it is core so the portability_subset dependency
		// (VUID-01387) is satisfied implicitly. Our created instances are 1.1
		// for exactly this reason; DIY apps must do the same.
		std::vector<const char *> extensions;
		const VkDeviceCreateInfo * appInfo = createInfo->vulkanCreateInfo;
		for (uint32_t i = 0; i < appInfo->enabledExtensionCount; ++i)
			extensions.push_back(appInfo->ppEnabledExtensionNames[i]);
		bool haveSubset = false;
		for (const char * name: extensions)
		{
			if (std::strcmp(name, "VK_KHR_portability_subset") == 0)
			{
				haveSubset = true;
				break;
			}
		}
		if (!haveSubset)
			extensions.push_back("VK_KHR_portability_subset");
		VkDeviceCreateInfo create = *appInfo;
		create.enabledExtensionCount = (uint32_t)extensions.size();
		create.ppEnabledExtensionNames = extensions.data();
		*vulkanResult =
		        vkCreateDevice(createInfo->vulkanPhysicalDevice, &create, createInfo->vulkanAllocator, vulkanDevice);
		if (*vulkanResult != VK_SUCCESS)
		{
			wivrn_macos::log("CreateVulkanDevice: vkCreateDevice -> %d", (int)*vulkanResult);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		return XR_SUCCESS;
	}

} // extern "C"
