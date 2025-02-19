#include <R2/VKCore.hpp>
#include <R2/R2.hpp>
#include <RenderPassCache.hpp>
#include <volk.h>
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#endif
#include <vk_mem_alloc.h>
#include <vector>
#include <stdint.h>
#include <stdio.h>

namespace R2::VK
{
    VkBool32 vulkanDebugMessageCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageTypes,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        Core* r = static_cast<Core*>(pUserData);
        // Validation layers seem to have a bug where having a pipeline bound before beginning dynamic rendering that
        // has the incorrect attachments will cause validation errors when changing to a correct pipeline
        if (pCallbackData->messageIdNumber == 296975921 || pCallbackData->messageIdNumber == -690520546 ||
            pCallbackData->messageIdNumber == 1813430196)
            return VK_FALSE;

        if (r->GetDebugOutputReceiver())
            r->GetDebugOutputReceiver()->DebugMessage(pCallbackData->pMessage);
        else
            printf("vk: %s\n", pCallbackData->pMessage);
        return VK_FALSE;
    }

    void Core::createInstance(bool enableValidation, const char** instanceExts)
    {
        VKCHECK(volkInitialize());
        VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pEngineName = "Worlds Engine";
        appInfo.pApplicationName = "R2";
        appInfo.apiVersion = VK_API_VERSION_1_3;
        appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.engineVersion = appInfo.applicationVersion;

        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &appInfo;

        std::vector<const char*> layers;
        std::vector<const char*> extensions;

        if (enableValidation)
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back("VK_EXT_debug_utils");
        }

        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
#ifdef __ANDROID__
        extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif

        if (instanceExts != nullptr)
        {
            const char** exts = instanceExts;
            while (*exts != nullptr)
            {
                extensions.push_back(*exts);
                exts++;
            }
        }

        ici.enabledExtensionCount = (uint32_t)extensions.size();
        ici.enabledLayerCount = (uint32_t)layers.size();

        ici.ppEnabledExtensionNames = extensions.data();
        ici.ppEnabledLayerNames = layers.data();

        VKCHECK(vkCreateInstance(&ici, handles.AllocCallbacks, &handles.Instance));
        volkLoadInstanceOnly(handles.Instance);

        if (enableValidation)
        {
            VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
            };
            messengerCreateInfo.pfnUserCallback = vulkanDebugMessageCallback;
            messengerCreateInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            messengerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            messengerCreateInfo.pUserData = this;
            VKCHECK(vkCreateDebugUtilsMessengerEXT(
                handles.Instance, &messengerCreateInfo, handles.AllocCallbacks, &messenger));
        }
        else
        {
            messenger = VK_NULL_HANDLE;
        }

        selectPhysicalDevice();
    }

    void Core::selectPhysicalDevice()
    {
        VkPhysicalDevice devices[8];
        int deviceScores[8] = {0};
        uint32_t deviceCount = 8;

        VKCHECK(vkEnumeratePhysicalDevices(handles.Instance, &deviceCount, devices));

        // Most of the time, we just want to pick the first device.
        handles.PhysicalDevice = devices[0];
    }

    void Core::findQueueFamilies()
    {
        const VkQueueFlags graphicsFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        const VkQueueFlags asyncComputeFlags = VK_QUEUE_COMPUTE_BIT;

        uint32_t numQueueFamilyProperties = 8;
        VkQueueFamilyProperties queueFamilyProps[8];

        vkGetPhysicalDeviceQueueFamilyProperties(handles.PhysicalDevice, &numQueueFamilyProperties, queueFamilyProps);

        handles.Queues.AsyncComputeFamilyIndex = ~0u;
        handles.Queues.GraphicsFamilyIndex = ~0u;
        handles.Queues.PresentFamilyIndex = ~0u;

        for (uint32_t i = 0; i < numQueueFamilyProperties; i++)
        {
            VkQueueFamilyProperties& props = queueFamilyProps[i];

            if ((props.queueFlags & graphicsFlags) == graphicsFlags)
            {
                handles.Queues.GraphicsFamilyIndex = i;
                handles.Queues.PresentFamilyIndex = i;
            }
            else if ((props.queueFlags & asyncComputeFlags) == asyncComputeFlags)
            {
                handles.Queues.AsyncComputeFamilyIndex = i;
            }
        }

        if (handles.Queues.GraphicsFamilyIndex == ~0u)
        {
            throw RenderInitException("Couldn't find graphics queue!");
        }
    }

    struct VulkanStructHeader
    {
        VkStructureType sType;
        void* pNext;
    };

    bool Core::checkFeatures(VkPhysicalDevice device)
    {
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

        VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

        features2.pNext = &features11;
        features11.pNext = &features12;
        features12.pNext = &features13;

        vkGetPhysicalDeviceFeatures2(device, &features2);

        void* nextStruct = features2.pNext;

        if (!features11.multiview)
            return false;
        if (!features12.descriptorIndexing)
            return false;
        if (!features12.descriptorBindingPartiallyBound)
            return false;
        if (!features12.descriptorBindingVariableDescriptorCount)
            return false;
        if (!features13.synchronization2)
            return false;
        if (!features13.dynamicRendering)
            return false;

        return true;
    }

    bool Core::checkExtensionSupport(VkPhysicalDevice device, const char* extension)
    {
        uint32_t extCount;
        VKCHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr));
        std::vector<VkExtensionProperties> extProps;
        extProps.resize(extCount);
        VKCHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, extProps.data()));

        bool hasAccelStructure = false;
        bool hasRayQuery = false;

        for (auto& extProp : extProps)
        {
            if (strcmp(extProp.extensionName, extension) == 0)
                return true;
        }

        return false;
    }

    bool Core::checkRaytracingSupport(VkPhysicalDevice device)
    {
        return checkExtensionSupport(device, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
               checkExtensionSupport(device, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    }

    struct ChainHeader
    {
        VkStructureType sType;
        void* pNext;
    };

    void Core::createDevice(const char** deviceExts)
    {
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        supportedFeatures.RayTracing = checkRaytracingSupport(handles.PhysicalDevice);
        supportedFeatures.VariableRateShading = checkExtensionSupport(handles.PhysicalDevice, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        supportedFeatures.DynamicRendering = checkExtensionSupport(handles.PhysicalDevice, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

        if (!supportedFeatures.DynamicRendering)
        {
            g_renderPassCache = new RenderPassCache(this);
        }
        else
        {
            g_renderPassCache = nullptr;
        }

        // Features
        // ========
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

#ifndef __ANDROID__
        features.features.shaderStorageImageMultisample = true;
#endif
        features.features.samplerAnisotropy = true;
        features.features.multiDrawIndirect = true;
        features.features.fragmentStoresAndAtomics = true;
        features11.multiview = true;
        features11.shaderDrawParameters = true;
        features12.descriptorIndexing = true;
        features12.descriptorBindingPartiallyBound = true;
        features12.descriptorBindingVariableDescriptorCount = true;
        features12.descriptorBindingSampledImageUpdateAfterBind = true;
        features12.descriptorBindingUniformBufferUpdateAfterBind = true;
        features12.descriptorBindingStorageImageUpdateAfterBind = true;
        features12.descriptorBindingStorageBufferUpdateAfterBind = true;
        features12.shaderSampledImageArrayNonUniformIndexing = true;
        features12.runtimeDescriptorArray = true;
        features12.imagelessFramebuffer = true;
#ifndef __ANDROID__
        features13.synchronization2 = true;
        features13.dynamicRendering = true;
#endif

        dci.pNext = &features;
        features.pNext = &features11;
        features11.pNext = &features12;
        features12.pNext = &features13;

        ChainHeader* chainEnd = (ChainHeader*)&features13;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures
            {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        asFeatures.accelerationStructure = VK_TRUE;

        VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        rqFeatures.rayQuery = VK_TRUE;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtpFeatures
            {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        rtpFeatures.rayTracingPipeline = VK_TRUE;

        // Set up the features chain if the device supports raytracing
        if (supportedFeatures.RayTracing)
        {
            chainEnd->pNext = &asFeatures;
            asFeatures.pNext = &rqFeatures;
            rqFeatures.pNext = &rtpFeatures;
            chainEnd = (ChainHeader*)&rtpFeatures;
        }

        VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrsFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        if (supportedFeatures.VariableRateShading)
        {
            chainEnd->pNext = &vrsFeatures;
            vrsFeatures.attachmentFragmentShadingRate = VK_TRUE;
            vrsFeatures.primitiveFragmentShadingRate = VK_TRUE;
            vrsFeatures.pipelineFragmentShadingRate = VK_TRUE;
            chainEnd = (ChainHeader*)&vrsFeatures;
        }

        // Extensions
        // ==========
        std::vector<const char*> extensions;
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        if (supportedFeatures.RayTracing)
        {
            extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        }

        if (supportedFeatures.VariableRateShading)
        {
            extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        }

#ifdef __ANDROID__
        extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        extensions.push_back(VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
        extensions.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
#endif

        if (deviceExts != nullptr)
        {
            const char** exts = deviceExts;
            while (*exts != nullptr)
            {
                extensions.push_back(*exts);
                exts++;
            }
        }

        dci.enabledExtensionCount = (uint32_t)extensions.size();
        dci.ppEnabledExtensionNames = extensions.data();

        // Queues
        // ======
        VkDeviceQueueCreateInfo queueCreateInfos[2]{};

        const float one = 1.0f;

        queueCreateInfos[0] = VkDeviceQueueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueCreateInfos[0].queueCount = 1;
        queueCreateInfos[0].pQueuePriorities = &one;
        queueCreateInfos[0].queueFamilyIndex = handles.Queues.GraphicsFamilyIndex;

        // Create the async compute queue if we found it
        if (handles.Queues.AsyncComputeFamilyIndex != ~0u)
        {
            queueCreateInfos[1] = VkDeviceQueueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queueCreateInfos[1].queueCount = 1;
            queueCreateInfos[1].pQueuePriorities = &one;
            queueCreateInfos[1].queueFamilyIndex = handles.Queues.AsyncComputeFamilyIndex;
        }

        dci.pQueueCreateInfos = queueCreateInfos;

        // If there's no async compute queue, we're only creating the one graphics queue.
        dci.queueCreateInfoCount = handles.Queues.AsyncComputeFamilyIndex == ~0u ? 1 : 2;

        // Device Creation
        // ===============
        VKCHECK(vkCreateDevice(handles.PhysicalDevice, &dci, handles.AllocCallbacks, &handles.Device));

        volkLoadDevice(handles.Device);

        vkGetDeviceQueue(handles.Device, handles.Queues.GraphicsFamilyIndex, 0, &handles.Queues.Graphics);

        if (handles.Queues.AsyncComputeFamilyIndex != ~0u)
        {
            vkGetDeviceQueue(handles.Device, handles.Queues.AsyncComputeFamilyIndex, 0, &handles.Queues.AsyncCompute);
        }
    }

    void Core::createCommandPool()
    {
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = handles.Queues.GraphicsFamilyIndex;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VKCHECK(vkCreateCommandPool(handles.Device, &cpci, handles.AllocCallbacks, &handles.CommandPool));
    }

    void Core::createAllocator()
    {
        VmaVulkanFunctions vulkanFunctions{};
        vulkanFunctions.vkAllocateMemory = vkAllocateMemory;
        vulkanFunctions.vkBindBufferMemory = vkBindBufferMemory;
        vulkanFunctions.vkBindBufferMemory2KHR = vkBindBufferMemory2;
        vulkanFunctions.vkBindImageMemory = vkBindImageMemory;
        vulkanFunctions.vkBindImageMemory2KHR = vkBindImageMemory2;
        vulkanFunctions.vkCmdCopyBuffer = vkCmdCopyBuffer;
        vulkanFunctions.vkCreateBuffer = vkCreateBuffer;
        vulkanFunctions.vkCreateImage = vkCreateImage;
        vulkanFunctions.vkDestroyBuffer = vkDestroyBuffer;
        vulkanFunctions.vkDestroyImage = vkDestroyImage;
        vulkanFunctions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
        vulkanFunctions.vkFreeMemory = vkFreeMemory;
        vulkanFunctions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
        vulkanFunctions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
        vulkanFunctions.vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements;
        vulkanFunctions.vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements;
        vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        vulkanFunctions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
        vulkanFunctions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
        vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vulkanFunctions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
        vulkanFunctions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
        vulkanFunctions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
        vulkanFunctions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
        vulkanFunctions.vkMapMemory = vkMapMemory;
        vulkanFunctions.vkUnmapMemory = vkUnmapMemory;

        VmaAllocatorCreateInfo vaci{};
        vaci.vulkanApiVersion = VK_API_VERSION_1_1;
        vaci.physicalDevice = handles.PhysicalDevice;
        vaci.device = handles.Device;
        vaci.instance = handles.Instance;
        vaci.pVulkanFunctions = &vulkanFunctions;
        vaci.pAllocationCallbacks = handles.AllocCallbacks;

        VKCHECK(vmaCreateAllocator(&vaci, &handles.Allocator));
    }

    void Core::createDescriptorPool()
    {
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1000;
        VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 500},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 500},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 500}
        };

        dpci.pPoolSizes = poolSizes;
        dpci.poolSizeCount = sizeof(poolSizes) / sizeof(VkDescriptorPoolSize);
        dpci.flags =
            VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        VKCHECK(vkCreateDescriptorPool(handles.Device, &dpci, handles.AllocCallbacks, &handles.DescriptorPool));
    }
}
