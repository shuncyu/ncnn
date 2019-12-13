// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "pipeline.h"
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <string>
#include "mat.h"
#include "option.h"

#if __ANDROID_API__ >= 26
#include <android/hardware_buffer.h>
#endif // __ANDROID_API__ >= 26

namespace ncnn {

#if NCNN_VULKAN
Pipeline::Pipeline(const VulkanDevice* _vkdev) : vkdev(_vkdev)
{
    local_shader_module = 0;

    descriptorset_layout = 0;
    pipeline_layout = 0;
    pipeline = 0;
    descriptor_update_template = 0;

    local_size_x = 1;
    local_size_y = 1;
    local_size_z = 1;
}

Pipeline::~Pipeline()
{
    destroy();
}

int Pipeline::create(const uint32_t* spv_data, size_t spv_data_size, const char* entry_name, const std::vector<vk_specialization_type>& specializations, int binding_count, int push_constant_count)
{
    local_shader_module = vkdev->compile_shader_module(spv_data, spv_data_size);

//     fprintf(stderr, "local_shader_module %p %s created\n", local_shader_module, entry_name);

    return create(local_shader_module, entry_name, specializations, binding_count, push_constant_count);
}

int Pipeline::create(VkShaderModule shader_module, const char* entry_name, const std::vector<vk_specialization_type>& specializations, int binding_count, int push_constant_count)
{
    create_descriptorset_layout(binding_count);

    create_pipeline_layout(push_constant_count);

    create_pipeline(shader_module, entry_name, specializations);

    if (vkdev->info.support_VK_KHR_descriptor_update_template)
    {
        create_descriptor_update_template(binding_count);
    }

    return 0;
}

int Pipeline::create(const char* _name, const Option& opt, const std::vector<vk_specialization_type>& specializations, int binding_count, int push_constant_count)
{
    std::string name = _name;

    if (vkdev->info.support_fp16_arithmetic && opt.use_fp16_arithmetic)
    {
        name += "_fp16a";
    }
    else if (vkdev->info.support_fp16_storage && opt.use_fp16_storage)
    {
        name += "_fp16s";
    }
    else if (vkdev->info.support_fp16_packed && opt.use_fp16_packed)
    {
        name += "_fp16p";
    }

    VkShaderModule shader_module = vkdev->get_shader_module(name.c_str());

    return create(shader_module, name.c_str(), specializations, binding_count, push_constant_count);
}

void Pipeline::destroy()
{
    if (vkdev->info.support_VK_KHR_descriptor_update_template)
    {
        if (descriptor_update_template)
        {
            vkdev->vkDestroyDescriptorUpdateTemplateKHR(vkdev->vkdevice(), descriptor_update_template, 0);
            descriptor_update_template = 0;
        }
    }

    if (pipeline)
    {
        vkDestroyPipeline(vkdev->vkdevice(), pipeline, 0);
        pipeline = 0;
    }

    if (pipeline_layout)
    {
        vkDestroyPipelineLayout(vkdev->vkdevice(), pipeline_layout, 0);
        pipeline_layout = 0;
    }

    if (descriptorset_layout)
    {
        vkDestroyDescriptorSetLayout(vkdev->vkdevice(), descriptorset_layout, 0);
        descriptorset_layout = 0;
    }

    if (local_shader_module)
    {
        vkDestroyShaderModule(vkdev->vkdevice(), local_shader_module, 0);
        local_shader_module = 0;
    }
}

void Pipeline::set_optimal_local_size_xyz(int w, int h, int c)
{
    if (c > 0)
    {
        local_size_z = vkdev->info.max_workgroup_size[2];
        while ((uint32_t)c < local_size_z)
        {
            local_size_z /= 2;
        }
    }
    else
    {
        local_size_z = std::min((uint32_t)128, vkdev->info.max_workgroup_size[2]);
    }

    uint32_t max_local_size_xy = vkdev->info.max_workgroup_invocations / local_size_z;

    if (h == w || (h < 0 && w < 0))
    {
        uint32_t local_size_xy = sqrt(max_local_size_xy);
        uint32_t local_size_xy_prefer = 128;
        while (local_size_xy < local_size_xy_prefer)
        {
            local_size_xy_prefer /= 2;
        }
        local_size_x = local_size_xy_prefer;
        local_size_y = local_size_xy_prefer;
    }
    if (h > 0 && w > 0)
    {
        if (h > w)
        {
            float ps = h / (float)w;
            float local_size_xy = sqrt(max_local_size_xy / ps);
            local_size_y = local_size_xy * ps;
            local_size_x = std::max((uint32_t)local_size_xy, (uint32_t)1);
        }
        else
        {
            float ps = w / (float)h;
            float local_size_xy = sqrt(max_local_size_xy / ps);
            local_size_y = std::max((uint32_t)local_size_xy, (uint32_t)1);
            local_size_x = local_size_xy * ps;
        }

        uint32_t local_size_y_prefer = std::min((uint32_t)128, vkdev->info.max_workgroup_size[1]);
        while (local_size_y < local_size_y_prefer)
        {
            local_size_y_prefer /= 2;
        }

        uint32_t local_size_x_prefer = std::min((uint32_t)128, vkdev->info.max_workgroup_size[0]);
        while (local_size_x < local_size_x_prefer)
        {
            local_size_x_prefer /= 2;
        }

        local_size_y = local_size_y_prefer;
        local_size_x = local_size_x_prefer;
    }
    else if (h > 0)
    {
        local_size_y = std::min(max_local_size_xy, vkdev->info.max_workgroup_size[1]);
        while ((uint32_t)h < local_size_y)
        {
            local_size_y /= 2;
        }

        uint32_t max_local_size_x = max_local_size_xy / local_size_y;
        local_size_x = std::min(max_local_size_x, vkdev->info.max_workgroup_size[0]);
    }
    else if (w > 0)
    {
        local_size_x = std::min(max_local_size_xy, vkdev->info.max_workgroup_size[0]);
        while ((uint32_t)w < local_size_x)
        {
            local_size_x /= 2;
        }

        uint32_t max_local_size_y = max_local_size_xy / local_size_x;
        local_size_y = std::min(max_local_size_y, vkdev->info.max_workgroup_size[1]);
    }

//     fprintf(stderr, "local size = %d %d %d\n", local_size_x, local_size_y, local_size_z);
}

void Pipeline::set_local_size_xyz(int w, int h, int c)
{
    local_size_x = w;
    local_size_y = h;
    local_size_z = c;
}

int Pipeline::create_descriptorset_layout(int binding_count)
{
    if (binding_count == 0)
    {
        descriptorset_layout = 0;
        return 0;
    }

    std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings(binding_count);
    for (int i=0; i<binding_count; i++)
    {
        descriptorSetLayoutBindings[i].binding = i;
        descriptorSetLayoutBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorSetLayoutBindings[i].descriptorCount = 1;
        descriptorSetLayoutBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        descriptorSetLayoutBindings[i].pImmutableSamplers = 0;
    }

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.pNext = 0;
    descriptorSetLayoutCreateInfo.flags = 0;
    descriptorSetLayoutCreateInfo.bindingCount = binding_count;
    descriptorSetLayoutCreateInfo.pBindings = descriptorSetLayoutBindings.data();

    if (vkdev->info.support_VK_KHR_push_descriptor)
    {
        descriptorSetLayoutCreateInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    }

    VkResult ret = vkCreateDescriptorSetLayout(vkdev->vkdevice(), &descriptorSetLayoutCreateInfo, 0, &descriptorset_layout);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateDescriptorSetLayout failed %d\n", ret);
        return -1;
    }

    return 0;
}

int Pipeline::create_pipeline_layout(int push_constant_count)
{
    VkPushConstantRange pushConstantRange;
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(int) * push_constant_count;

    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCreateInfo.pNext = 0;
    pipelineLayoutCreateInfo.flags = 0;

    if (descriptorset_layout)
    {
    pipelineLayoutCreateInfo.setLayoutCount = 1;
    pipelineLayoutCreateInfo.pSetLayouts = &descriptorset_layout;
    }
    else
    {
    pipelineLayoutCreateInfo.setLayoutCount = 0;
    pipelineLayoutCreateInfo.pSetLayouts = 0;
    }

    if (push_constant_count > 0)
    {
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
    }
    else
    {
    pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pipelineLayoutCreateInfo.pPushConstantRanges = 0;
    }

    VkResult ret = vkCreatePipelineLayout(vkdev->vkdevice(), &pipelineLayoutCreateInfo, 0, &pipeline_layout);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreatePipelineLayout failed %d\n", ret);
        return -1;
    }

    return 0;
}

int Pipeline::create_pipeline(VkShaderModule shader_module, const char* entry_name, const std::vector<vk_specialization_type>& specializations)
{
    const int specialization_count = specializations.size();

    // +3 for local_size_xyz
    std::vector<VkSpecializationMapEntry> specializationMapEntries;
    specializationMapEntries.resize(specialization_count + 3);

    for (int i=0; i<specialization_count; i++)
    {
        specializationMapEntries[i].constantID = i;
        specializationMapEntries[i].offset = i * sizeof(vk_specialization_type);
        specializationMapEntries[i].size = sizeof(vk_specialization_type);
    }

    std::vector<vk_specialization_type> specialization_data = specializations;

    // append local_size_xyz specialization
    {
        VkSpecializationMapEntry* local_size_xyz_entries = specializationMapEntries.data() + specialization_count;

        local_size_xyz_entries[0].constantID = 233;
        local_size_xyz_entries[0].offset = (specialization_count+0) * sizeof(vk_specialization_type);
        local_size_xyz_entries[0].size = sizeof(vk_specialization_type);

        local_size_xyz_entries[1].constantID = 234;
        local_size_xyz_entries[1].offset = (specialization_count+1) * sizeof(vk_specialization_type);
        local_size_xyz_entries[1].size = sizeof(vk_specialization_type);

        local_size_xyz_entries[2].constantID = 235;
        local_size_xyz_entries[2].offset = (specialization_count+2) * sizeof(vk_specialization_type);
        local_size_xyz_entries[2].size = sizeof(vk_specialization_type);

        specialization_data.resize(specialization_count + 3);
        specialization_data[ specialization_count+0 ].u32 = local_size_x;
        specialization_data[ specialization_count+1 ].u32 = local_size_y;
        specialization_data[ specialization_count+2 ].u32 = local_size_z;
    }

    VkSpecializationInfo specializationInfo;
    specializationInfo.mapEntryCount = specializationMapEntries.size();
    specializationInfo.pMapEntries = specializationMapEntries.data();
    specializationInfo.dataSize = specialization_data.size() * sizeof(vk_specialization_type);
    specializationInfo.pData = specialization_data.data();

    VkPipelineShaderStageCreateInfo pipelineShaderStageCreateInfo;
    pipelineShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineShaderStageCreateInfo.pNext = 0;
    pipelineShaderStageCreateInfo.flags = 0;
    pipelineShaderStageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineShaderStageCreateInfo.module = shader_module;
    pipelineShaderStageCreateInfo.pName = entry_name;
    pipelineShaderStageCreateInfo.pSpecializationInfo = &specializationInfo;

    VkComputePipelineCreateInfo computePipelineCreateInfo;
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = 0;
    computePipelineCreateInfo.flags = 0;
    computePipelineCreateInfo.stage = pipelineShaderStageCreateInfo;
    computePipelineCreateInfo.layout = pipeline_layout;
    computePipelineCreateInfo.basePipelineHandle = 0;
    computePipelineCreateInfo.basePipelineIndex = 0;

    VkResult ret = vkCreateComputePipelines(vkdev->vkdevice(), 0, 1, &computePipelineCreateInfo, 0, &pipeline);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateComputePipelines failed %d\n", ret);
        return -1;
    }

    return 0;
}

int Pipeline::create_descriptor_update_template(int binding_count)
{
    if (binding_count == 0)
    {
        descriptor_update_template = 0;
        return 0;
    }

    std::vector<VkDescriptorUpdateTemplateEntryKHR> descriptorUpdateTemplateEntries(binding_count);
    for (int i=0; i<binding_count; i++)// TODO do not update weights
    {
        descriptorUpdateTemplateEntries[i].dstBinding = i;
        descriptorUpdateTemplateEntries[i].dstArrayElement = 0;
        descriptorUpdateTemplateEntries[i].descriptorCount = 1;
        descriptorUpdateTemplateEntries[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorUpdateTemplateEntries[i].offset = i * sizeof(VkDescriptorBufferInfo);
        descriptorUpdateTemplateEntries[i].stride = sizeof(VkDescriptorBufferInfo);
    }

    VkDescriptorUpdateTemplateCreateInfoKHR descriptorUpdateTemplateCreateInfo;
    descriptorUpdateTemplateCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO_KHR;
    descriptorUpdateTemplateCreateInfo.pNext = 0;
    descriptorUpdateTemplateCreateInfo.flags = 0;
    descriptorUpdateTemplateCreateInfo.descriptorUpdateEntryCount = binding_count;// TODO do not update weights
    descriptorUpdateTemplateCreateInfo.pDescriptorUpdateEntries = descriptorUpdateTemplateEntries.data();
    if (vkdev->info.support_VK_KHR_push_descriptor)
    {
    descriptorUpdateTemplateCreateInfo.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    }
    else
    {
    descriptorUpdateTemplateCreateInfo.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET_KHR;
    }
    // descriptorSetLayout should be ignored if VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
    // FIXME HACK WARNING TODO NOTE but crash on radv if set NULL  :(
    descriptorUpdateTemplateCreateInfo.descriptorSetLayout = descriptorset_layout;
    descriptorUpdateTemplateCreateInfo.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    descriptorUpdateTemplateCreateInfo.pipelineLayout = pipeline_layout;
    descriptorUpdateTemplateCreateInfo.set = 0;

    VkResult ret = vkdev->vkCreateDescriptorUpdateTemplateKHR(vkdev->vkdevice(), &descriptorUpdateTemplateCreateInfo, 0, &descriptor_update_template);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateDescriptorUpdateTemplateKHR failed %d\n", ret);
        return -1;
    }

    return 0;
}

#if __ANDROID_API__ >= 26
ImportAndroidHardwareBufferPipeline::ImportAndroidHardwareBufferPipeline(const VulkanDevice* _vkdev) : Pipeline(_vkdev)
{
    samplerYcbcrConversion = 0;
    sampler = 0;
}

ImportAndroidHardwareBufferPipeline::~ImportAndroidHardwareBufferPipeline()
{
    destroy();
}

int ImportAndroidHardwareBufferPipeline::create(AHardwareBuffer* hb, int _type_to, int _rotate_from, const Option& opt)
{
    AHardwareBuffer_Desc bufferDesc;
    AHardwareBuffer_describe(hb, &bufferDesc);

    w = bufferDesc.width;
    h = bufferDesc.height;
    type_to = _type_to;
    rotate_from = _rotate_from;

    if (rotate_from < 5) // 1 2 3 4
    {
        outw = w;
        outh = h;
    }
    else // 5 6 7 8
    {
        outw = h;
        outh = w;
    }

    if (type_to == 1 || type_to == 2)
    {
        outc = 3;
        out_elemsize = vkdev->info.support_fp16_storage && opt.use_fp16_storage ? 2u : 4u;
        out_elempack = 1;
    }
    else if (type_to == 3)
    {
        outc = 1;
        out_elemsize = vkdev->info.support_fp16_storage && opt.use_fp16_storage ? 2u : 4u;
        out_elempack = 1;
    }
    else // if (type_to == 4)
    {
        outc = 4;
        out_elemsize = ((vkdev->info.support_fp16_packed && opt.use_fp16_packed) || (vkdev->info.support_fp16_storage && opt.use_fp16_storage)) ? 8u : 16u;
        out_elempack = 4;
    }

    bufferFormatProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    bufferFormatProperties.pNext = 0;

    bufferProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    bufferProperties.pNext = &bufferFormatProperties;

    VkResult ret = vkGetAndroidHardwareBufferPropertiesANDROID(vkdev->vkdevice(), hb, &bufferProperties);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkGetAndroidHardwareBufferPropertiesANDROID failed %d\n", ret);
        return -1;
    }

    set_local_size_xyz(8, 8, 1);

    std::vector<vk_specialization_type> specializations(4);
    specializations[0].i = outw;
    specializations[1].i = outh;
    specializations[2].i = type_to;
    specializations[3].i = rotate_from;

    create_sampler();

    create_descriptorset_layout();

    create_pipeline_layout(0);

    std::string name = "convert_ycbcr";

    if (vkdev->info.support_fp16_arithmetic && opt.use_fp16_arithmetic)
    {
        name += "_fp16a";
    }
    else if (vkdev->info.support_fp16_storage && opt.use_fp16_storage)
    {
        name += "_fp16s";
    }
    else if (vkdev->info.support_fp16_packed && opt.use_fp16_packed)
    {
        name += "_fp16p";
    }

    VkShaderModule shader_module = vkdev->get_shader_module(name.c_str());

    create_pipeline(shader_module, name.c_str(), specializations);

    if (vkdev->info.support_VK_KHR_descriptor_update_template)
    {
        create_descriptor_update_template();
    }

    return 0;
}

void ImportAndroidHardwareBufferPipeline::destroy()
{
    if (sampler)
    {
        vkDestroySampler(vkdev->vkdevice(), sampler, 0);
        sampler = 0;
    }

    if (samplerYcbcrConversion)
    {
        vkdev->vkDestroySamplerYcbcrConversionKHR(vkdev->vkdevice(), samplerYcbcrConversion, 0);
        samplerYcbcrConversion = 0;
    }

    Pipeline::destroy();
}

int ImportAndroidHardwareBufferPipeline::create_image_memory_imageview(AHardwareBuffer* hb, VkImage* image, VkDeviceMemory* memory, VkImageView* imageView)
{
    VkExternalFormatANDROID externalFormat;
    externalFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    externalFormat.pNext = 0;
    externalFormat.externalFormat = bufferFormatProperties.externalFormat;

    VkExternalMemoryImageCreateInfo externalMemoryImageCreateInfo;
    externalMemoryImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
    externalMemoryImageCreateInfo.pNext = &externalFormat,
    externalMemoryImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageCreateInfo;
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    imageCreateInfo.pNext = &externalMemoryImageCreateInfo;
    imageCreateInfo.flags = 0;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_UNDEFINED;
    imageCreateInfo.extent.width = w;
    imageCreateInfo.extent.height = h;
    imageCreateInfo.extent.depth = 1;
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.queueFamilyIndexCount = 0;
    imageCreateInfo.pQueueFamilyIndices = 0;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    *image = 0;
    VkResult ret = vkCreateImage(vkdev->vkdevice(), &imageCreateInfo, 0, image);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateImage failed %d\n", ret);
        return -1;
    }

    VkImportAndroidHardwareBufferInfoANDROID importAndroidHardwareBufferInfo;
    importAndroidHardwareBufferInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importAndroidHardwareBufferInfo.pNext = 0;
    importAndroidHardwareBufferInfo.buffer = hb;

    VkMemoryDedicatedAllocateInfo memoryDedicatedAllocateInfo;
    memoryDedicatedAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memoryDedicatedAllocateInfo.pNext = &importAndroidHardwareBufferInfo;
    memoryDedicatedAllocateInfo.image = *image;
    memoryDedicatedAllocateInfo.buffer = VK_NULL_HANDLE;

    VkMemoryAllocateInfo memoryAllocateInfo;
    memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryAllocateInfo.pNext = &memoryDedicatedAllocateInfo;
    memoryAllocateInfo.allocationSize = bufferProperties.allocationSize;
    memoryAllocateInfo.memoryTypeIndex = vkdev->find_memory_index(bufferProperties.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    *memory = 0;
    ret = vkAllocateMemory(vkdev->vkdevice(), &memoryAllocateInfo, 0, memory);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkAllocateMemory failed %d\n", ret);
        return -1;
    }

    VkBindImageMemoryInfo bindImageMemoryInfo;
    bindImageMemoryInfo.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bindImageMemoryInfo.pNext = 0;
    bindImageMemoryInfo.image = *image;
    bindImageMemoryInfo.memory = *memory;
    bindImageMemoryInfo.memoryOffset = 0;
    ret = vkdev->vkBindImageMemory2KHR(vkdev->vkdevice(), 1, &bindImageMemoryInfo);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkBindImageMemory2KHR failed %d\n", ret);
        return -1;
    }

    VkSamplerYcbcrConversionInfoKHR samplerYcbcrConversionInfo;
    samplerYcbcrConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO_KHR;
    samplerYcbcrConversionInfo.pNext = &externalFormat;
    samplerYcbcrConversionInfo.conversion = samplerYcbcrConversion;

    VkImageViewCreateInfo imageViewCreateInfo;
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.pNext = &samplerYcbcrConversionInfo;
    imageViewCreateInfo.flags = 0;
    imageViewCreateInfo.image = *image;
    imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewCreateInfo.format = VK_FORMAT_UNDEFINED;
    imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = 1;

    *imageView = 0;
    ret = vkCreateImageView(vkdev->vkdevice(), &imageViewCreateInfo, 0, imageView);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateImageView failed %d\n", ret);
        return -1;
    }

    return 0;
}

int ImportAndroidHardwareBufferPipeline::create_sampler()
{
    VkExternalFormatANDROID externalFormat;
    externalFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    externalFormat.pNext = 0;
    externalFormat.externalFormat = bufferFormatProperties.externalFormat;

    VkSamplerYcbcrConversionCreateInfoKHR samplerYcbcrConversionCreateInfo;
    samplerYcbcrConversionCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO_KHR;
    samplerYcbcrConversionCreateInfo.pNext = &externalFormat;
    samplerYcbcrConversionCreateInfo.format = VK_FORMAT_UNDEFINED;
    samplerYcbcrConversionCreateInfo.ycbcrModel = bufferFormatProperties.suggestedYcbcrModel;
    samplerYcbcrConversionCreateInfo.ycbcrRange = bufferFormatProperties.suggestedYcbcrRange;
    samplerYcbcrConversionCreateInfo.components = bufferFormatProperties.samplerYcbcrConversionComponents;
    samplerYcbcrConversionCreateInfo.xChromaOffset = bufferFormatProperties.suggestedXChromaOffset;
    samplerYcbcrConversionCreateInfo.yChromaOffset = bufferFormatProperties.suggestedYChromaOffset;
    samplerYcbcrConversionCreateInfo.chromaFilter = VK_FILTER_NEAREST;
    samplerYcbcrConversionCreateInfo.forceExplicitReconstruction = VK_FALSE;

    VkResult ret = vkdev->vkCreateSamplerYcbcrConversionKHR(vkdev->vkdevice(), &samplerYcbcrConversionCreateInfo, 0, &samplerYcbcrConversion);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateSamplerYcbcrConversionKHR failed %d\n", ret);
        return -1;
    }

    VkSamplerYcbcrConversionInfoKHR samplerYcbcrConversionInfo;
    samplerYcbcrConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO_KHR;
    samplerYcbcrConversionInfo.pNext = &externalFormat;
    samplerYcbcrConversionInfo.conversion = samplerYcbcrConversion;

    VkSamplerCreateInfo samplerCreateInfo;
    samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCreateInfo.pNext = &samplerYcbcrConversionInfo;
    samplerCreateInfo.magFilter = VK_FILTER_NEAREST;
    samplerCreateInfo.minFilter = VK_FILTER_NEAREST;
    samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.mipLodBias = 0.0f;
    samplerCreateInfo.anisotropyEnable = VK_FALSE;
    samplerCreateInfo.maxAnisotropy = 1;
    samplerCreateInfo.compareEnable = VK_FALSE;
    samplerCreateInfo.compareOp = VK_COMPARE_OP_NEVER;
    samplerCreateInfo.minLod = 0.0f;
    samplerCreateInfo.maxLod = 0.0f;
    samplerCreateInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;//VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; FIXME
    samplerCreateInfo.unnormalizedCoordinates = VK_TRUE;//VK_FALSE; FIXME ?

    ret = vkCreateSampler(vkdev->vkdevice(), &samplerCreateInfo, 0, &sampler);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateSampler failed %d\n", ret);
        return -1;
    }

    return 0;
}

int ImportAndroidHardwareBufferPipeline::create_descriptorset_layout()
{
    VkDescriptorSetLayoutBinding descriptorSetLayoutBindings[3];
    descriptorSetLayoutBindings[0].binding = 0;
    descriptorSetLayoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorSetLayoutBindings[0].descriptorCount = 1;
    descriptorSetLayoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptorSetLayoutBindings[0].pImmutableSamplers = &sampler;
    descriptorSetLayoutBindings[1].binding = 1;
    descriptorSetLayoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorSetLayoutBindings[1].descriptorCount = 1;
    descriptorSetLayoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptorSetLayoutBindings[1].pImmutableSamplers = 0;
    descriptorSetLayoutBindings[2].binding = 2;
    descriptorSetLayoutBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorSetLayoutBindings[2].descriptorCount = 1;
    descriptorSetLayoutBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptorSetLayoutBindings[2].pImmutableSamplers = 0;

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.pNext = 0;
    descriptorSetLayoutCreateInfo.flags = 0;
    descriptorSetLayoutCreateInfo.bindingCount = 3;
    descriptorSetLayoutCreateInfo.pBindings = descriptorSetLayoutBindings;

    if (vkdev->info.support_VK_KHR_push_descriptor)
    {
        descriptorSetLayoutCreateInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    }

    VkResult ret = vkCreateDescriptorSetLayout(vkdev->vkdevice(), &descriptorSetLayoutCreateInfo, 0, &descriptorset_layout);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateDescriptorSetLayout failed %d\n", ret);
        return -1;
    }

    return 0;
}

int ImportAndroidHardwareBufferPipeline::create_descriptor_update_template()
{
    VkDescriptorUpdateTemplateEntryKHR descriptorUpdateTemplateEntries[3];
    descriptorUpdateTemplateEntries[0].dstBinding = 0;
    descriptorUpdateTemplateEntries[0].dstArrayElement = 0;
    descriptorUpdateTemplateEntries[0].descriptorCount = 1;
    descriptorUpdateTemplateEntries[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorUpdateTemplateEntries[0].offset = 0;
    descriptorUpdateTemplateEntries[0].stride = sizeof(VkDescriptorImageInfo);
    descriptorUpdateTemplateEntries[1].dstBinding = 1;
    descriptorUpdateTemplateEntries[1].dstArrayElement = 0;
    descriptorUpdateTemplateEntries[1].descriptorCount = 1;
    descriptorUpdateTemplateEntries[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorUpdateTemplateEntries[1].offset = sizeof(VkDescriptorImageInfo);
    descriptorUpdateTemplateEntries[1].stride = sizeof(VkDescriptorBufferInfo);
    descriptorUpdateTemplateEntries[2].dstBinding = 2;
    descriptorUpdateTemplateEntries[2].dstArrayElement = 0;
    descriptorUpdateTemplateEntries[2].descriptorCount = 1;
    descriptorUpdateTemplateEntries[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorUpdateTemplateEntries[2].offset = sizeof(VkDescriptorImageInfo) + sizeof(VkDescriptorBufferInfo);
    descriptorUpdateTemplateEntries[2].stride = sizeof(VkDescriptorBufferInfo);

    VkDescriptorUpdateTemplateCreateInfoKHR descriptorUpdateTemplateCreateInfo;
    descriptorUpdateTemplateCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO_KHR;
    descriptorUpdateTemplateCreateInfo.pNext = 0;
    descriptorUpdateTemplateCreateInfo.flags = 0;
    descriptorUpdateTemplateCreateInfo.descriptorUpdateEntryCount = 3;
    descriptorUpdateTemplateCreateInfo.pDescriptorUpdateEntries = descriptorUpdateTemplateEntries;
    if (vkdev->info.support_VK_KHR_push_descriptor)
    {
        descriptorUpdateTemplateCreateInfo.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    }
    else
    {
        descriptorUpdateTemplateCreateInfo.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET_KHR;
    }
    // descriptorSetLayout should be ignored if VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
    // FIXME HACK WARNING TODO NOTE but crash on radv if set NULL  :(
    descriptorUpdateTemplateCreateInfo.descriptorSetLayout = descriptorset_layout;
    descriptorUpdateTemplateCreateInfo.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    descriptorUpdateTemplateCreateInfo.pipelineLayout = pipeline_layout;
    descriptorUpdateTemplateCreateInfo.set = 0;

    VkResult ret = vkdev->vkCreateDescriptorUpdateTemplateKHR(vkdev->vkdevice(), &descriptorUpdateTemplateCreateInfo, 0, &descriptor_update_template);
    if (ret != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateDescriptorUpdateTemplateKHR failed %d\n", ret);
        return -1;
    }

    return 0;
}
#endif // __ANDROID_API__ >= 26
#endif // NCNN_VULKAN

} // namespace ncnn