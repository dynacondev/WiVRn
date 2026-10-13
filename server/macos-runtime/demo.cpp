// wivrn-macos-demo: minimal animated OpenXR app for the macOS server v1
// proof. Renders a cycling background + spinning colored cube through OUR
// runtime swapchains (loadOp-CLEAR, no depth, no culling, no
// ClearAttachments): every piece used here was verified pixel-by-pixel
// against hello_xr, which cannot render on this stack (its
// vkCmdClearAttachments yields driver fill X and its BACK+CW cubes are
// all culled — see ROADMAP run 8; both app-side on MoltenVK).
//
// Usage (via the launcher so XR_RUNTIME_JSON + ICD pin apply):
//   wivrn-server-macos -- wivrn-macos-demo
// Runs until killed; each frame advances the animation.

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#define CHECK(cond)                                                                          \
	do                                                                                   \
	{                                                                                    \
		if (!(cond))                                                                 \
		{                                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			std::exit(1);                                                        \
		}                                                                            \
	} while (0)
#define CHECK_XR(call, expected)                                                                                                             \
	do                                                                                                                                   \
	{                                                                                                                                    \
		XrResult res__ = (call);                                                                                                     \
		if (res__ != (expected))                                                                                                     \
		{                                                                                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s -> %d, expected %d\n", __FILE__, __LINE__, #call, (int)res__, (int)(expected)); \
			std::exit(1);                                                                                                        \
		}                                                                                                                            \
	} while (0)
#define CHECK_VK(call)                                                                                         \
	do                                                                                                     \
	{                                                                                                      \
		VkResult res__ = (call);                                                                       \
		if (res__ != VK_SUCCESS)                                                                       \
		{                                                                                              \
			std::fprintf(stderr, "FAIL %s:%d: %s -> %d\n", __FILE__, __LINE__, #call, (int)res__); \
			std::exit(1);                                                                          \
		}                                                                                              \
	} while (0)

namespace
{

struct VkDeviceSet
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
};

bool bring_up_vulkan(VkDeviceSet & dev)
{
	static const char * extensions[] = {
	        "VK_KHR_surface",
	        "VK_EXT_metal_surface",
	        "VK_KHR_portability_enumeration",
	};
	VkApplicationInfo appInfo{
	        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	        .pNext = nullptr,
	        .pApplicationName = "wivrn-macos-demo",
	        .applicationVersion = 1,
	        .pEngineName = nullptr,
	        .engineVersion = 0,
	        .apiVersion = VK_API_VERSION_1_1,
	};
	uint32_t extCount = 0;
	const char ** enabledExts = extensions;
	VkInstanceCreateInfo instanceInfo{
	        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
	        .pApplicationInfo = &appInfo,
	        .enabledLayerCount = 0,
	        .ppEnabledLayerNames = nullptr,
	        .enabledExtensionCount = 3,
	        .ppEnabledExtensionNames = enabledExts,
	};
	(void)extCount;
	if (vkCreateInstance(&instanceInfo, nullptr, &dev.instance) != VK_SUCCESS)
		return false;
	uint32_t physCount = 0;
	if (vkEnumeratePhysicalDevices(dev.instance, &physCount, nullptr) != VK_SUCCESS || physCount == 0)
		return false;
	std::vector<VkPhysicalDevice> phys(physCount);
	if (vkEnumeratePhysicalDevices(dev.instance, &physCount, phys.data()) != VK_SUCCESS)
		return false;
	dev.phys = phys[0];
	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &familyCount, families.data());
	for (uint32_t i = 0; i < familyCount; ++i)
	{
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			dev.family = i;
			break;
		}
	}
	static const char * deviceExtensions[] = {"VK_KHR_portability_subset"};
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .queueFamilyIndex = dev.family,
	        .queueCount = 1,
	        .pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .queueCreateInfoCount = 1,
	        .pQueueCreateInfos = &queueInfo,
	        .enabledExtensionCount = 1,
	        .ppEnabledExtensionNames = deviceExtensions,
	};
	if (vkCreateDevice(dev.phys, &deviceInfo, nullptr, &dev.device) != VK_SUCCESS)
		return false;
	vkGetDeviceQueue(dev.device, dev.family, 0, &dev.queue);
	return dev.queue != VK_NULL_HANDLE;
}

// Cube corners (±0.5) and per-face colors; 36 non-indexed verts (2 tris
// per face), positions + colors interleaved. CULL_NONE downstream, so
// winding is irrelevant here.
struct Vert
{
	float x, y, z, r, g, b;
};
// Cube vertex data follows below (explicit, no macros).
constexpr Vert cubeVerts[] = {
        // -X dark red
        {-0.5f, 0.5f, -0.5f, 0.25f, 0, 0},
        {-0.5f, -0.5f, 0.5f, 0.25f, 0, 0},
        {-0.5f, -0.5f, -0.5f, 0.25f, 0, 0},
        {-0.5f, 0.5f, -0.5f, 0.25f, 0, 0},
        {-0.5f, 0.5f, 0.5f, 0.25f, 0, 0},
        {-0.5f, -0.5f, 0.5f, 0.25f, 0, 0},
        // +X red
        {0.5f, 0.5f, -0.5f, 1, 0, 0},
        {0.5f, -0.5f, -0.5f, 1, 0, 0},
        {0.5f, -0.5f, 0.5f, 1, 0, 0},
        {0.5f, 0.5f, -0.5f, 1, 0, 0},
        {0.5f, -0.5f, 0.5f, 1, 0, 0},
        {0.5f, 0.5f, 0.5f, 1, 0, 0},
        // -Y dark green
        {-0.5f, -0.5f, -0.5f, 0, 0.25f, 0},
        {-0.5f, -0.5f, 0.5f, 0, 0.25f, 0},
        {0.5f, -0.5f, 0.5f, 0, 0.25f, 0},
        {-0.5f, -0.5f, -0.5f, 0, 0.25f, 0},
        {0.5f, -0.5f, 0.5f, 0, 0.25f, 0},
        {0.5f, -0.5f, -0.5f, 0, 0.25f, 0},
        // +Y green
        {-0.5f, 0.5f, -0.5f, 0, 1, 0},
        {0.5f, 0.5f, -0.5f, 0, 1, 0},
        {0.5f, 0.5f, 0.5f, 0, 1, 0},
        {-0.5f, 0.5f, -0.5f, 0, 1, 0},
        {0.5f, 0.5f, 0.5f, 0, 1, 0},
        {-0.5f, 0.5f, 0.5f, 0, 1, 0},
        // -Z dark blue
        {-0.5f, -0.5f, -0.5f, 0, 0, 0.25f},
        {0.5f, -0.5f, -0.5f, 0, 0, 0.25f},
        {0.5f, 0.5f, -0.5f, 0, 0, 0.25f},
        {-0.5f, -0.5f, -0.5f, 0, 0, 0.25f},
        {0.5f, 0.5f, -0.5f, 0, 0, 0.25f},
        {-0.5f, 0.5f, -0.5f, 0, 0, 0.25f},
        // +Z blue
        {-0.5f, -0.5f, 0.5f, 0, 0, 1},
        {-0.5f, 0.5f, 0.5f, 0, 0, 1},
        {0.5f, 0.5f, 0.5f, 0, 0, 1},
        {-0.5f, -0.5f, 0.5f, 0, 0, 1},
        {0.5f, 0.5f, 0.5f, 0, 0, 1},
        {0.5f, -0.5f, 0.5f, 0, 0, 1},
};

std::vector<uint32_t> readSpv(const char * path)
{
	FILE * f = std::fopen(path, "rb");
	if (f == nullptr)
		return {};
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<uint32_t> out((size_t)(n + 3) / 4);
	if (n > 0 && std::fread(out.data(), 1, (size_t)n, f) != (size_t)n)
		out.clear();
	std::fclose(f);
	return out;
}

} // namespace

int main(int argc, char ** argv)
{
	const char * manifest = std::getenv("XR_RUNTIME_JSON");
	if (manifest == nullptr)
	{
		std::puts("set XR_RUNTIME_JSON to the generated wivrn-macos.json (use wivrn-server-macos -- wivrn-macos-demo)");
		return 2;
	}
	std::filesystem::path exe = argv[0];
	std::filesystem::path shaderDir = exe.parent_path();
	auto vertPath = (shaderDir / "cubec.vert.spv").string();
	auto fragPath = (shaderDir / "cubec.frag.spv").string();

	// ---- OpenXR instance/session through the real loader ----
	const char * enabledExts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	XrInstance instance = XR_NULL_HANDLE;
	XrInstanceCreateInfo createInfo{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .next = nullptr,
	        .createFlags = 0,
	        .applicationInfo = {"wivrn-macos-demo", 1, "", 0, XR_CURRENT_API_VERSION},
	        .enabledApiLayerCount = 0,
	        .enabledApiLayerNames = nullptr,
	        .enabledExtensionCount = 2,
	        .enabledExtensionNames = enabledExts,
	};
	CHECK_XR(xrCreateInstance(&createInfo, &instance), XR_SUCCESS);

	VkDeviceSet dev{};
	CHECK(bring_up_vulkan(dev));

	XrSystemGetInfo sysInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .next = nullptr, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	CHECK_XR(xrGetSystem(instance, &sysInfo, &systemId), XR_SUCCESS);

	XrGraphicsBindingVulkan2KHR binding{
	        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
	        .next = nullptr,
	        .instance = dev.instance,
	        .physicalDevice = dev.phys,
	        .device = dev.device,
	        .queueFamilyIndex = dev.family,
	        .queueIndex = 0,
	};
	XrSession session = XR_NULL_HANDLE;
	XrSessionCreateInfo sessionInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .createFlags = 0, .systemId = systemId};
	CHECK_XR(xrCreateSession(instance, &sessionInfo, &session), XR_SUCCESS);

	XrViewConfigurationType config = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	XrSessionBeginInfo beginInfo{.type = XR_TYPE_SESSION_BEGIN_INFO, .next = nullptr, .primaryViewConfigurationType = config};
	CHECK_XR(xrBeginSession(session, &beginInfo), XR_SUCCESS);

	// ---- Swapchains (modest size: software x264 encodes ~30ms/eye at
	// full res; 1024px flows far faster for the motion proof) ----
	uint32_t formatCount = 0;
	CHECK_XR(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr), XR_SUCCESS);
	std::vector<int64_t> formats(formatCount);
	CHECK_XR(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()), XR_SUCCESS);
	int64_t format = formats[0];

	XrSwapchain swapchains[2] = {};
	for (int eye = 0; eye < 2; ++eye)
	{
		XrSwapchainCreateInfo swapInfo{
		        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		        .next = nullptr,
		        .createFlags = 0,
		        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
		        .format = format,
		        .sampleCount = 1,
		        .width = 1024,
		        .height = 1024,
		        .faceCount = 1,
		        .arraySize = 1,
		        .mipCount = 1,
		};
		CHECK_XR(xrCreateSwapchain(session, &swapInfo, &swapchains[eye]), XR_SUCCESS);
		uint32_t imageCount = 0;
		CHECK_XR(xrEnumerateSwapchainImages(swapchains[eye], 0, &imageCount, nullptr), XR_SUCCESS);
		CHECK(imageCount >= 2);
	}

	XrReferenceSpaceCreateInfo spaceInfo{
	        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	        .next = nullptr,
	        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
	        .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	};
	XrSpace local = XR_NULL_HANDLE;
	CHECK_XR(xrCreateReferenceSpace(session, &spaceInfo, &local), XR_SUCCESS);

	// ---- Render setup: LOAD pass + hardcoded-color cube, no depth, no
	// culling, no ClearAttachments (all verified paths; see file header) ----
	std::vector<uint32_t> vs = readSpv(vertPath.c_str());
	std::vector<uint32_t> fs = readSpv(fragPath.c_str());
	CHECK(!vs.empty() && vs[0] == 0x07230203);
	CHECK(!fs.empty() && fs[0] == 0x07230203);

	VkAttachmentDescription att{
	        .flags = 0,
	        .format = (VkFormat)format,
	        .samples = VK_SAMPLE_COUNT_1_BIT,
	        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
	        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub{
	        .flags = 0,
	        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	        .inputAttachmentCount = 0,
	        .pInputAttachments = nullptr,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &colorRef,
	        .pResolveAttachments = nullptr,
	        .pDepthStencilAttachment = nullptr,
	        .preserveAttachmentCount = 0,
	        .pPreserveAttachments = nullptr,
	};
	VkRenderPassCreateInfo rpInfo{
	        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .attachmentCount = 1,
	        .pAttachments = &att,
	        .subpassCount = 1,
	        .pSubpasses = &sub,
	        .dependencyCount = 0,
	        .pDependencies = nullptr,
	};
	VkRenderPass pass = VK_NULL_HANDLE;
	CHECK_VK(vkCreateRenderPass(dev.device, &rpInfo, nullptr, &pass));

	VkShaderModuleCreateInfo vsInfo{
	        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .codeSize = vs.size() * 4,
	        .pCode = vs.data(),
	};
	VkShaderModuleCreateInfo fsInfo = vsInfo;
	fsInfo.codeSize = fs.size() * 4;
	fsInfo.pCode = fs.data();
	VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
	CHECK_VK(vkCreateShaderModule(dev.device, &vsInfo, nullptr, &vsm));
	CHECK_VK(vkCreateShaderModule(dev.device, &fsInfo, nullptr, &fsm));
	VkPipelineShaderStageCreateInfo stages[2] = {
	        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", nullptr},
	        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main", nullptr},
	};
	VkVertexInputBindingDescription vbind{0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription vattr[2] = {
	        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
	        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
	};
	VkPipelineVertexInputStateCreateInfo vi{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .vertexBindingDescriptionCount = 1,
	        .pVertexBindingDescriptions = &vbind,
	        .vertexAttributeDescriptionCount = 2,
	        .pVertexAttributeDescriptions = vattr,
	};
	VkPipelineInputAssemblyStateCreateInfo ia{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	        .primitiveRestartEnable = VK_FALSE,
	};
	VkPipelineViewportStateCreateInfo vps{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .viewportCount = 1,
	        .pViewports = nullptr, // dynamic below
	        .scissorCount = 1,
	        .pScissors = nullptr, // dynamic below
	};
	VkPipelineRasterizationStateCreateInfo rs{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .depthClampEnable = VK_FALSE,
	        .rasterizerDiscardEnable = VK_FALSE,
	        .polygonMode = VK_POLYGON_MODE_FILL,
	        .cullMode = VK_CULL_MODE_NONE,
	        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	        .depthBiasEnable = VK_FALSE,
	        .lineWidth = 1,
	};
	VkPipelineMultisampleStateCreateInfo ms{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	        .sampleShadingEnable = VK_FALSE,
	};
	VkPipelineDepthStencilStateCreateInfo dss{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .depthTestEnable = VK_FALSE,
	        .depthWriteEnable = VK_FALSE,
	        .stencilTestEnable = VK_FALSE,
	};
	VkPipelineColorBlendAttachmentState ba{};
	ba.blendEnable = VK_FALSE;
	ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cb{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .logicOpEnable = VK_FALSE,
	        .attachmentCount = 1,
	        .pAttachments = &ba,
	};
	VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .dynamicStateCount = 2,
	        .pDynamicStates = dynStates,
	};
	VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
	VkPipelineLayoutCreateInfo layoutInfo{
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .setLayoutCount = 0,
	        .pSetLayouts = nullptr,
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &pcRange,
	};
	VkPipelineLayout layout = VK_NULL_HANDLE;
	CHECK_VK(vkCreatePipelineLayout(dev.device, &layoutInfo, nullptr, &layout));
	VkGraphicsPipelineCreateInfo pipeInfo{
	        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .stageCount = 2,
	        .pStages = stages,
	        .pVertexInputState = &vi,
	        .pInputAssemblyState = &ia,
	        .pTessellationState = nullptr,
	        .pViewportState = &vps,
	        .pRasterizationState = &rs,
	        .pMultisampleState = &ms,
	        .pDepthStencilState = &dss,
	        .pColorBlendState = &cb,
	        .pDynamicState = &dyn,
	        .layout = layout,
	        .renderPass = pass,
	        .subpass = 0,
	        .basePipelineHandle = VK_NULL_HANDLE,
	        .basePipelineIndex = -1,
	};
	VkPipeline pipe = VK_NULL_HANDLE;
	CHECK_VK(vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipe));
	vkDestroyShaderModule(dev.device, vsm, nullptr);
	vkDestroyShaderModule(dev.device, fsm, nullptr);

	VkBuffer vb = VK_NULL_HANDLE;
	VkDeviceMemory vbMem = VK_NULL_HANDLE;
	{
		VkBufferCreateInfo vbInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = 0,
		        .size = sizeof(cubeVerts),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		CHECK_VK(vkCreateBuffer(dev.device, &vbInfo, nullptr, &vb));
		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(dev.device, vb, &req);
		VkPhysicalDeviceMemoryProperties mem{};
		vkGetPhysicalDeviceMemoryProperties(dev.phys, &mem);
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
		{
			if ((req.memoryTypeBits & (1u << i)) == 0)
				continue;
			if ((mem.memoryTypes[i].propertyFlags &
			     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			{
				type = i;
				break;
			}
		}
		VkMemoryAllocateInfo allocInfo{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		        .pNext = nullptr,
		        .allocationSize = req.size,
		        .memoryTypeIndex = type,
		};
		CHECK(type != UINT32_MAX);
		CHECK_VK(vkAllocateMemory(dev.device, &allocInfo, nullptr, &vbMem));
		CHECK_VK(vkBindBufferMemory(dev.device, vb, vbMem, 0));
		void * mapped = nullptr;
		CHECK_VK(vkMapMemory(dev.device, vbMem, 0, VK_WHOLE_SIZE, 0, &mapped));
		std::memcpy(mapped, cubeVerts, sizeof(cubeVerts));
		vkUnmapMemory(dev.device, vbMem);
	}

	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandPoolCreateInfo poolInfo{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
	        .queueFamilyIndex = dev.family,
	};
	CHECK_VK(vkCreateCommandPool(dev.device, &poolInfo, nullptr, &pool));
	VkFence fence = VK_NULL_HANDLE;
	VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
	CHECK_VK(vkCreateFence(dev.device, &fenceInfo, nullptr, &fence));

	// Per-swapchain image views (framebuffers are per acquired image).
	struct EyeImages
	{
		std::vector<VkImage> images;
		std::vector<VkImageView> views;
	};
	EyeImages eyes[2];
	for (int eye = 0; eye < 2; ++eye)
	{
		uint32_t imageCount = 0;
		CHECK_XR(xrEnumerateSwapchainImages(swapchains[eye], 0, &imageCount, nullptr), XR_SUCCESS);
		std::vector<XrSwapchainImageVulkan2KHR> imgs(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
		uint32_t fetched = imageCount;
		CHECK_XR(
		        xrEnumerateSwapchainImages(
		                swapchains[eye], imageCount, &fetched, reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data())),
		        XR_SUCCESS);
		for (const auto & im: imgs)
		{
			eyes[eye].images.push_back(im.image);
			VkImageViewCreateInfo viewInfo{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			        .pNext = nullptr,
			        .flags = 0,
			        .image = im.image,
			        .viewType = VK_IMAGE_VIEW_TYPE_2D,
			        .format = (VkFormat)format,
			        .components = {},
			        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			VkImageView view = VK_NULL_HANDLE;
			CHECK_VK(vkCreateImageView(dev.device, &viewInfo, nullptr, &view));
			eyes[eye].views.push_back(view);
		}
	}

	// ---- Animated frame loop (runs until killed) ----
	XrSwapchainImageAcquireInfo acquireInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = nullptr};
	XrSwapchainImageWaitInfo waitInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .next = nullptr, .timeout = XR_INFINITE_DURATION};
	XrSwapchainImageReleaseInfo releaseInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = nullptr};
	XrFrameWaitInfo frameWaitInfo{.type = XR_TYPE_FRAME_WAIT_INFO, .next = nullptr};
	XrFrameBeginInfo frameBeginInfo{.type = XR_TYPE_FRAME_BEGIN_INFO, .next = nullptr};
	uint64_t frame = 0;
	auto start = std::chrono::steady_clock::now();
	for (;; ++frame)
	{
		XrFrameState frameState{.type = XR_TYPE_FRAME_STATE, .next = nullptr};
		CHECK_XR(xrWaitFrame(session, &frameWaitInfo, &frameState), XR_SUCCESS);
		CHECK_XR(xrBeginFrame(session, &frameBeginInfo), XR_SUCCESS);

		// Animated background (cycles red/blue) + spinning cube.
		float t = (float)((frame * 3) % 240) / 240.0f;
		float a = (float)frame * 0.05f;
		float c = std::cos(a), s = std::sin(a);
		float mvp[16] = {0.5f * c, 0, -0.5f * s, 0, 0, 0.5f, 0, 0, 0.5f * s, 0, 0.5f * c, 0, 0, 0, 0, 1};

		XrCompositionLayerProjectionView projViews[2] = {};
		XrCompositionLayerProjection proj{
		        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
		        .next = nullptr,
		        .layerFlags = 0,
		        .space = local,
		        .viewCount = 2,
		        .views = projViews,
		};
		// Views from the runtime (HMD-derived once a headset connects).
		XrViewLocateInfo viewLocateInfo{
		        .type = XR_TYPE_VIEW_LOCATE_INFO,
		        .next = nullptr,
		        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		        .displayTime = frameState.predictedDisplayTime,
		        .space = local,
		};
		XrViewState viewState{.type = XR_TYPE_VIEW_STATE, .next = nullptr};
		XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
		uint32_t viewCount = 0;
		CHECK_XR(xrLocateViews(session, &viewLocateInfo, &viewState, 2, &viewCount, views), XR_SUCCESS);

		for (int eye = 0; eye < 2; ++eye)
		{
			uint32_t index = 0;
			CHECK_XR(xrAcquireSwapchainImage(swapchains[eye], &acquireInfo, &index), XR_SUCCESS);
			CHECK_XR(xrWaitSwapchainImage(swapchains[eye], &waitInfo), XR_SUCCESS);

			VkCommandBufferAllocateInfo cmdInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			        .pNext = nullptr,
			        .commandPool = pool,
			        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			        .commandBufferCount = 1,
			};
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			CHECK_VK(vkAllocateCommandBuffers(dev.device, &cmdInfo, &cmd));
			VkCommandBufferBeginInfo begin{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			        .pNext = nullptr,
			        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			        .pInheritanceInfo = nullptr,
			};
			CHECK_VK(vkBeginCommandBuffer(cmd, &begin));
			VkFramebufferCreateInfo fbInfo{
			        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			        .pNext = nullptr,
			        .flags = 0,
			        .renderPass = pass,
			        .attachmentCount = 1,
			        .pAttachments = &eyes[eye].views[index],
			        .width = 1024,
			        .height = 1024,
			        .layers = 1,
			};
			VkFramebuffer fb = VK_NULL_HANDLE;
			CHECK_VK(vkCreateFramebuffer(dev.device, &fbInfo, nullptr, &fb));
			VkClearValue clear{};
			clear.color.float32[0] = t;
			clear.color.float32[1] = 0.1f;
			clear.color.float32[2] = 1.0f - t;
			clear.color.float32[3] = 1.0f;
			VkRenderPassBeginInfo rpBegin{
			        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			        .pNext = nullptr,
			        .renderPass = pass,
			        .framebuffer = fb,
			        .renderArea = {{0, 0}, {1024, 1024}},
			        .clearValueCount = 1,
			        .pClearValues = &clear,
			};
			vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			VkDeviceSize off = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
			vkCmdDraw(cmd, 36, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
			CHECK_VK(vkEndCommandBuffer(cmd));
			VkSubmitInfo submit{
			        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			        .pNext = nullptr,
			        .waitSemaphoreCount = 0,
			        .pWaitSemaphores = nullptr,
			        .pWaitDstStageMask = nullptr,
			        .commandBufferCount = 1,
			        .pCommandBuffers = &cmd,
			        .signalSemaphoreCount = 0,
			        .pSignalSemaphores = nullptr,
			};
			vkResetFences(dev.device, 1, &fence);
			CHECK_VK(vkQueueSubmit(dev.queue, 1, &submit, fence));
			CHECK_VK(vkWaitForFences(dev.device, 1, &fence, VK_TRUE, 10'000'000'000ull));
			vkFreeCommandBuffers(dev.device, pool, 1, &cmd);
			vkDestroyFramebuffer(dev.device, fb, nullptr);

			projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[eye].pose = views[eye].pose;
			projViews[eye].fov = views[eye].fov;
			projViews[eye].subImage = {.swapchain = swapchains[eye], .imageRect = {.offset = {0, 0}, .extent = {1024, 1024}}, .imageArrayIndex = 0};
			CHECK_XR(xrReleaseSwapchainImage(swapchains[eye], &releaseInfo), XR_SUCCESS);
		}
		const XrCompositionLayerBaseHeader * layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj)};
		XrFrameEndInfo endInfo{
		        .type = XR_TYPE_FRAME_END_INFO,
		        .next = nullptr,
		        .displayTime = frameState.predictedDisplayTime,
		        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		        .layerCount = 1,
		        .layers = layers,
		};
		CHECK_XR(xrEndFrame(session, &endInfo), XR_SUCCESS);

		if (frame % 120 == 0)
		{
			auto dt = std::chrono::steady_clock::now() - start;
			std::printf(
			        "demo: %llu frames in %.1fs\n",
			        (unsigned long long)frame,
			        std::chrono::duration<double>(dt).count());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
	}
	return 0;
}
