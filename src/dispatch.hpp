#pragma once
#include <map>
#include <vector>
#include "stats.hpp"

// generated from vk.xml
//#include "vk_dispatch_table_helper.h"
#include "vks/VulkanDevice.hpp"

// use the loader's dispatch table pointer as a key for dispatch map lookups
template<typename DispatchableType>
void *GetKey(DispatchableType inst)
{
	return *(void **)inst;
}

struct InstanceData {
	VkLayerInstanceDispatchTable vtable;
	VkInstance instance;
	PFN_vkSetInstanceLoaderData set_instance_loader_data;
	std::vector<VkExtensionProperties> exts;
	Stats stats;

	bool extensionSupported(const char* extensionName)
	{
		for (auto& ext : exts) {
			if (strcmp(ext.extensionName, extensionName) == 0) {
				return true;
			}
		}
		return false;
	}
};

struct QueueData;
struct DeviceData {
	InstanceData *instance = nullptr;

	PFN_vkSetDeviceLoaderData set_device_loader_data;

	IGPUStats *deviceStats = nullptr;

	VkLayerDispatchTable vtable;
	VkPhysicalDevice physical_device;
	VkDevice device;
	vks::VulkanDevice *vulkanDevice = nullptr;

	struct QueueData *graphic_queue = nullptr;
	std::vector<QueueData*> queues;

};

/* Mapped from VkQueue */
struct QueueData {
	struct DeviceData *device;

	VkQueue queue;
	VkQueueFlags flags;
	uint32_t family_index;
	uint64_t timestamp_mask;
};

// layer book-keeping information, to store dispatch tables by key
extern std::map<void *, InstanceData> g_instance_dispatch;
extern std::map<void *, DeviceData> g_device_dispatch;
InstanceData *GetInstanceData(void *key);
DeviceData *GetDeviceData(void *key);
