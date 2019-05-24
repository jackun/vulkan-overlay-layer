// generated from vk.xml
#include "vk_dispatch_table_helper.h"

#include <vector>
#include <mutex>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
//#include <glm/gtc/matrix_transform.hpp>
//#include <glm/gtc/matrix_inverse.hpp>

#include "vks/VulkanTools.h"
#include "vks/VulkanDevice.hpp"

#include "../external/stb/stb_font_consolas_bold_24_latin1.inl"

struct Vertex
{
	glm::vec2 pos;
	glm::vec2 uv;
	glm::vec3 color;
};

/*
	Mostly self-contained text overlay class
*/
class TextOverlay
{
private:
	vks::VulkanDevice *vulkanDevice;

	VkQueue queue;
	VkFormat colorFormat;
//	VkFormat depthFormat;


	// TextOverlay gets recreated when swpachain resizes so take these as values
	uint32_t frameBufferWidth;
	uint32_t frameBufferHeight;
	int bufferIndex = 0, renderIndex = 0;
	std::mutex biMutex;

	VkSampler sampler;
	VkImage image;
	VkImageView view;
	VkBuffer buffer[2];
	VkDeviceMemory memory[2];
	VkDeviceMemory imageMemory;
	struct {
		VkDeviceMemory memory;
		VkBuffer buffer;
		VkDescriptorBufferInfo descriptor;
	} uniformBuffer;
	VkDescriptorPool descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSet;
	VkPipelineLayout pipelineLayout;
	VkPipelineCache pipelineCache;
	VkPipeline pipeline;
	VkRenderPass renderPass;
	VkCommandPool commandPool;
	std::vector<VkCommandBuffer> cmdBuffers;
	std::vector<VkFramebuffer*> frameBuffers;
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

	// Pointer to mapped vertex buffer
	Vertex *mapped = nullptr;

	stb_fontchar stbFontData[STB_FONT_consolas_bold_24_latin1_NUM_CHARS];
	uint32_t numLetters;
	glm::vec4 fontColor = {1.0f, 1.0f, 1.0f, 1.0f};
public:

	enum TextAlign { alignLeft, alignCenter, alignRight };

	bool visible = true;

	TextOverlay(
		vks::VulkanDevice *vulkanDevice,
		VkQueue queue,
		std::vector<VkFramebuffer> &framebuffers,
		VkFormat colorformat,
//		VkFormat depthformat,
		uint32_t framebufferwidth,
		uint32_t framebufferheight
		/*, std::vector<VkPipelineShaderStageCreateInfo> shaderstages*/);

	~TextOverlay();

	VkPipelineShaderStageCreateInfo loadShader(const uint32_t *shaderCode, const size_t size, VkShaderStageFlagBits stage);
	// Prepare all vulkan resources required to render the font
	// The text overlay uses separate resources for descriptors (pool, sets, layouts), pipelines and command buffers
	void prepareResources();

	// Prepare a separate pipeline for the font rendering decoupled from the main application
	void preparePipeline();

	// Prepare a separate render pass for rendering the text as an overlay
	void prepareRenderPass();

	// Map buffer 
	void beginTextUpdate();

	// Add text to the current buffer
	// todo : drop shadow? color attribute?
	void addText(std::string text, float x, float y, float scale = 1.0f, TextAlign align = alignLeft);

	// Unmap buffer and update command buffers
	void endTextUpdate();

	// Needs to be called by the application
	void updateCommandBuffers(uint32_t image_index, VkImageMemoryBarrier imb);

	// Submit the text command buffers to a queue
	// Does a queue wait idle
	void submit(VkQueue queue, uint32_t bufferindex, VkSubmitInfo submit_info, VkFence fence);
};
