#pragma once
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
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
	CPUStats cpuStats;

	struct {
		bool quit = false;
		std::thread thread;
	} cpu;

	struct {
		int fd = -1;
		struct sockaddr_un addr;
		bool quit = false;
		std::thread thread;
		std::vector<std::string> lines;
		std::mutex mutex;
	} ss;

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
