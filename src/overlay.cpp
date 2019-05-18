#include <array>
#include <cstdlib>
#include "dispatch.hpp"
#include "overlay.hpp"
#include "vks/VulkanTools.h"

#define VERTEX_BUFFER_BIND_ID 0
#define ENABLE_VALIDATION false

// Max. number of chars the text overlay buffer can hold
#define TEXTOVERLAY_MAX_CHAR_COUNT 2048u

static const uint32_t overlay_vert_spv[] = {
#include "overlay.vert.spv.h"
};
static const uint32_t overlay_frag_spv[] = {
#include "overlay.frag.spv.h"
};

VkPipelineShaderStageCreateInfo TextOverlay::loadShader(const uint32_t *shaderCode, const size_t size, VkShaderStageFlagBits stage)
{
	VkPipelineShaderStageCreateInfo shaderStage = {};
	shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStage.stage = stage;
	shaderStage.module = vks::tools::loadShader(shaderCode, size, vulkanDevice->logicalDevice);
	shaderStage.pName = "main"; // todo : make param
	assert(shaderStage.module != VK_NULL_HANDLE);
	return shaderStage;
}

TextOverlay::TextOverlay(
	vks::VulkanDevice *vulkanDevice,
	VkQueue queue,
	std::vector<VkFramebuffer> &framebuffers,
	VkFormat colorformat,
//	VkFormat depthformat,
	uint32_t framebufferwidth,
	uint32_t framebufferheight
	/*, std::vector<VkPipelineShaderStageCreateInfo> shaderstages*/)
{
	this->vulkanDevice = vulkanDevice;
	this->queue = queue;
	this->colorFormat = colorformat;
//	this->depthFormat = depthformat;

	this->frameBuffers.resize(framebuffers.size());
	for (uint32_t i = 0; i < framebuffers.size(); i++)
	{
		this->frameBuffers[i] = &framebuffers[i];
	}

	this->shaderStages.push_back(loadShader(overlay_vert_spv, sizeof(overlay_vert_spv), VK_SHADER_STAGE_VERTEX_BIT));
	this->shaderStages.push_back(loadShader(overlay_frag_spv, sizeof(overlay_frag_spv), VK_SHADER_STAGE_FRAGMENT_BIT));

	this->frameBufferWidth = framebufferwidth;
	this->frameBufferHeight = framebufferheight;

	char *env = getenv("NUUDEL_RGBA");
	int r,g,b,a, ret;
	if (env && (ret = sscanf(env, "%d,%d,%d,%d", &r, &g, &b, &a)) >= 3)
	{
		fontColor[0] = r / 255.f;
		fontColor[1] = g / 255.f;
		fontColor[2] = b / 255.f;
		if (ret == 4)
			fontColor[3] = a / 255.f;
	}

	cmdBuffers.resize(framebuffers.size());
	prepareResources();
	prepareRenderPass();
	preparePipeline();
}

TextOverlay::~TextOverlay()
{
	// Free up all Vulkan resources requested by the text overlay
	for (auto& shaderStage : shaderStages)
	{
		vulkanDevice->getDispatch()->DestroyShaderModule(vulkanDevice->logicalDevice, shaderStage.module, nullptr);
	}
	vulkanDevice->getDispatch()->DestroySampler(vulkanDevice->logicalDevice, sampler, nullptr);
	vulkanDevice->getDispatch()->DestroyImage(vulkanDevice->logicalDevice, image, nullptr);
	vulkanDevice->getDispatch()->DestroyImageView(vulkanDevice->logicalDevice, view, nullptr);
	vulkanDevice->getDispatch()->DestroyBuffer(vulkanDevice->logicalDevice, buffer[0], nullptr);
	vulkanDevice->getDispatch()->DestroyBuffer(vulkanDevice->logicalDevice, buffer[1], nullptr);
	vulkanDevice->getDispatch()->FreeMemory(vulkanDevice->logicalDevice, memory[0], nullptr);
	vulkanDevice->getDispatch()->FreeMemory(vulkanDevice->logicalDevice, memory[1], nullptr);
	vulkanDevice->getDispatch()->FreeMemory(vulkanDevice->logicalDevice, imageMemory, nullptr);
	vulkanDevice->getDispatch()->DestroyDescriptorSetLayout(vulkanDevice->logicalDevice, descriptorSetLayout, nullptr);
	vulkanDevice->getDispatch()->DestroyDescriptorPool(vulkanDevice->logicalDevice, descriptorPool, nullptr);
	vulkanDevice->getDispatch()->DestroyPipelineLayout(vulkanDevice->logicalDevice, pipelineLayout, nullptr);
	vulkanDevice->getDispatch()->DestroyPipelineCache(vulkanDevice->logicalDevice, pipelineCache, nullptr);
	vulkanDevice->getDispatch()->DestroyPipeline(vulkanDevice->logicalDevice, pipeline, nullptr);
	vulkanDevice->getDispatch()->DestroyRenderPass(vulkanDevice->logicalDevice, renderPass, nullptr);
	vulkanDevice->getDispatch()->DestroyCommandPool(vulkanDevice->logicalDevice, commandPool, nullptr);
}

// Prepare all vulkan resources required to render the font
// The text overlay uses separate resources for descriptors (pool, sets, layouts), pipelines and command buffers
void TextOverlay::prepareResources()
{
	const uint32_t fontWidth = STB_FONT_consolas_bold_24_latin1_BITMAP_WIDTH;
	const uint32_t fontHeight = STB_FONT_consolas_bold_24_latin1_BITMAP_WIDTH;

	static unsigned char font24pixels[fontWidth][fontHeight];
	stb_font_consolas_bold_24_latin1(stbFontData, font24pixels, fontHeight);

	// Command buffer

	// Pool
	VkCommandPoolCreateInfo cmdPoolInfo = {};
	cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdPoolInfo.queueFamilyIndex = vulkanDevice->queueFamilyIndices.graphics;
	cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateCommandPool(vulkanDevice->logicalDevice, &cmdPoolInfo, nullptr, &commandPool));

	VkCommandBufferAllocateInfo cmdBufAllocateInfo =
		vks::initializers::commandBufferAllocateInfo(
			commandPool,
			VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			(uint32_t)cmdBuffers.size());

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->AllocateCommandBuffers(vulkanDevice->logicalDevice, &cmdBufAllocateInfo, cmdBuffers.data()));

	const DeviceData *device_data = &g_device_dispatch[GetKey(vulkanDevice->logicalDevice)];
	for (uint32_t i = 0; i < cmdBuffers.size(); ++i)
		device_data->set_device_loader_data(vulkanDevice->logicalDevice, cmdBuffers[i]);

	// Vertex buffer
	VkDeviceSize bufferSize = TEXTOVERLAY_MAX_CHAR_COUNT * sizeof(Vertex);

	VkBufferCreateInfo bufferInfo = vks::initializers::bufferCreateInfo(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, bufferSize);
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateBuffer(vulkanDevice->logicalDevice, &bufferInfo, nullptr, &buffer[0]));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateBuffer(vulkanDevice->logicalDevice, &bufferInfo, nullptr, &buffer[1]));

	VkMemoryRequirements memReqs;
	VkMemoryAllocateInfo allocInfo = vks::initializers::memoryAllocateInfo();

	vulkanDevice->getDispatch()->GetBufferMemoryRequirements(vulkanDevice->logicalDevice, buffer[0], &memReqs);
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->AllocateMemory(vulkanDevice->logicalDevice, &allocInfo, nullptr, &memory[0]));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->BindBufferMemory(vulkanDevice->logicalDevice, buffer[0], memory[0], 0));

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->AllocateMemory(vulkanDevice->logicalDevice, &allocInfo, nullptr, &memory[1]));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->BindBufferMemory(vulkanDevice->logicalDevice, buffer[1], memory[1], 0));

	// Font texture
	VkImageCreateInfo imageInfo = vks::initializers::imageCreateInfo();
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R8_UNORM;
	imageInfo.extent.width = fontWidth;
	imageInfo.extent.height = fontHeight;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateImage(vulkanDevice->logicalDevice, &imageInfo, nullptr, &image));

	vulkanDevice->getDispatch()->GetImageMemoryRequirements(vulkanDevice->logicalDevice, image, &memReqs);
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->AllocateMemory(vulkanDevice->logicalDevice, &allocInfo, nullptr, &imageMemory));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->BindImageMemory(vulkanDevice->logicalDevice, image, imageMemory, 0));

	// Staging

	struct {
		VkDeviceMemory memory;
		VkBuffer buffer;
	} stagingBuffer;

	VkBufferCreateInfo bufferCreateInfo = vks::initializers::bufferCreateInfo();
	bufferCreateInfo.size = allocInfo.allocationSize;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateBuffer(vulkanDevice->logicalDevice, &bufferCreateInfo, nullptr, &stagingBuffer.buffer));

	// Get memory requirements for the staging buffer (alignment, memory type bits)
	vulkanDevice->getDispatch()->GetBufferMemoryRequirements(vulkanDevice->logicalDevice, stagingBuffer.buffer, &memReqs);

	allocInfo.allocationSize = memReqs.size;
	// Get memory type index for a host visible buffer
	allocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->AllocateMemory(vulkanDevice->logicalDevice, &allocInfo, nullptr, &stagingBuffer.memory));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->BindBufferMemory(vulkanDevice->logicalDevice, stagingBuffer.buffer, stagingBuffer.memory, 0));

	uint8_t *data;
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->MapMemory(vulkanDevice->logicalDevice, stagingBuffer.memory, 0, allocInfo.allocationSize, 0, (void **)&data));
	// Size of the font texture is WIDTH * HEIGHT * 1 byte (only one channel)
	memcpy(data, &font24pixels[0][0], fontWidth * fontHeight);
	vulkanDevice->getDispatch()->UnmapMemory(vulkanDevice->logicalDevice, stagingBuffer.memory);

	// Copy to image

	VkCommandBuffer copyCmd;
	cmdBufAllocateInfo.commandBufferCount = 1;
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->AllocateCommandBuffers(vulkanDevice->logicalDevice, &cmdBufAllocateInfo, &copyCmd));

	device_data->set_device_loader_data(vulkanDevice->logicalDevice, copyCmd);

	VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->BeginCommandBuffer(copyCmd, &cmdBufInfo));

	// Prepare for transfer
	vks::tools::setImageLayout(
		vulkanDevice->logicalDevice,
		copyCmd,
		image,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkBufferImageCopy bufferCopyRegion = {};
	bufferCopyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	bufferCopyRegion.imageSubresource.mipLevel = 0;
	bufferCopyRegion.imageSubresource.layerCount = 1;
	bufferCopyRegion.imageExtent.width = fontWidth;
	bufferCopyRegion.imageExtent.height = fontHeight;
	bufferCopyRegion.imageExtent.depth = 1;

	vulkanDevice->getDispatch()->CmdCopyBufferToImage(
		copyCmd,
		stagingBuffer.buffer,
		image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&bufferCopyRegion
		);

	// Prepare for shader read
	vks::tools::setImageLayout(
		vulkanDevice->logicalDevice,
		copyCmd,
		image,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->EndCommandBuffer(copyCmd));

	VkSubmitInfo submitInfo = vks::initializers::submitInfo();
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &copyCmd;

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->QueueWaitIdle(queue));

	vulkanDevice->getDispatch()->FreeCommandBuffers(vulkanDevice->logicalDevice, commandPool, 1, &copyCmd);
	vulkanDevice->getDispatch()->FreeMemory(vulkanDevice->logicalDevice, stagingBuffer.memory, nullptr);
	vulkanDevice->getDispatch()->DestroyBuffer(vulkanDevice->logicalDevice, stagingBuffer.buffer, nullptr);

	VkImageViewCreateInfo imageViewInfo = vks::initializers::imageViewCreateInfo();
	imageViewInfo.image = image;
	imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	imageViewInfo.format = imageInfo.format;
	imageViewInfo.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,	VK_COMPONENT_SWIZZLE_A };
	imageViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateImageView(vulkanDevice->logicalDevice, &imageViewInfo, nullptr, &view));

	// Sampler
	VkSamplerCreateInfo samplerInfo = vks::initializers::samplerCreateInfo();
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;//VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;//VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;//VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	//samplerInfo.unnormalizedCoordinates = VK_TRUE;
	samplerInfo.compareEnable = VK_FALSE;
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateSampler(vulkanDevice->logicalDevice, &samplerInfo, nullptr, &sampler));

	// Descriptor
	// Font uses a separate descriptor pool
	std::array<VkDescriptorPoolSize, 1> poolSizes;
	poolSizes[0] = vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);

	VkDescriptorPoolCreateInfo descriptorPoolInfo =
		vks::initializers::descriptorPoolCreateInfo(
			static_cast<uint32_t>(poolSizes.size()),
			poolSizes.data(),
			1);

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateDescriptorPool(vulkanDevice->logicalDevice, &descriptorPoolInfo, nullptr, &descriptorPool));

	// Descriptor set layout
	std::array<VkDescriptorSetLayoutBinding, 1> setLayoutBindings;
	setLayoutBindings[0] = vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0);

	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo =
		vks::initializers::descriptorSetLayoutCreateInfo(
			setLayoutBindings.data(),
			static_cast<uint32_t>(setLayoutBindings.size()));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateDescriptorSetLayout(vulkanDevice->logicalDevice, &descriptorSetLayoutInfo, nullptr, &descriptorSetLayout));

	// Pipeline layout
	VkPipelineLayoutCreateInfo pipelineLayoutInfo =
		vks::initializers::pipelineLayoutCreateInfo(
			&descriptorSetLayout,
			1);

	VkPushConstantRange pushConstantRange0 = vks::initializers::pushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float) * 4, 0);
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange0;

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreatePipelineLayout(vulkanDevice->logicalDevice, &pipelineLayoutInfo, nullptr, &pipelineLayout));

	// Descriptor set
	VkDescriptorSetAllocateInfo descriptorSetAllocInfo =
		vks::initializers::descriptorSetAllocateInfo(
			descriptorPool,
			&descriptorSetLayout,
			1);

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->AllocateDescriptorSets(vulkanDevice->logicalDevice, &descriptorSetAllocInfo, &descriptorSet));

	VkDescriptorImageInfo texDescriptor =
		vks::initializers::descriptorImageInfo(
			sampler,
			view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL /*VK_IMAGE_LAYOUT_GENERAL*/); // validation wants VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL

	std::array<VkWriteDescriptorSet, 1> writeDescriptorSets;
	writeDescriptorSets[0] = vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &texDescriptor);
	vulkanDevice->getDispatch()->UpdateDescriptorSets(vulkanDevice->logicalDevice, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);

	// Pipeline cache
	VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {};
	pipelineCacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreatePipelineCache(vulkanDevice->logicalDevice, &pipelineCacheCreateInfo, nullptr, &pipelineCache));
}

// Prepare a separate pipeline for the font rendering decoupled from the main application
void TextOverlay::preparePipeline()
{
	// Enable blending, using alpha from red channel of the font texture (see text.frag)
	VkPipelineColorBlendAttachmentState blendAttachmentState{};
	blendAttachmentState.blendEnable = VK_TRUE;
	blendAttachmentState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;

	VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, VK_FALSE);
	VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE, 0);
	VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
	VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
	VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
	VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
	std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);

	std::array<VkVertexInputBindingDescription, 3> vertexInputBindings = {
		vks::initializers::vertexInputBindingDescription(0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
		vks::initializers::vertexInputBindingDescription(1, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
		vks::initializers::vertexInputBindingDescription(2, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
	};
	std::array<VkVertexInputAttributeDescription, 3> vertexInputAttributes = {
		vks::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32_SFLOAT, 0),					// Location 0: Position
		vks::initializers::vertexInputAttributeDescription(1, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)),	// Location 1: UV
		vks::initializers::vertexInputAttributeDescription(2, 2, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)),	// Location 2: Color
	};

	VkPipelineVertexInputStateCreateInfo vertexInputState = vks::initializers::pipelineVertexInputStateCreateInfo();
	vertexInputState.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
	vertexInputState.pVertexBindingDescriptions = vertexInputBindings.data();
	vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
	vertexInputState.pVertexAttributeDescriptions = vertexInputAttributes.data();

	VkGraphicsPipelineCreateInfo pipelineCreateInfo = vks::initializers::pipelineCreateInfo(pipelineLayout, renderPass, 0);
	pipelineCreateInfo.pVertexInputState = &vertexInputState;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
	pipelineCreateInfo.pRasterizationState = &rasterizationState;
	pipelineCreateInfo.pColorBlendState = &colorBlendState;
	pipelineCreateInfo.pMultisampleState = &multisampleState;
	pipelineCreateInfo.pViewportState = &viewportState;
	pipelineCreateInfo.pDepthStencilState = &depthStencilState;
	pipelineCreateInfo.pDynamicState = &dynamicState;
	pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineCreateInfo.pStages = shaderStages.data();

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateGraphicsPipelines(vulkanDevice->logicalDevice, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipeline));
}

// Prepare a separate render pass for rendering the text as an overlay
void TextOverlay::prepareRenderPass()
{
	VkAttachmentDescription attachments[2] = {};

	// Color attachment
	attachments[0].format = colorFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	// Don't clear the framebuffer (like the renderpass from the example does)
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

/*
	// Depth attachment
	attachments[1].format = depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
*/

	VkAttachmentReference colorReference = {};
	colorReference.attachment = 0;
	colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	//VkAttachmentReference depthReference = {};
	//depthReference.attachment = 1;
	//depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	// Use subpass dependencies for image layout transitions
	VkSubpassDependency subpassDependencies[2] = {};

	// Transition from final to initial (VK_SUBPASS_EXTERNAL refers to all commmands executed outside of the actual renderpass)
	subpassDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	subpassDependencies[0].dstSubpass = 0;
	//subpassDependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	subpassDependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	subpassDependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	subpassDependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	subpassDependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	subpassDependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	// Transition from initial to final
	subpassDependencies[1].srcSubpass = 0;
	subpassDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	subpassDependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	subpassDependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	subpassDependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	subpassDependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	subpassDependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkSubpassDescription subpassDescription = {};
	subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescription.flags = 0;
	subpassDescription.inputAttachmentCount = 0;
	subpassDescription.pInputAttachments = NULL;
	subpassDescription.colorAttachmentCount = 1;
	subpassDescription.pColorAttachments = &colorReference;
	subpassDescription.pResolveAttachments = NULL;
	subpassDescription.pDepthStencilAttachment = NULL;//&depthReference;
	subpassDescription.preserveAttachmentCount = 0;
	subpassDescription.pPreserveAttachments = NULL;

	VkRenderPassCreateInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.pNext = NULL;
	renderPassInfo.attachmentCount = 1;//2;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpassDescription;
	renderPassInfo.dependencyCount = 1;//2;
	renderPassInfo.pDependencies = subpassDependencies;

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->CreateRenderPass(vulkanDevice->logicalDevice, &renderPassInfo, nullptr, &renderPass));
}

// Map buffer 
void TextOverlay::beginTextUpdate()
{
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->MapMemory(vulkanDevice->logicalDevice, memory[bufferIndex], 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
	numLetters = 0;
}

// Add text to the current buffer
// todo : drop shadow? color attribute?
void TextOverlay::addText(std::string text, float x, float y, float scale, TextAlign align)
{
	const uint32_t firstChar = STB_FONT_consolas_bold_24_latin1_FIRST_CHAR;

	if (numLetters >= TEXTOVERLAY_MAX_CHAR_COUNT) {
		printf("text vertex buffer is full! skipping...\n");
		return;
	}

	assert(mapped != nullptr);

	const float charW = (1.5f * scale) / frameBufferWidth;
	const float charH = (1.5f * scale) / frameBufferHeight;

	float fbW = (float)frameBufferWidth;
	float fbH = (float)frameBufferHeight;
	x = (x / fbW * 2.0f) - 1.0f;
	y = (y / fbH * 2.0f) - 1.0f;

	// Calculate text width
	float textWidth = 0;
	uint8_t prev_letter = 0;
	bool skip_triplet = false;
	for (char letter : text)
	{
		//printf("%hhx %c\n", letter, letter);
		if (skip_triplet) {
			skip_triplet = false;
			char tmp[] = { (char)0xE0, (char)prev_letter, letter, 0 };
			fprintf(stderr, "[FIXME] %s: cannot map character to latin1: '%s'\n", __func__, tmp);
			continue;
		}

		if (prev_letter == 0xE0) {
			skip_triplet = true;
			prev_letter = letter;
			continue;
		}

		if ((uint8_t)letter >= 0xC2) {
			prev_letter = letter;
			continue;
		}

		if (prev_letter > 0xC3) {
			char tmp[] = { (char)prev_letter, letter, 0 };
			fprintf(stderr, "[FIXME] %s: cannot map character to latin1: '%s'\n", __func__, tmp);
			continue;
		}

		// utf-8 to latin1 ext
		if (prev_letter == 0xC2) {
			//nuffin
		}
		else if (prev_letter == 0xC3)
			letter = letter + 64;
		else if(prev_letter >= 0xC2 || (uint32_t)(letter & 0xFF) - firstChar >= STB_FONT_consolas_bold_24_latin1_NUM_CHARS)
			continue;
		prev_letter = letter;

		stb_fontchar *charData = &stbFontData[(uint32_t)(letter & 0xFF) - firstChar];
		textWidth += charData->advance * charW;
	}

	switch (align)
	{
		case alignRight:
			x -= textWidth;
			break;
		case alignCenter:
			x -= textWidth / 2.0f;
			break;
		default:
			break;
	}

	skip_triplet = false;
	prev_letter = 0;

	// Generate a uv mapped quad per char in the new text
	for (auto letter : text)
	{
		if (skip_triplet) {
			skip_triplet = false;
			continue;
		}

		if (prev_letter == 0xE0) {
			skip_triplet = true;
			prev_letter = 0;
			continue;
		}

		if ((uint8_t)letter >= 0xC2) {
			prev_letter = letter;
			continue;
		}

		// utf-8 to latin1 ext
		if (prev_letter == 0xC2){
			// nuffin
		}
		else if (prev_letter == 0xC3){
			letter = letter + 64;
		}
		else if (prev_letter >= 0xC2 || (uint32_t)(letter & 0xFF) - firstChar >= STB_FONT_consolas_bold_24_latin1_NUM_CHARS)
			continue;
		prev_letter = 0;

		stb_fontchar *charData = &stbFontData[(uint32_t)(letter & 0xFF) - firstChar];

		mapped->pos.x = (x + (float)charData->x0 * charW * scale);// - charW;
		mapped->pos.y = (y + (float)charData->y0 * charH * scale);// - charH;
		mapped->uv.x = charData->s0;
		mapped->uv.y = charData->t0;
		mapped->color.x = fontColor[0];
		mapped->color.y = fontColor[1];
		mapped->color.z = fontColor[2];
		mapped++;

		mapped->pos.x = (x + (float)charData->x1 * charW * scale);// + charW;
		mapped->pos.y = (y + (float)charData->y0 * charH * scale);// - charH;
		mapped->uv.x = charData->s1;
		mapped->uv.y = charData->t0;
		mapped->color.x = fontColor[0];
		mapped->color.y = fontColor[1];
		mapped->color.z = fontColor[2];
		mapped++;

		mapped->pos.x = (x + (float)charData->x0 * charW * scale);// - charW;
		mapped->pos.y = (y + (float)charData->y1 * charH * scale);// + charH;
		mapped->uv.x = charData->s0;
		mapped->uv.y = charData->t1;
		mapped->color.x = fontColor[0];
		mapped->color.y = fontColor[1];
		mapped->color.z = fontColor[2];
		mapped++;

		mapped->pos.x = (x + (float)charData->x1 * charW * scale);// + charW;
		mapped->pos.y = (y + (float)charData->y1 * charH * scale);// + charH;
		mapped->uv.x = charData->s1;
		mapped->uv.y = charData->t1;
		mapped->color.x = fontColor[0];
		mapped->color.y = fontColor[1];
		mapped->color.z = fontColor[2];
		mapped++;

		x += charData->advance * charW * scale;

		numLetters++;
		if (numLetters >= TEXTOVERLAY_MAX_CHAR_COUNT)
			break;
	}
}

// Unmap buffer and update command buffers
void TextOverlay::endTextUpdate()
{
	vulkanDevice->getDispatch()->UnmapMemory(vulkanDevice->logicalDevice, memory[bufferIndex]);
	mapped = nullptr;

	std::lock_guard<std::mutex> l(biMutex);
	renderIndex = bufferIndex;
	bufferIndex = 1 - renderIndex;
}

// Needs to be called by the application
void TextOverlay::updateCommandBuffers(uint32_t i, VkImageMemoryBarrier imb)
{
	std::lock_guard<std::mutex> l(biMutex);
	VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

	VkClearValue clearValues[2];
	clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };

	VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
	renderPassBeginInfo.renderPass = renderPass;
	renderPassBeginInfo.renderArea.extent.width = frameBufferWidth;
	renderPassBeginInfo.renderArea.extent.height = frameBufferHeight;
	renderPassBeginInfo.clearValueCount = 0;//2;
	renderPassBeginInfo.pClearValues = clearValues;

	//for (uint32_t i = 0; i < cmdBuffers.size(); ++i)
	{
		//printf("updateCommandBuffers %d fb %p\n", i, *frameBuffers[i]);
		renderPassBeginInfo.framebuffer = *frameBuffers[i];

		VK_CHECK_RESULT(vulkanDevice->getDispatch()->BeginCommandBuffer(cmdBuffers[i], &cmdBufInfo));

		vulkanDevice->getDispatch()->CmdPipelineBarrier(cmdBuffers[i],
										  VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
										  VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
										  0,          /* dependency flags */
										  0, nullptr, /* memory barriers */
										  0, nullptr, /* buffer memory barriers */
										  1, &imb);   /* image memory barriers */
		vulkanDevice->getDispatch()->CmdBeginRenderPass(cmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = vks::initializers::viewport((float)frameBufferWidth, (float)frameBufferHeight, 0.0f, 1.0f);
		vulkanDevice->getDispatch()->CmdSetViewport(cmdBuffers[i], 0, 1, &viewport);

		VkRect2D scissor = vks::initializers::rect2D(frameBufferWidth, frameBufferHeight, 0, 0);
		vulkanDevice->getDispatch()->CmdSetScissor(cmdBuffers[i], 0, 1, &scissor);

		vulkanDevice->getDispatch()->CmdBindPipeline(cmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vulkanDevice->getDispatch()->CmdBindDescriptorSets(cmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, NULL);

		VkDeviceSize offsets[] = { 0 };
		vulkanDevice->getDispatch()->CmdBindVertexBuffers(cmdBuffers[i], 0, 1, &buffer[renderIndex], offsets);
		vulkanDevice->getDispatch()->CmdBindVertexBuffers(cmdBuffers[i], 1, 1, &buffer[renderIndex], offsets);
		vulkanDevice->getDispatch()->CmdBindVertexBuffers(cmdBuffers[i], 2, 1, &buffer[renderIndex], offsets);

		vulkanDevice->getDispatch()->CmdPushConstants(cmdBuffers[i], pipelineLayout,
			VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float) * 0, sizeof(float) * 4, fontColor);

		numLetters = std::min(numLetters, TEXTOVERLAY_MAX_CHAR_COUNT);
		for (uint32_t j = 0; j < numLetters; j++)
		{
			vulkanDevice->getDispatch()->CmdDraw(cmdBuffers[i], 4, 1, j * 4, 0);
		}

		vulkanDevice->getDispatch()->CmdEndRenderPass(cmdBuffers[i]);

		VK_CHECK_RESULT(vulkanDevice->getDispatch()->EndCommandBuffer(cmdBuffers[i]));
	}
}

// Submit the text command buffers to a queue
// Does a queue wait idle
void TextOverlay::submit(VkQueue queue, uint32_t bufferindex, VkSubmitInfo submitInfo, VkFence fence)
{
	//VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	//submitInfo.commandBufferCount = visible ? 1 : 0; // toggle rendering, but call VkQueueSubmit so present semaphore gets signalled
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmdBuffers[bufferindex];

	VK_CHECK_RESULT(vulkanDevice->getDispatch()->QueueSubmit(queue, 1, &submitInfo, fence));
	VK_CHECK_RESULT(vulkanDevice->getDispatch()->WaitForFences(vulkanDevice->logicalDevice, 1, &fence, VK_TRUE, UINT64_MAX));
}
