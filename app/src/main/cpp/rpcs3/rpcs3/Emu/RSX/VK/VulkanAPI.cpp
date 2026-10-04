#include "stdafx.h"
#include "VulkanAPI.h"

#include "Emu/system_config.h"
#include "vkutils/device.h"
#if __ANDROID__
#include "libadrenotools/include/adrenotools/priv.h"
#include "libadrenotools/include/adrenotools/driver.h"
#include <dlfcn.h>
#endif
#include <filesystem>

#define DEF_VK_FUNCTION
#include "VKPFNTable.h"
#undef DEF_VK_FUNCTION

// Global instance handle used by the android WSI surface creation
VkInstance vk_instance = VK_NULL_HANDLE;

namespace vk
{
    void init_base_pfn()
    {
#if __ANDROID__
        static void* vk_lib_handle = nullptr;
        if (vk_lib_handle)
        {
            dlclose(vk_lib_handle);
            vk_lib_handle = nullptr;
        }

        const std::string custom_lib_path = g_cfg.video.vk.custom_driver_lib_path.to_string();

        if (g_cfg.video.vk.use_custom_driver && std::filesystem::exists(custom_lib_path))
        {
            const std::string hook_dir = std::string(getenv("APS3E_NATIVE_LIB_DIR")) + '/';
            const std::string custom_lib_dir = custom_lib_path.substr(0, custom_lib_path.find_last_of('/') + 1);
            const std::string custom_lib_name = custom_lib_path.substr(custom_lib_path.find_last_of('/') + 1);

            vk_lib_handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr
                , hook_dir.c_str()
                , custom_lib_dir.c_str()
                , custom_lib_name.c_str()
                , nullptr, nullptr);

            if (!vk_lib_handle)
            {
                rsx_log.error("#### Failed to load custom driver: %s", custom_lib_path);
            }

            adrenotools_set_turbo(g_cfg.video.vk.custom_driver_force_max_gpu_clocks.get());
        }

        if (vk_lib_handle)
        {
#define LOAD_VK_FUNCTION
#include "VKPFNTable.h"
#undef LOAD_VK_FUNCTION
        }
        else
        {
            vk_lib_handle = dlopen("libvulkan.so", RTLD_NOW);
#define LOAD_VK_FUNCTION
#include "VKPFNTable.h"
#undef LOAD_VK_FUNCTION
        }

#elif defined(_WIN32)
#define LOAD_VK_FUNCTION
#include "VKPFNTable.h"
#undef LOAD_VK_FUNCTION
#else
#define LOAD_VK_FUNCTION
#include "VKPFNTable.h"
#undef LOAD_VK_FUNCTION
#endif
    }

    void init_instance_pfn(VkInstance instance)
    {
#define INSTANCE_VK_FUNCTION
#define VK_FUNC(func) _##func = reinterpret_cast<PFN_##func>(_vkGetInstanceProcAddr(instance, #func))
#include "VKPFNTableEXT.h"
#undef VK_FUNC
#undef INSTANCE_VK_FUNCTION
        vk_instance = instance;
    }

    void init_device_pfn(VkDevice device)
    {
#define DEVICE_VK_FUNCTION
#define VK_FUNC(func) _##func = reinterpret_cast<PFN_##func>(_vkGetDeviceProcAddr(device, #func))
#include "VKPFNTableEXT.h"
#undef VK_FUNC
#undef DEVICE_VK_FUNCTION
    }
}
