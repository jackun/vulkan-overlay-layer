// https://renderdoc.org/vulkan-layer-guide.html
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <assert.h>
#include <string.h>
#include <cstdlib>

#include <vector>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "dispatch.hpp"
#include "overlay.hpp"

//#include "vks/VulkanTools.h"

using hrc = std::chrono::high_resolution_clock;
using ms = std::chrono::milliseconds;
using us = std::chrono::microseconds;
using ns = std::chrono::nanoseconds;
using sec = std::chrono::seconds;

template< typename T, size_t N >
size_t countof( const T (&)[N] ) { return N; }

//vk_layer.h should have it
#if !defined(VK_LAYER_EXPORT)
#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif
#endif

#if defined(WIN32)
#undef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __declspec(dllexport)
#endif

/**
 * Unreachable macro. Useful for suppressing "control reaches end of non-void
 * function" warnings.
 */
#ifdef HAVE___BUILTIN_UNREACHABLE
#define unreachable(str)    \
do {                        \
   assert(!str);            \
   __builtin_unreachable(); \
} while (0)
#elif defined (_MSC_VER)
#define unreachable(str)    \
do {                        \
   assert(!str);            \
   __assume(0);             \
} while (0)
#else
#define unreachable(str) assert(!str)
#endif

#define vk_foreach_struct(__iter, __start) \
	for (struct VkBaseOutStructure *__iter = (struct VkBaseOutStructure *)(__start); \
		__iter; __iter = __iter->pNext)

#define vk_foreach_struct_const(__iter, __start) \
	for (const struct VkBaseInStructure *__iter = (const struct VkBaseInStructure *)(__start); \
		__iter; __iter = __iter->pNext)

// single global lock, for simplicity
std::mutex global_lock;
typedef std::lock_guard<std::mutex> scoped_lock;

/*struct InstanceData {
	VkLayerInstanceDispatchTable vtable;
	VkInstance instance;
};*/

// use the loader's dispatch table pointer as a key for dispatch map lookups
/*template<typename DispatchableType>
void *GetKey(DispatchableType inst)
{
	return *(void **)inst;
}*/

// layer book-keeping information, to store dispatch tables by key
std::map<void *, InstanceData> g_instance_dispatch;
std::map<void *, DeviceData> g_device_dispatch;
std::map<void *, QueueData> g_queue_data;

static float overlay_x = 25.0f, overlay_y = 25.0f;
static bool avg_cpus = false;

InstanceData *GetInstanceData(void *key)
{
	scoped_lock l(global_lock);
	return &g_instance_dispatch[GetKey(key)];
}

DeviceData *GetDeviceData(void *key)
{
	scoped_lock l(global_lock);
	return &g_device_dispatch[GetKey(key)];
}

QueueData *GetQueueData(void *key)
{
	scoped_lock l(global_lock);
	return &g_queue_data[key];
}

struct PresentStats
{
	hrc::time_point last_fps_update;
	unsigned n_frames_since_update = 0;
	float last_fps = 0;
};

std::map<void*, PresentStats> present_stats;

/* Mapped from VkSwapchainKHR */
struct SwapchainData {
	DeviceData *device = nullptr;
	TextOverlay *overlay = nullptr;
	PresentStats stats;

	VkSwapchainKHR swapchain;
	unsigned width, height;
	VkFormat format;

	std::vector<VkImage> images;
	std::vector<VkImageView> image_views;
	std::vector<VkFramebuffer> framebuffers;

	VkRenderPass render_pass = nullptr;

	VkCommandPool command_pool = nullptr;

	VkSemaphore submission_semaphore = nullptr;
};

std::map<void *, SwapchainData> g_swapchain_data;
SwapchainData *GetSwapchainData(void *key)
{
	scoped_lock l(global_lock);
	return &g_swapchain_data[key];
}

VkPhysicalDeviceProperties2 initDeviceProperties2(void * pNext) {
	VkPhysicalDeviceProperties2 props2{};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
	props2.pNext = pNext;
	return props2;
}
void readExtensions(VkPhysicalDevice device)
{
	assert(device != NULL);
	VkResult vkRes;
	do {
		uint32_t extCount;
		vkRes = GetInstanceData(device)->vtable.EnumerateDeviceExtensionProperties(device, NULL, &extCount, NULL);
		assert(!vkRes);
		std::vector<VkExtensionProperties> exts(extCount);
		vkRes = GetInstanceData(device)->vtable.EnumerateDeviceExtensionProperties(device, NULL, &extCount, &exts.front());
		for (auto& ext : exts) {
			#ifndef NDEBUG
			std::cerr << "Device ext: " << ext.extensionName << std::endl;
			#endif
			GetInstanceData(device)->exts.push_back(ext);
		}
	} while (vkRes == VK_INCOMPLETE);
	assert(!vkRes);
}

static QueueData *new_queue_data(VkQueue queue,
							 const VkQueueFamilyProperties *family_props,
							 uint32_t family_index,
							 DeviceData *device_data)
{
	QueueData *data = GetQueueData(queue);
	data->device = device_data;
	data->queue = queue;
	data->flags = family_props->queueFlags;
	data->timestamp_mask = (1ull << family_props->timestampValidBits) - 1;
	data->family_index = family_index;
	//LIST_INITHEAD(&data.running_command_buffer);

	if (data->flags & VK_QUEUE_GRAPHICS_BIT)
		device_data->graphic_queue = data;

	return data;
}

static void DeviceUnmapQueues(struct DeviceData *device_data)
{
	//scoped_lock l(global_lock);

	for (QueueData* qd: device_data->queues) {
		g_queue_data.erase(qd->queue);
	}
	device_data->queues.clear();
}

static void DeviceMapQueues(struct DeviceData *data,
							const VkDeviceCreateInfo *pCreateInfo)
{
	InstanceData *instance_data = data->instance;
	uint32_t n_family_props;
	instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(data->physical_device,
																&n_family_props,
																NULL);

	std::vector<VkQueueFamilyProperties> family_props(n_family_props);
	instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(data->physical_device,
																&n_family_props,
																family_props.data());

	for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
		for (uint32_t j = 0; j < pCreateInfo->pQueueCreateInfos[i].queueCount; j++) {
			VkQueue queue;
			data->vtable.GetDeviceQueue(data->device,
				pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
				j, &queue);

			VK_CHECK_RESULT(data->set_device_loader_data(data->device, queue));

			data->queues.push_back(new_queue_data(queue,
					&family_props[pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex],
					pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex, data));
		}
	}
}

// Update the text buffer displayed by the text overlay
static void updateTextOverlay(const SwapchainData * const swapchain, TextOverlay *textOverlay)
{
	const InstanceData * const instance = swapchain->device->instance;
	const DeviceData * const device_data = swapchain->device;

	textOverlay->beginTextUpdate();

	//TODO throttle
	std::time_t t = std::time(nullptr);
	std::stringstream time;
	time << std::put_time(std::localtime(&t), "%T");

	textOverlay->addText(time.str(), overlay_x, overlay_y);

	/* 24.f stb font size */
	float spacing_y = 20.f;
	float scaling_cpu = .9f;
	float tmp_x = overlay_x, tmp_y = overlay_y;
	std::stringstream ss;

	ss << "FPS: " << std::fixed << std::setprecision(0) << swapchain->stats.last_fps;
	tmp_y += spacing_y;
	textOverlay->addText(ss.str(), tmp_x, tmp_y, 0.9f);

	double avg_cpus_percent = 0;
	if (instance /*&& instance->stats.Updated()*/) {

		if (device_data->deviceStats) {
			int value = -1, value2 = -1;

			value = device_data->deviceStats->getCoreClock();
			value2 = device_data->deviceStats->getCoreTemp();
			ss.str(""); ss.clear(); 
			ss << "Core: ";
			if (value > -1)
				ss << value << "MHz ";
			if (value2 > -1)
				ss << value2 << "°C";
			if (value > -1 || value2 > -1) {
				tmp_y += spacing_y;
				textOverlay->addText(ss.str(), tmp_x, tmp_y, 0.9f);
			}

			ss.str(""); ss.clear(); 
			ss << "Mem:  ";
			value = device_data->deviceStats->getMemClock();
			value2 = device_data->deviceStats->getMemTemp();
			if (value > -1)
				ss << value << "MHz ";
			if (value2 > -1)
				ss << value2 << "°C";
			if (value > -1 || value2 > -1) {
				tmp_y += spacing_y;
				textOverlay->addText(ss.str(), tmp_x, tmp_y, 0.9f);
			}

			value = device_data->deviceStats->getGPUUsage();
			if (value > -1) {
				ss.str(""); ss.clear(); ss << "Busy: " << value << "%";
				tmp_y += spacing_y;
				textOverlay->addText(ss.str(), tmp_x, tmp_y, 0.9f);
			}

		}

		//instance->stats.UpdateCPUData();
		int cpuid = 0;
		//double period = instance->stats.GetCPUPeriod();
		//printf("period %f\n", period);
		for (const CPUData &cpuData : instance->stats.GetCPUData()) {

			double total = (double)(cpuData.totalPeriod == 0 ? 1 : cpuData.totalPeriod);
			double percent = 0;
			double v[4];
			v[0] = cpuData.nicePeriod / total * 100.0;
			v[1] = cpuData.userPeriod / total * 100.0;

			/* if not detailed */
			v[2] = cpuData.systemAllPeriod / total * 100.0;
			v[3] = (cpuData.stealPeriod + cpuData.guestPeriod) / total * 100.0;
			percent = std::clamp(v[0]+v[1]+v[2]+v[3], 0.0, 100.0);
			//percent = v[0]+v[1]+v[2]+v[3];

			if (!avg_cpus) {
				ss.str(""); ss.clear(); ss << "CPU" << cpuid << ": " << std::fixed << std::setprecision(0) << percent << "%";
				tmp_y += spacing_y * scaling_cpu;
				textOverlay->addText(ss.str(), tmp_x, tmp_y, scaling_cpu);
			} else {
				avg_cpus_percent += percent;
			}
			cpuid++;
		}

		if (avg_cpus) {
			ss.str(""); ss.clear(); ss << "CPU: " << std::fixed << std::setprecision(0) << (avg_cpus_percent / cpuid) << "%";
			tmp_y += spacing_y * scaling_cpu;
			textOverlay->addText(ss.str(), tmp_x, tmp_y, scaling_cpu);
			cpuid++;
		}

		//textOverlay->addText("Some º text 1", 50.0f, 35.0f, TextOverlay::alignLeft);
		//textOverlay->addText("Some text 2 þñ©öäüÕ", 50.0f, 65.0f, TextOverlay::alignLeft);
	}

	textOverlay->endTextUpdate();
}

///////////////////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown

static VkLayerDeviceCreateInfo *get_device_chain_info(const VkDeviceCreateInfo *pCreateInfo,
													VkLayerFunction func)
{
   vk_foreach_struct(item, pCreateInfo->pNext) {
		if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
				((VkLayerDeviceCreateInfo *) item)->function == func)
			return (VkLayerDeviceCreateInfo *)item;
	}
	unreachable("device chain info not found");
	return nullptr;
}

static VkLayerInstanceCreateInfo *get_instance_chain_info(const VkInstanceCreateInfo *pCreateInfo,
														VkLayerFunction func)
{
	vk_foreach_struct(item, pCreateInfo->pNext) {
		if (item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
			((VkLayerInstanceCreateInfo *) item)->function == func)
		return (VkLayerInstanceCreateInfo *) item;
	}
	unreachable("instance chain info not found");
	return NULL;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL Overlay_CreateInstance(
	const VkInstanceCreateInfo*                 pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkInstance*                                 pInstance)
{
	VkLayerInstanceCreateInfo *layerCreateInfo = get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

	if (!layerCreateInfo)
	{
		// No loader instance create info
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	assert(layerCreateInfo->u.pLayerInfo);
	PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	// move chain on for next layer
	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

	PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");
	if (!fpCreateInstance) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/*std::vector<const char*> createExts;
	createExts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	for(uint32_t i=0; i<pCreateInfo->enabledExtensionCount; i++) {
		createExts.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
	}

	VkInstanceCreateInfo createInfo = *pCreateInfo;
	createInfo.enabledExtensionCount = createExts.size();
	createInfo.ppEnabledExtensionNames = createExts.data();

	for(auto c : createExts) {
		printf("%s exts: %s\n", __func__, c);
	}*/

	VkResult ret = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
	if (ret != VK_SUCCESS)
		return ret;

	/*auto EnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties) gpa(*pInstance, "vkEnumerateInstanceExtensionProperties");
	std::vector<VkExtensionProperties> extensions;
	VkResult vkRes;
	extensions.clear();
	do {
		uint32_t extCount;
		vkRes = EnumerateInstanceExtensionProperties(NULL, &extCount, NULL);
		if (vkRes != VK_SUCCESS) {
			break;
		}
		std::vector<VkExtensionProperties> exts(extCount);
		vkRes = EnumerateInstanceExtensionProperties(NULL, &extCount, &exts.front());
		extensions.insert(extensions.end(), exts.begin(), exts.end());
	} while (vkRes == VK_INCOMPLETE);*/

	// fetch our own dispatch table for the functions we need, into the next layer
	VkLayerInstanceDispatchTable dispatchTable;
	layer_init_instance_dispatch_table(*pInstance, &dispatchTable, gpa);
	//TODO nullptr :(
	dispatchTable.EnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties) gpa(*pInstance, "vkEnumerateInstanceExtensionProperties");

	GetInstanceData(*pInstance)->vtable = dispatchTable;
	GetInstanceData(*pInstance)->instance = *pInstance;

	int env_pos_x, env_pos_y;
	char* env = getenv ("NUUDEL_POS");
	if (env && sscanf(env, "%d%*[.,: ]%d", &env_pos_x, &env_pos_y) == 2) {
		overlay_x = env_pos_x;
		overlay_y = env_pos_y;
	}

	int env_avg_cpus = 0;
	env = getenv ("NUUDEL_AVGCPU");
	if (env && sscanf(env, "%d", &env_avg_cpus) == 1) {
		avg_cpus = !!env_avg_cpus;
	}

	return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL Overlay_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
	scoped_lock l(global_lock);
	void *key = GetKey(instance);
	g_instance_dispatch[key].vtable.DestroyInstance(instance, pAllocator);
	g_instance_dispatch.erase(key);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL Overlay_CreateDevice(
	VkPhysicalDevice                            physicalDevice,
	const VkDeviceCreateInfo*                   pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkDevice*                                   pDevice)
{
	VkLayerDeviceCreateInfo *layerCreateInfo = get_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

	if (!layerCreateInfo)
	{
		// No loader instance create info
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

	PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
	if (!fpCreateDevice) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	// move chain on for next layer
	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

	/* modify VkDeviceCreateInfo struct for our own use here if needed */

	VkResult ret = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
	if (ret != VK_SUCCESS)
		return ret;

	InstanceData *instance = GetInstanceData(physicalDevice);
	if (instance->exts.size() == 0)
		readExtensions(physicalDevice);

	// fetch our own dispatch table for the functions we need, into the next layer
	VkLayerDispatchTable dispatchTable;
	layer_init_device_dispatch_table(*pDevice, &dispatchTable, gdpa);

	// store the table by key
	// TODO the whole thing needs a lock until all fields are populated?
	GetDeviceData(*pDevice)->vtable = dispatchTable;
	GetDeviceData(*pDevice)->physical_device = physicalDevice;
	GetDeviceData(*pDevice)->device = *pDevice;
	GetDeviceData(*pDevice)->instance = instance;

	VkLayerDeviceCreateInfo *load_data_info = get_device_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
	GetDeviceData(*pDevice)->set_device_loader_data = load_data_info->u.pfnSetDeviceLoaderData;

	DeviceMapQueues(GetDeviceData(*pDevice), pCreateInfo);

	GetDeviceData(*pDevice)->vulkanDevice = new vks::VulkanDevice(physicalDevice,
		*pDevice, &GetDeviceData(*pDevice)->vtable, &instance->vtable);

	VkPhysicalDeviceProperties properties;
	instance->vtable.GetPhysicalDeviceProperties(physicalDevice, &properties);
	//printf("Vendor: 0x%04X Device: 0x%04X\n", properties.vendorID, properties.deviceID);

	//FIXME nullptr, nullptr, nullptr, nullptr, nullptr, nullptr :(
	if (false && instance->extensionSupported(VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) {
		VkPhysicalDevicePCIBusInfoPropertiesEXT  extProps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT };
		VkPhysicalDeviceProperties2 deviceProps2(initDeviceProperties2(&extProps));
		instance->vtable.GetPhysicalDeviceProperties2KHR(physicalDevice, &deviceProps2);
		printf("PCI bus info: %04d:%04d:04%d.%d\n", extProps.pciDomain, extProps.pciBus, extProps.pciDevice, extProps.pciFunction);
	}

	//FIXME nullptr, nullptr, nullptr, nullptr, nullptr, nullptr :(
	if (false && instance->extensionSupported(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
		VkPhysicalDeviceDriverPropertiesKHR driverProps = {};
		driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR;
		driverProps.pNext = nullptr;

		VkPhysicalDeviceProperties2 pp2(initDeviceProperties2(&driverProps));
		instance->vtable.GetPhysicalDeviceProperties2KHR(physicalDevice, &pp2);

		printf("Driver: %s %s\n", driverProps.driverName, driverProps.driverInfo);
	}

	// get physical device props, IF I COULD, to select proper sysfs paths
	int env_amdgpu_index = 0;
	char *env = getenv ("NUUDEL_AMDGPU_INDEX");
	if (env && sscanf(env, "%d", &env_amdgpu_index) == 1)
		GetDeviceData(*pDevice)->deviceStats = new AMDgpuStats(env_amdgpu_index);
	return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL Overlay_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
	scoped_lock l(global_lock);
	auto key = GetKey(device);

	DeviceData *device_data = &g_device_dispatch[key];

	DeviceUnmapQueues(device_data);

	delete device_data->vulkanDevice;
	delete device_data->deviceStats;

	device_data->vtable.DestroyDevice(device, pAllocator);
	g_device_dispatch.erase(key);
}

///////////////////////////////////////////////////////////////////////////////////////////
// Enumeration function

VK_LAYER_EXPORT VkResult VKAPI_CALL Overlay_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
																		VkLayerProperties *pProperties)
{
	if(pPropertyCount) *pPropertyCount = 1;

	if(pProperties)
	{
		strcpy(pProperties->layerName, "VK_LAYER_NUUDEL_overlay");
		strcpy(pProperties->description, "Overlay layer");
		pProperties->implementationVersion = 1;
		pProperties->specVersion = VK_API_VERSION_1_0;
	}

	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL Overlay_EnumerateDeviceLayerProperties(
	VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
	return Overlay_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL Overlay_EnumerateInstanceExtensionProperties(
	const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
	if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_NUUDEL_overlay"))
		return VK_ERROR_LAYER_NOT_PRESENT;

	// don't expose any extensions
	if(pPropertyCount)
		*pPropertyCount = 0;
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL Overlay_EnumerateDeviceExtensionProperties(
	VkPhysicalDevice physicalDevice, const char *pLayerName,
	uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
	// pass through any queries that aren't to us
	if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_NUUDEL_overlay"))
	{
		if(physicalDevice == VK_NULL_HANDLE)
			return VK_SUCCESS;

		scoped_lock l(global_lock);
		return g_instance_dispatch[GetKey(physicalDevice)].vtable.EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
	}

	// don't expose any extensions
	if(pPropertyCount)
		*pPropertyCount = 0;
	return VK_SUCCESS;
}

static void SetupSwapchainData(struct SwapchainData *data,
								const VkSwapchainCreateInfoKHR *pCreateInfo)
{
	data->width = pCreateInfo->imageExtent.width;
	data->height = pCreateInfo->imageExtent.height;
	data->format = pCreateInfo->imageFormat;

	struct DeviceData *device_data = data->device;

	/* Render pass */
	VkAttachmentDescription attachment_desc = {};
	attachment_desc.format = pCreateInfo->imageFormat;
	attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment_desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference color_attachment = {};
	color_attachment.attachment = 0;
	color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_attachment;

	VkSubpassDependency dependency = {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo render_pass_info = {};
	render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.attachmentCount = 1;
	render_pass_info.pAttachments = &attachment_desc;
	render_pass_info.subpassCount = 1;
	render_pass_info.pSubpasses = &subpass;
	render_pass_info.dependencyCount = 1;
	render_pass_info.pDependencies = &dependency;
	VK_CHECK_RESULT(device_data->vtable.CreateRenderPass(device_data->device,
												 &render_pass_info,
												 NULL, &data->render_pass));

   //setup_swapchain_data_pipeline(data);

	uint32_t n_images = 0;
	VK_CHECK_RESULT(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
													  data->swapchain,
													  &n_images,
													  NULL));

	data->images.resize(n_images);
	data->image_views.resize(n_images);
	data->framebuffers.resize(n_images);

	VK_CHECK_RESULT(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
													  data->swapchain,
													  &n_images,
													  data->images.data()));

	/* Image views */
	VkImageViewCreateInfo view_info = {};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = pCreateInfo->imageFormat;
	view_info.components.r = VK_COMPONENT_SWIZZLE_R;
	view_info.components.g = VK_COMPONENT_SWIZZLE_G;
	view_info.components.b = VK_COMPONENT_SWIZZLE_B;
	view_info.components.a = VK_COMPONENT_SWIZZLE_A;
	view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	for (uint32_t i = 0; i < n_images; i++) {
		view_info.image = data->images[i];
		VK_CHECK_RESULT(device_data->vtable.CreateImageView(device_data->device,
													&view_info, NULL,
													&data->image_views[i]));
	}

	/* Framebuffers */
	VkImageView attachment[1];
	VkFramebufferCreateInfo fb_info = {};
	fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb_info.renderPass = data->render_pass;
	fb_info.attachmentCount = 1;
	fb_info.pAttachments = attachment;
	fb_info.width = data->width;
	fb_info.height = data->height;
	fb_info.layers = 1;
	for (uint32_t i = 0; i < data->image_views.size(); i++) {
		attachment[0] = data->image_views[i];
		VK_CHECK_RESULT(device_data->vtable.CreateFramebuffer(device_data->device, &fb_info,
													NULL, &data->framebuffers[i]));
	}

	// Create command pool
	VkCommandPoolCreateInfo cmdPoolInfo = {};
	cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdPoolInfo.queueFamilyIndex = device_data->graphic_queue->family_index;
	cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VK_CHECK_RESULT(device_data->vtable.CreateCommandPool(device_data->device, &cmdPoolInfo, nullptr, &data->command_pool));

	data->overlay = new TextOverlay(device_data->vulkanDevice,
		device_data->graphic_queue->queue, data->framebuffers,
		data->format, data->width, data->height);
	//updateTextOverlay(data->overlay);
}

static void ShutdownSwapchainData(struct SwapchainData *data)
{
	struct DeviceData *device_data = data->device;

	delete data->overlay;
	data->overlay = nullptr;

	for (uint32_t i = 0; i < data->images.size(); i++) {
		device_data->vtable.DestroyImageView(device_data->device, data->image_views[i], NULL);
		device_data->vtable.DestroyFramebuffer(device_data->device, data->framebuffers[i], NULL);
	}

	device_data->vtable.DestroyRenderPass(device_data->device, data->render_pass, NULL);
	device_data->vtable.DestroyCommandPool(device_data->device, data->command_pool, NULL);

	if (data->submission_semaphore)
		device_data->vtable.DestroySemaphore(device_data->device, data->submission_semaphore, NULL);
	data->submission_semaphore = nullptr;
}

static void RenderSwapchainDisplay(struct SwapchainData *data,
									const VkSemaphore *wait_semaphores,
									unsigned n_wait_semaphores,
									unsigned image_index)
{
	struct DeviceData *device_data = data->device;

	/* Bounce the image to display back to color attachment layout for
	* rendering on top of it.
	*/
	VkImageMemoryBarrier imb;
	imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imb.pNext = nullptr;
	imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	imb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	imb.image = data->images[image_index];
	imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imb.subresourceRange.baseMipLevel = 0;
	imb.subresourceRange.levelCount = 1;
	imb.subresourceRange.baseArrayLayer = 0;
	imb.subresourceRange.layerCount = 1;
	imb.srcQueueFamilyIndex = device_data->graphic_queue->family_index;
	imb.dstQueueFamilyIndex = device_data->graphic_queue->family_index;

	//device_data->vtable.CmdPipelineBarrier(command_buffer,
										  //VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
										  //VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
										  //0,          /* dependency flags */
										  //0, nullptr, /* memory barriers */
										  //0, nullptr, /* buffer memory barriers */
										  //1, &imb);   /* image memory barriers */

	/* draw stuff */
	/*static int throttle = 0;
	if (!throttle)
		updateTextOverlay(data, data->overlay);
	throttle = (throttle+1) % 20;*/
	data->overlay->updateCommandBuffers(image_index, imb);

	if (data->submission_semaphore) {
		device_data->vtable.DestroySemaphore(device_data->device,
											data->submission_semaphore,
											NULL);
   }

	/* Submission semaphore */
	VkSemaphoreCreateInfo semaphore_info = {};
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VK_CHECK_RESULT(device_data->vtable.CreateSemaphore(device_data->device, &semaphore_info,
												NULL, &data->submission_semaphore));

	VkSubmitInfo submit_info = {};
	VkPipelineStageFlags stage_wait = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	//submit_info.commandBufferCount = 1;
	//submit_info.pCommandBuffers = &command_buffer;
	submit_info.pWaitDstStageMask = &stage_wait;
	submit_info.waitSemaphoreCount = n_wait_semaphores;
	submit_info.pWaitSemaphores = wait_semaphores;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &data->submission_semaphore;

	// Submit text overlay to queue
	data->overlay->submit(device_data->graphic_queue->queue, image_index, submit_info);
}

VK_LAYER_EXPORT void VKAPI_CALL Overlay_DestroySwapchainKHR(
	VkDevice                                    device,
	VkSwapchainKHR                              swapchain,
	const VkAllocationCallbacks*                pAllocator)
{
	struct SwapchainData *swapchain_data = GetSwapchainData(swapchain);

	ShutdownSwapchainData(swapchain_data);
	swapchain_data->device->vtable.DestroySwapchainKHR(device, swapchain, pAllocator);

	{
		scoped_lock l(global_lock);
		g_swapchain_data.erase(GetKey(device));
	}
}

VK_LAYER_EXPORT VkResult VKAPI_CALL Overlay_QueuePresentKHR(
	VkQueue                                     queue,
	const VkPresentInfoKHR*                     pPresentInfo)
{
	VkResult result = VK_SUCCESS;
	QueueData *queue_data = GetQueueData(queue);

	auto now = hrc::now();

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
		VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];

		SwapchainData *swapchain_data = GetSwapchainData(swapchain);

		PresentStats& ps = swapchain_data->stats;
		auto dur = std::chrono::duration_cast<ms>(now - ps.last_fps_update).count();

		if (dur >= 500) {
			//printf("FPS: %0.f\n", ps.n_frames_since_update / (dur/1000.f));
			ps.last_fps = ps.n_frames_since_update / (dur/1000.f);
			ps.n_frames_since_update = 0;
			ps.last_fps_update = now;
			queue_data->device->instance->stats.UpdateCPUData();
			updateTextOverlay(swapchain_data, swapchain_data->overlay);
		}

		ps.n_frames_since_update ++;

		VkPresentInfoKHR present_info = *pPresentInfo;
		present_info.swapchainCount = 1;
		present_info.pSwapchains = &swapchain;

		RenderSwapchainDisplay(swapchain_data,
					pPresentInfo->pWaitSemaphores,
					pPresentInfo->waitSemaphoreCount,
					pPresentInfo->pImageIndices[i]);

		/* Because the submission of the overlay draw waits on the semaphores
		* handed for present, we don't need to have this present operation
		* wait on them as well, we can just wait on the overlay submission
		* semaphore.
		*/
		present_info.pWaitSemaphores = &swapchain_data->submission_semaphore;
		present_info.waitSemaphoreCount = 1;

		VkResult chain_result;
		{
			scoped_lock l(global_lock);
			//chain_result = g_device_dispatch[GetKey(queue)].vtable.QueuePresentKHR(queue, pPresentInfo);
			chain_result= queue_data->device->vtable.QueuePresentKHR(queue, &present_info);
		}

		if (pPresentInfo->pResults)
			pPresentInfo->pResults[i] = chain_result;
		if (chain_result != VK_SUCCESS && result == VK_SUCCESS)
			result = chain_result;
	}

	return result;
}

VK_LAYER_EXPORT VkResult Overlay_CreateSwapchainKHR(
	VkDevice                                    device,
	const VkSwapchainCreateInfoKHR*             pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSwapchainKHR*                             pSwapchain)
{

	scoped_lock l(global_lock);

	VkResult result = g_device_dispatch[GetKey(device)].vtable.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
	if (result != VK_SUCCESS) return result;

	SwapchainData *swapchain_data = &g_swapchain_data[*pSwapchain];
	swapchain_data->swapchain = *pSwapchain;
	swapchain_data->device = &g_device_dispatch[GetKey(device)];
	SetupSwapchainData(swapchain_data, pCreateInfo);
	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////
// GetProcAddr functions, entry points of the layer
#ifdef __cplusplus
extern "C" {
#endif

#define GETPROCADDR(func) if(!strcmp(pName, "vk" #func)) return (PFN_vkVoidFunction)&Overlay_##func;

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL Overlay_GetDeviceProcAddr(VkDevice device, const char *pName)
{
	//printf("%s: %s\n", __func__, pName);
	// device chain functions we intercept
	GETPROCADDR(GetDeviceProcAddr);
	GETPROCADDR(EnumerateDeviceLayerProperties);
	GETPROCADDR(EnumerateDeviceExtensionProperties);
	GETPROCADDR(CreateDevice);
	GETPROCADDR(DestroyDevice);
	//GETPROCADDR(BeginCommandBuffer);
	//GETPROCADDR(CmdDraw);
	//GETPROCADDR(CmdDrawIndexed);
	//GETPROCADDR(EndCommandBuffer);
	GETPROCADDR(QueuePresentKHR);
	GETPROCADDR(CreateSwapchainKHR);
	GETPROCADDR(DestroySwapchainKHR);

	{
		scoped_lock l(global_lock);
		return g_device_dispatch[GetKey(device)].vtable.GetDeviceProcAddr(device, pName);
	}
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL Overlay_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
	// instance chain functions we intercept
	GETPROCADDR(GetInstanceProcAddr);
	GETPROCADDR(EnumerateInstanceLayerProperties);
	GETPROCADDR(EnumerateInstanceExtensionProperties);
	GETPROCADDR(CreateInstance);
	GETPROCADDR(DestroyInstance);

	// device chain functions we intercept
	GETPROCADDR(GetDeviceProcAddr);
	GETPROCADDR(EnumerateDeviceLayerProperties);
	GETPROCADDR(EnumerateDeviceExtensionProperties);
	GETPROCADDR(CreateDevice);
	GETPROCADDR(DestroyDevice);
	//GETPROCADDR(BeginCommandBuffer);
	//GETPROCADDR(CmdDraw);
	//GETPROCADDR(CmdDrawIndexed);
	//GETPROCADDR(EndCommandBuffer);

	{
		scoped_lock l(global_lock);
		return g_instance_dispatch[GetKey(instance)].vtable.GetInstanceProcAddr(instance, pName);
	}
}

#ifdef __cplusplus
}
#endif
