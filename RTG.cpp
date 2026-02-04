#include "RTG.hpp"

#include "VK.hpp"
// #include "refsol.hpp"

#include <vulkan/vulkan_core.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_beta.h> //for portability subset
#include <vulkan/vulkan_metal.h> //for VK_EXT_METAL_SURFACE_EXTENSION_NAME
#endif
#include <vulkan/vk_enum_string_helper.h> //useful for debug output
#include <vulkan/utility/vk_format_utils.h> //for getting format sizes
#include <GLFW/glfw3.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <set>

void RTG::Configuration::parse(int argc, char **argv) {
	for (int argi = 1; argi < argc; ++argi) {
		std::string arg = argv[argi];
		if (arg == "--debug") {
			debug = true;
		} else if (arg == "--no-debug") {
			debug = false;
		} else if (arg == "--physical-device") {
			if (argi + 1 >= argc) throw std::runtime_error("--physical-device requires a parameter (a device name).");
			argi += 1;
			physical_device_name = argv[argi];
		} else if (arg == "--drawing-size") {
			if (argi + 2 >= argc) throw std::runtime_error("--drawing-size requires two parameters (width and height).");
			auto conv = [&](std::string const &what) {
				argi += 1;
				std::string val = argv[argi];
				for (size_t i = 0; i < val.size(); ++i) {
					if (val[i] < '0' || val[i] > '9') {
						throw std::runtime_error("--drawing-size " + what + " should match [0-9]+, got '" + val + "'.");
					}
				}
				return std::stoul(val);
			};
			surface_extent.width = conv("width");
			surface_extent.height = conv("height");
		} else if (arg == "--headless") {
			headless = true;
		} else {
			throw std::runtime_error("Unrecognized argument '" + arg + "'.");
		}
	}
}

void RTG::Configuration::usage(std::function< void(const char *, const char *) > const &callback) {
	callback("--debug, --no-debug", "Turn on/off debug and validation layers.");
	callback("--physical-device <name>", "Run on the named physical device (guesses, otherwise).");
	callback("--drawing-size <w> <h>", "Set the size of the surface to draw to.");
	callback("--headless", "Don't create a window; read events from stdin.");
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT type,
	const VkDebugUtilsMessengerCallbackDataEXT *data,
	void *user_data
) {
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		std::cerr << "\x1b[91m" << "E: "; // TODO: understandt that this is ANSI escape code; these escape codes will ensure that compliant terminals print our error logging messages in color.
	} else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		std::cerr << "\x1b[33m" << "w: ";
	} else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
		std::cerr << "\x1b[90m" << "i: ";
	} else { //VERBOSE
		std::cerr << "\x1b[90m" << "v: ";
	}
	std::cerr << data->pMessage << "\x1b[0m" << std::endl;

	return VK_FALSE;
}

RTG::RTG(Configuration const &configuration_) : helpers(*this) {

	//copy input configuration:
	configuration = configuration_;

	//fill in flags/extensions/layers information:

	{ //create the `instance` (main handle to Vulkan library):
		// refsol::RTG_constructor_create_instance(
		// 	configuration.application_info,
		// 	configuration.debug,
		// 	&instance,
		// 	&debug_messenger
		// );
		VkInstanceCreateFlags instance_flags = 0;
		
		// ┌─────────┬─────────┬─────────┐                                                                          
		// │ ptr[0]  │ ptr[1]  │ ptr[2]  │                                                                          
		// └────┬────┴────┬────┴────┬────┘                                                                          
		// 	│         │         │                                                                               
		// 	▼         ▼         ▼                                                                               
		// 	"VK_KHR_surface"  "VK_EXT_debug_utils"  "VK_KHR_swapchain"                                            
     	// (string in memory) (string in memory)    (string in memory)  
		std::vector< const char * > instance_extensions; // what does the * syntax mean //vv this vec holds pointers to constant character(s)  
		std::vector< const char * > instance_layers;

		// add extensions for MoltenVK portability layer on macOS
		// First, the portability layer extensions (which are only needed on macOS). 
		// These allow our app to work on macOS through the MoltenVK translation layer between Vulkan and Metal. 
		#if defined(__APPLE__)
		instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; // Tells Vulkan to include "portability" devices (MoltenVK) when enumerating GPUs 

		instance_extensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME); // Required extension for the above flag to work 
		instance_extensions.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME); // Base surface extension for presenting to window
		instance_extensions.emplace_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME); // Creates Vulkan surfaces from Metal layers (macOS-specific)
		#endif

		//add extensions and layers for debugging:
		if (configuration.debug) {
			instance_extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); // what's this //vv allows us to get debug messages delivered to a callback of our choosing; a Vulkan extension that provides the infrastructure for receiving debug messages.
			instance_layers.emplace_back("VK_LAYER_KHRONOS_validation"); // what's this //vv check that our Vulkan usage comports(aligns) with the specification
		}

		{ //add extensions needed by glfw:
			glfwInit();
			if (!glfwVulkanSupported()) { // tells us if the GLFW version we're using can actually do things with Vulkan
				throw std::runtime_error("GLFW reports Vulkan is not supported.");
			}

			uint32_t count;
			// returns an array of extensions that GLFW wants:
			const char **extensions = glfwGetRequiredInstanceExtensions(&count); // how to understand the ** syntax //vv a pointer to an array of pointers to constant character(s)
			if (extensions == nullptr) {
				throw std::runtime_error("GLFW failed to return a list of requested instance extensions. Perhaps it was not compiled with Vulkan support.");
			}
			for (uint32_t i = 0; i < count; ++i) {
				instance_extensions.emplace_back(extensions[i]);
			}
		}

		// write debug messenger structure
		VkDebugUtilsMessengerCreateInfoEXT debug_messenger_create_info{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			.messageSeverity =
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			.messageType =
				VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			.pfnUserCallback = debug_callback,
			.pUserData = nullptr
		};

		VkInstanceCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pNext = (configuration.debug ? &debug_messenger_create_info : nullptr), // pass debug structure if configured
			.flags = instance_flags, //  Platform-specific flags (e.g., VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR for MoltenVK on macOS).
			.pApplicationInfo = &configuration.application_info, // App name, version, engine name, Vulkan API version. Helps drivers optimize for known apps.
			.enabledLayerCount = uint32_t(instance_layers.size()), // How many validation/debug layers to enable. 
			.ppEnabledLayerNames = instance_layers.data(), //  Array of layer names like "VK_LAYER_KHRONOS_validation".  
			.enabledExtensionCount = uint32_t(instance_extensions.size()), // How many instance extensions to enable.
			.ppEnabledExtensionNames = instance_extensions.data() // Array of extension names like "VK_KHR_surface", "VK_EXT_debug_utils". 
		};
		VK( vkCreateInstance(&create_info, nullptr, &instance) );

		// create debug messenger
		if (configuration.debug) {
			PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
			if (!vkCreateDebugUtilsMessengerEXT) {
				throw std::runtime_error("Failed to lookup debug utils create fn.");
			}
			VK( vkCreateDebugUtilsMessengerEXT(instance, &debug_messenger_create_info, nullptr, &debug_messenger) );
		}
	}

	{ //create the `window` and `surface` (where things get drawn):
		// refsol::RTG_constructor_create_surface(
		// 	configuration.application_info,
		// 	configuration.debug,
		// 	configuration.surface_extent,
		// 	instance,
		// 	&window,
		// 	&surface
		// );

		// GLFW uses a hint system to configure the next window created. This Set hints (affects next glfwCreateWindow call) 
		// A hint is a configuration request that the system will try to honor, but isn't guaranteed. 
		// GLFW_CLIENT_API is hint name - it specifies which graphics API GLFW should set up for the window; GLFW_NO_API means No OpenGL (for Vulkan use)
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

		window = glfwCreateWindow(configuration.surface_extent.width, configuration.surface_extent.height, configuration.application_info.pApplicationName, nullptr, nullptr);

		if (!window) {
			throw std::runtime_error("GLFW failed to create a window.");
		}

		VK( glfwCreateWindowSurface(instance, window, nullptr, &surface) );
	}

	{ //select the `physical_device` -- the gpu that will be used to draw:
		// refsol::RTG_constructor_select_physical_device(
		// 	configuration.debug,
		// 	configuration.physical_device_name,
		// 	instance,
		// 	&physical_device
		// );
		std::vector< std::string > physical_device_names; //for later error message
		{ //pick the best physical device
			uint32_t count = 0;
			VK( vkEnumeratePhysicalDevices(instance, &count, nullptr) ); // sets count
			std::vector< VkPhysicalDevice > physical_devices(count);
			VK( vkEnumeratePhysicalDevices(instance, &count, physical_devices.data()) ); // Enumerates the physical devices accessible to a Vulkan instance

			uint32_t best_score = 0;

			for (auto const &pd : physical_devices) {
				VkPhysicalDeviceProperties properties;
				vkGetPhysicalDeviceProperties(pd, &properties);

				VkPhysicalDeviceFeatures features;
				vkGetPhysicalDeviceFeatures(pd, &features);

				physical_device_names.emplace_back(properties.deviceName);

				if (!configuration.physical_device_name.empty()) { // either (a) look for a name matching the configuration, if one was specified:
					if (configuration.physical_device_name == properties.deviceName) {
						if (physical_device) {
							std::cerr << "WARNING: have two physical devices with the name '" << properties.deviceName << "'; using the first to be enumerated." << std::endl;
						} else {
							physical_device = pd;
						}
					}
				} else { // or (b) look for a device with a high "score" for a simple scoring function:
					uint32_t score = 1;
					//  just looks for any discrete GPU. You might -- at some point -- want to refine this to look for specific features of interest.
					if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
						score += 0x8000;
					}

					if (score > best_score) {
						best_score = score;
						physical_device = pd;
					}
				}
			}
		}

		if (physical_device == VK_NULL_HANDLE) {
			// report error
			std::cerr << "Physical devices:\n";
			for (std::string const &name : physical_device_names) {
				std::cerr << "    " << name << "\n";
			}
			std::cerr.flush(); //vv make error appears immediately

			if (!configuration.physical_device_name.empty()) {
				throw std::runtime_error("No physical device with name '" + configuration.physical_device_name + "'.");
			} else {
				throw std::runtime_error("No suitable GPU found.");
			}
		}

		{ //report device name:
			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(physical_device, &properties);
			std::cout << "Selected physical device '" << properties.deviceName << "'." << std::endl;
		}
	}

	{ //select the `surface_format` and `present_mode` which control how colors are represented on the surface and how new images are supplied to the surface:
		// refsol::RTG_constructor_select_format_and_mode(
		// 	configuration.debug,
		// 	configuration.surface_formats,
		// 	configuration.present_modes,
		// 	physical_device,
		// 	surface,
		// 	&surface_format,
		// 	&present_mode
		// );
		std::vector< VkSurfaceFormatKHR > formats;
		std::vector< VkPresentModeKHR > present_modes;
		
		{
			uint32_t count = 0;
			VK( vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, nullptr) );
			formats.resize(count);
			VK( vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, formats.data()) );
		}

		{
			uint32_t count = 0;
			VK( vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, nullptr) );
			present_modes.resize(count);
			VK( vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, present_modes.data()) );
		}

		//find first available surface format matching config:
		surface_format = [&](){
			for (auto const &config_format : configuration.surface_formats) {
				for (auto const &format : formats) {
					if (config_format.format == format.format && config_format.colorSpace == format.colorSpace) {
						return format;
					}
				}
			}
			throw std::runtime_error("No format matching requested format(s) found.");
		}();

		//find first available present mode matching config:
		present_mode = [&](){
			for (auto const &config_mode : configuration.present_modes) {
				for (auto const &mode : present_modes) {
					if (config_mode == mode) {
						return mode;
					}
				}
			}
			throw std::runtime_error("No present mode matching requested mode(s) found.");
		}();
	}

	{ //create the `device` (logical interface to the GPU) and the `queue`s to which we can submit commands:
		// refsol::RTG_constructor_create_device(
		// 	configuration.debug,
		// 	physical_device,
		// 	surface,
		// 	&device,
		// 	&graphics_queue_family,
		// 	&graphics_queue,
		// 	&present_queue_family,
		// 	&present_queue
		// );

		{ //look up queue indices; getting handles to the queues we will be submitting work on:
			uint32_t count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
			std::vector< VkQueueFamilyProperties > queue_families(count); // Queues come from various families. So we list all of the queue families
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queue_families.data());

			// We need to find a queue family (index) that supports graphics, and one that we can use to present on the supplied surface.
			// so we check for the desired properties on each queue family:
			for (auto const &queue_family : queue_families) {
				uint32_t i = uint32_t(&queue_family - &queue_families[0]);

				//if it does graphics, set the graphics queue family:
				if (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
					if (!graphics_queue_family) graphics_queue_family = i; // std::optional< uint32_t > type allows us to check them as bools (testing if they contain a value) and set them to indices.
				}

				//if it has present support, set the present queue family:
				VkBool32 present_support = VK_FALSE;
				VK( vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &present_support) );
				if (present_support == VK_TRUE) {
					if (!present_queue_family) present_queue_family = i;
				}
			}

			if (!graphics_queue_family) {
				throw std::runtime_error("No queue with graphics support.");
			}

			if (!present_queue_family) {
				throw std::runtime_error("No queue with present support.");
			}
		}

		//select device extensions:
		std::vector< const char * > device_extensions;
		#if defined(__APPLE__)
		device_extensions.emplace_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
		#endif
		//Add the swapchain extension:
		device_extensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

		{ //create the logical device - the root of all our application-specific Vulkan resources
			std::vector< VkDeviceQueueCreateInfo > queue_create_infos;
			std::set< uint32_t > unique_queue_families{
				graphics_queue_family.value(),
				present_queue_family.value()
			};

			float queue_priorities[1] = { 1.0f };
			for (uint32_t queue_family : unique_queue_families) {
				queue_create_infos.emplace_back(VkDeviceQueueCreateInfo{
					.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = queue_family,
					.queueCount = 1,
					.pQueuePriorities = queue_priorities,
				});
			}

			VkDeviceCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.queueCreateInfoCount = uint32_t(queue_create_infos.size()),
				.pQueueCreateInfos = queue_create_infos.data(),

				//device layers are depreciated; spec suggests passing instance_layers or nullptr:
				.enabledLayerCount = 0,
				.ppEnabledLayerNames = nullptr,

				.enabledExtensionCount = static_cast< uint32_t>(device_extensions.size()),
				.ppEnabledExtensionNames = device_extensions.data(),

				//pass a pointer to a VkPhysicalDeviceFeatures to request specific features: (e.g., thick lines)
				.pEnabledFeatures = nullptr,
			};

			VK( vkCreateDevice(physical_device, &create_info, nullptr, &device) );

			vkGetDeviceQueue(device, graphics_queue_family.value(), 0, &graphics_queue);
			vkGetDeviceQueue(device, present_queue_family.value(), 0, &present_queue);
		}
	}

	//run any resource creation required by Helpers structure:
	helpers.create();

	//create initial swapchain:
	recreate_swapchain();

	//create workspace resources:
	workspaces.resize(configuration.workspaces);
	for (auto &workspace : workspaces) {
		// refsol::RTG_constructor_per_workspace(device, &workspace);
		{ // create workspace fences:
				VkFenceCreateInfo create_info{
					.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
					.flags = VK_FENCE_CREATE_SIGNALED_BIT, // start signaled, because all workspaces are available to start
				};
				VK( vkCreateFence(device, &create_info, nullptr, &workspace.workspace_available) );
		}
		{ // create workspace semaphores:
			VkSemaphoreCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			};
			VK( vkCreateSemaphore(device, &create_info, nullptr, &workspace.image_available) );
		}
	}

}
RTG::~RTG() {
	//don't destroy until device is idle:
	if (device != VK_NULL_HANDLE) {
		if (VkResult result = vkDeviceWaitIdle(device); result != VK_SUCCESS) {
			std::cerr << "Failed to vkDeviceWaitIdle in RTG::~RTG [" << string_VkResult(result) << "]; continuing anyway." << std::endl;
		}
	}

	//destroy workspace resources:
	for (auto &workspace : workspaces) {
		// refsol::RTG_destructor_per_workspace(device, &workspace);
		if (workspace.workspace_available != VK_NULL_HANDLE) {
			vkDestroyFence(device, workspace.workspace_available, nullptr);
			workspace.workspace_available = VK_NULL_HANDLE;
		}
		if (workspace.image_available != VK_NULL_HANDLE) {
			vkDestroySemaphore(device, workspace.image_available, nullptr);
			workspace.image_available = VK_NULL_HANDLE;
		}
	}
	workspaces.clear();

	//destroy the swapchain:
	destroy_swapchain();

	//destroy Helpers structure resources:
	helpers.destroy();

	//destroy the rest of the resources:
	// refsol::RTG_destructor( &device, &surface, &window, &debug_messenger, &instance );
	if (device != VK_NULL_HANDLE) { // logical device is the handle to our code's view of the GPU
		vkDestroyDevice(device, nullptr);
		device = VK_NULL_HANDLE;
	}

	if (surface != VK_NULL_HANDLE) { // surface is Vulkan's view of the part of the window that shows our graphics
		vkDestroySurfaceKHR(instance, surface, nullptr);
		surface = VK_NULL_HANDLE;
	}

	if (window != nullptr) { // window is the handle showing output, managed by GLFW
		glfwDestroyWindow(window);
		window = nullptr;
	}

	// The debug_messenger holds information about the callback function that we've been using to get information from the validation layer
	if (debug_messenger != VK_NULL_HANDLE) {
		PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
		if (vkDestroyDebugUtilsMessengerEXT) {
			vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, nullptr);
			debug_messenger = VK_NULL_HANDLE;
		}
	}

	if (instance != VK_NULL_HANDLE) { // instance is handle to library
		vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}
}


void RTG::recreate_swapchain() {
	// refsol::RTG_recreate_swapchain(
	// 	configuration.debug,
	// 	device,
	// 	physical_device,
	// 	surface,
	// 	surface_format,
	// 	present_mode,
	// 	graphics_queue_family,
	// 	present_queue_family,
	// 	&swapchain,
	// 	&swapchain_extent,
	// 	&swapchain_images,
	// 	&swapchain_image_views,
	// 	&swapchain_image_dones
	// );

	//clean up swapchain if it already exists:
	if (!swapchain_images.empty()) {
		destroy_swapchain();
	}

	if (configuration.headless) {
		// assert(surface == VK_NULL_HANDLE); //headless, so must not have a surface

		//make a fake swapchain:

		// set extent from configuration
		swapchain_extent = configuration.surface_extent;

		// set number of images to 3
		uint32_t requested_count = 3; //enough for FIFO-style presentation

		{ //create command pool for the headless image copy command buffers:

			//  In headless mode, there's no window or real swapchain. Instead, the      
			// 	application creates a "fake swapchain" with its own GPU images. The      
			// 	command pool is used to allocate command buffers that copy rendered      
			// 	images from GPU memory to host memory (so they can be saved to files or  
			// 	processed).                                                              

			// Since we're going to record these commands once and never reset them, we don't pass VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT when creating the command pool.
			assert(headless_command_pool == VK_NULL_HANDLE);
			VkCommandPoolCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = 0,
				.queueFamilyIndex = graphics_queue_family.value(),
			};
			VK( vkCreateCommandPool(device, &create_info, nullptr, &headless_command_pool) );
		}

		// create headless_swapchain
		assert(headless_swapchain.empty());
		headless_swapchain.reserve(requested_count);
		for (uint32_t i = 0; i < requested_count; ++i) {
			//add a headless "swapchain" image:
			HeadlessSwapchainImage &h = headless_swapchain.emplace_back();

			//allocate image data: (on-GPU, will be rendered to)
			h.image = helpers.create_image(
				swapchain_extent,
				surface_format.format,
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // The image can be used as a color attachment in a framebuffer (i.e., you can render to it) + The image can be used as the source of a transfer/copy operation 
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT // is allocated on the GPU
			);

			//allocate buffer data: (on-CPU, will be copied to)
			h.buffer = helpers.create_buffer(
				// calculates the buffer size in bytes needed to hold the entire image for CPU readback:
				// width × height × texelBlockSize / texelsPerBlock // TODO: understand this
				swapchain_extent.width * swapchain_extent.height * vkuFormatTexelBlockSize(surface_format.format) / vkuFormatTexelsPerBlock(surface_format.format),
				
				VK_BUFFER_USAGE_TRANSFER_DST_BIT, // the image will be received as a transfer destination
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				Helpers::Mapped // host-visible, host-coherent, mapped memory so it's easy for us to extract the image for saving later.
			);

			{ //create and record copy command:
				// almost identical to the one used in Helpers::transfer_to_image, except that we're copying image-to-buffer, not buffer-to-image.
				VkCommandBufferAllocateInfo alloc_info{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					.commandPool = headless_command_pool,
					.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					.commandBufferCount = 1,
				};
				VK( vkAllocateCommandBuffers(device, &alloc_info, &h.copy_command) );

				//record:
				// we're going to submit the command buffer more that once, 
				// so we don't specify VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT in the begin_info flags.
				VkCommandBufferBeginInfo begin_info{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = 0,
				};
				VK( vkBeginCommandBuffer(h.copy_command, &begin_info) );

				
				VkBufferImageCopy region{
					.bufferOffset = 0,
					.bufferRowLength = swapchain_extent.width,
					.bufferImageHeight = swapchain_extent.height,
					.imageSubresource{
						.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
						.mipLevel = 0,
						.baseArrayLayer = 0,
						.layerCount = 1,
					},
					.imageOffset{ .x = 0, .y = 0, .z = 0 },
					.imageExtent{
						.width = swapchain_extent.width,
						.height = swapchain_extent.height,
						.depth = 1
					},
				};
				vkCmdCopyImageToBuffer(
					h.copy_command, // command buffer
					h.image.handle, // source
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // source image layout; note that we'll need to make sure the image is transitioned to this layout when rendering finishes
					h.buffer.handle, // dest buffer
					1, &region
				);
				
				VK( vkEndCommandBuffer(h.copy_command) );
			}

			{ //create fence to signal when image is done being "presented" (copied to host memory):
				VkFenceCreateInfo create_info{
					.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
					.flags = VK_FENCE_CREATE_SIGNALED_BIT, //start signaled, because all images are available to start with
				};
				VK( vkCreateFence(device, &create_info, nullptr, &h.image_presented) );
			}
		}

		//copy image references into swapchain_images:
		// instead of calling vkGetSwapchainImagesKHR to extract references to the images, we just copy the images' handles
		assert(swapchain_images.empty());
		swapchain_images.assign(requested_count, VK_NULL_HANDLE);
		for (uint32_t i = 0; i < requested_count; ++i) {
			swapchain_images[i] = headless_swapchain[i].image.handle;
		}
	} else {
		assert(surface != VK_NULL_HANDLE); //not headless, so must have a surface

		//request a swapchain from the windowing system:

		//determine size, image count, and transform (capabilities.currentTransform) for swapchain:
		// size:
		VkSurfaceCapabilitiesKHR capabilities;
		VK( vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities) );
		swapchain_extent = capabilities.currentExtent;

		// image count:
		// set to one more than the minimum supported, but clamp it to the maximum supported count
		uint32_t requested_count = capabilities.minImageCount + 1; // add one more to allow some amount of parallelism in rendering.
		if (capabilities.maxImageCount != 0) {
			requested_count = std::min(capabilities.maxImageCount, requested_count);
		}

		{ //create swapchain
			VkSwapchainCreateInfoKHR create_info{
				.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
				.surface = surface,
				.minImageCount = requested_count,
				.imageFormat = surface_format.format,
				.imageColorSpace = surface_format.colorSpace,
				.imageExtent = swapchain_extent,
				.imageArrayLayers = 1,
				.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				.preTransform = capabilities.currentTransform,
				.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, //  No transparency; controls how your window blends with content behind it (like the desktop or other windows).
				.presentMode = present_mode,
				.clipped = VK_TRUE,
				.oldSwapchain = VK_NULL_HANDLE //NOTE: could be more efficient by passing old swapchain handle here instead of destroying it
			};

			std::vector< uint32_t > queue_family_indices{
				graphics_queue_family.value(),
				present_queue_family.value()
			};

			if (queue_family_indices[0] != queue_family_indices[1]) {
				//if images will be presented on a different queue, make sure they are shared:
				create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
				create_info.queueFamilyIndexCount = uint32_t(queue_family_indices.size());
				create_info.pQueueFamilyIndices = queue_family_indices.data();
			} else {
				create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			}

			VK( vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain) );
		}

		// Creating the swapchain created a list of images, but we can't do anything with those images without VkImage handles, so
		{ //get the swapchain images:
			uint32_t count = 0;
			VK( vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr) ); // getting the array size; Queries that return a variable-length array will return just the length if the data parameter is nullptr
			swapchain_images.resize(count);
			VK( vkGetSwapchainImagesKHR(device, swapchain, &count, swapchain_images.data()) );
		}
	}

	// Vulkan code that accesses images generally does so through an image view (handle type: VkImageView). 
	// So we might as well create image views for all our swapchain images here:
	swapchain_image_views.assign(swapchain_images.size(), VK_NULL_HANDLE);
	for (size_t i = 0; i < swapchain_images.size(); ++i) {
		VkImageViewCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = swapchain_images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = surface_format.format,
			.components{
				.r = VK_COMPONENT_SWIZZLE_IDENTITY,
				.g = VK_COMPONENT_SWIZZLE_IDENTITY,
				.b = VK_COMPONENT_SWIZZLE_IDENTITY,
				.a = VK_COMPONENT_SWIZZLE_IDENTITY
			},
			.subresourceRange{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
		};
		VK( vkCreateImageView(device, &create_info, nullptr, &swapchain_image_views[i]) );
	}

	// every rendering to a swapchain image will need a semaphore to communicate that the rendering has finished to the windowing system, 
	// so it can wait before presenting the image. 
	// Because there is no elegant way to reclaim these semaphores other than waiting for a given swapchain image to be re-acquired, 
	// we need to allocate one such semaphore for each swapchain image:
	{ //create semaphores for waiting on each image to be done:
		VkSemaphoreCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};

		swapchain_image_dones.assign(swapchain_images.size(), VK_NULL_HANDLE);
		for (size_t i = 0; i < swapchain_image_dones.size(); ++i) {
			VK( vkCreateSemaphore(device, &create_info, nullptr, &swapchain_image_dones[i]) );
		}
	}

	if (configuration.debug) {
		std::cout << "Swapchain is now " << swapchain_images.size() << " images of size " << swapchain_extent.width << "x" << swapchain_extent.height << "." << std::endl;
	}
}


void RTG::destroy_swapchain() {
	// refsol::RTG_destroy_swapchain(
	// 	device,
	// 	&swapchain,
	// 	&swapchain_images,
	// 	&swapchain_image_views,
	// 	&swapchain_image_dones
	// );

	// ensure that nothing is actively rendering to a swapchain image (or waiting on a swapchain semaphore) while we are freeing it. 
	VK( vkDeviceWaitIdle(device) ); // wait for any rendering to old swapchain to finish

	// clean up semaphores used for waiting on the swapchain:
	for (auto &semaphore : swapchain_image_dones) {
		vkDestroySemaphore(device, semaphore, nullptr);
		semaphore = VK_NULL_HANDLE;
	}
	swapchain_image_dones.clear();

	// clean up image views referencing swapchain:
	// Destroy image views with vkDestroyImageView (you created them with vkCreateImageView, you destroy them) 
	for (auto &iamge_view : swapchain_image_views) {
		vkDestroyImageView(device, iamge_view, nullptr);
		iamge_view = VK_NULL_HANDLE;
	}
	swapchain_image_views.clear(); 

	// forget handles to swapchain images (will destroy by deallocating th eswapchain itself):
	// Just clear the image handles vector (no vkDestroyImage call - you don't own them)
	swapchain_images.clear();

	if (configuration.headless) {
		//destroy the headless swapchain with its images and buffers
		for (auto &h : headless_swapchain) {
			helpers.destroy_image(std::move(h.image));

			// destroys a VkBuffer - a chunk of GPU memory used
			// to store data. In this headless swapchain context, h.buffer is used to  
			// hold the rendered image data that gets copied from the GPU image to      
			// CPU-readable memory.   
			helpers.destroy_buffer(std::move(h.buffer));
			
			h.copy_command = VK_NULL_HANDLE; //pool deallocated below
			vkDestroyFence(device, h.image_presented, nullptr);
			h.image_presented = VK_NULL_HANDLE;
		}
		headless_swapchain.clear();

		// destroy headless_command_pool:
		// Notice that we don't bother to free the individual command buffers, since we're about to destroy the whole pool anyway
		//free all of the copy command buffers (VkCommandBuffer objects) by destroying the pool from which they were allocated:
		vkDestroyCommandPool(device, headless_command_pool, nullptr);
		headless_command_pool = VK_NULL_HANDLE;
	} else {
		// we don't need to call vkDestroyImage on the swapchain image handles, since these are owned by the swapchain itself. 
		// (We do need to destroy the image views, since we created those ourselves.)
		// deallocate the swapchain and (thus) its images:
		if (swapchain != VK_NULL_HANDLE) {
			vkDestroySwapchainKHR(device, swapchain, nullptr);
			swapchain = VK_NULL_HANDLE;
		}
	}
}

// generates a mouse motion event:
static void cursor_pos_callback(GLFWwindow *window, double xpos, double ypos) { // what does static mean //vv only visible in this file (file-scoped)
	std::vector< InputEvent > *event_queue = reinterpret_cast< std::vector< InputEvent > * >(glfwGetWindowUserPointer(window));
	if (!event_queue) return;

	InputEvent event;
	std::memset(&event, '\0', sizeof(event)); // what does this do //??  make sure that any parts of the union we don't write are in a known (and boring) state.

	event.type = InputEvent::MouseMotion;
	event.motion.x = float(xpos);
	event.motion.y = float(ypos);

	// what does this mean //vv Initializes the button state bitmask to zero before the loop populates it. 
	// Each bit will represent whether a mouse button is pressed (bit 0 = left button, bit 1 = right, bit 2 = middle, etc.). 
	event.motion.state = 0; 
	
	// Builds a bitmask of which mouse buttons are currently held during this mouse movement:  
	for (int b = 0; b < 8 && b < GLFW_MOUSE_BUTTON_LAST; ++b) { // what does this loop do //vv Iterates through mouse buttons 0-7 (or up to GLFW_MOUSE_BUTTON_LAST) 
		if (glfwGetMouseButton(window, b) == GLFW_PRESS) { // For each pressed button, 
			event.motion.state |= (1 << b); // sets the corresponding bit: state |= (1 << b)
			// Result: state = 0b00000101 would mean buttons 0 and 2 are pressed 
		}
	}

	event_queue->emplace_back(event);
}

static void mouse_button_callback(GLFWwindow *window, int button, int action, int mods) {
	std::vector< InputEvent > *event_queue = reinterpret_cast< std::vector< InputEvent > * >(glfwGetWindowUserPointer(window)); // what does this do, what is reinterpret_cast //vv "trust me, this void* is actually a std::vector<InputEvent>*"
	if (!event_queue) return;

	InputEvent event;
	std::memset(&event, '\0', sizeof(event));

	if (action == GLFW_PRESS) {
		event.type = InputEvent::MouseButtonDown;
	} else if (action == GLFW_RELEASE) {
		event.type = InputEvent::MouseButtonUp;
	} else {
		std::cerr << "Strange: unknown mouse button action." << std::endl;
		return;
	}

	double xpos, ypos;
	glfwGetCursorPos(window, &xpos, &ypos);
	event.button.x = float(xpos);
	event.button.y = float(ypos);
	event.button.state = 0; // what does this mean //vv This records which other buttons are held while this button was clicked (e.g., Ctrl+click, or left+right click together). 
	for (int b = 0; b < 8 && b < GLFW_MOUSE_BUTTON_LAST; ++b) {
		if (glfwGetMouseButton(window, b) == GLFW_PRESS) {
			event.button.state |= (1 << b);
		}
	}

	// Stores which button triggered this callback:                                                             
	// - button = 0 → Left mouse button (GLFW_MOUSE_BUTTON_LEFT)                                                
	// - button = 1 → Right mouse button (GLFW_MOUSE_BUTTON_RIGHT)                                              
	// - button = 2 → Middle mouse button (GLFW_MOUSE_BUTTON_MIDDLE)   
	event.button.button = uint8_t(button); // what does this do //vv

	// Stores modifier keys held during the click. mods is a bitmask:                                           
	// - GLFW_MOD_SHIFT (0x0001) - Shift key                                                                    
	// - GLFW_MOD_CONTROL (0x0002) - Ctrl key                                                                   
	// - GLFW_MOD_ALT (0x0004) - Alt key                                                                        
	// - GLFW_MOD_SUPER (0x0008) - Windows/Cmd key   
	event.button.mods = uint8_t(mods); // what's this //vv

	event_queue->emplace_back(event);
}


static void scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
	std::vector< InputEvent > *event_queue = reinterpret_cast< std::vector< InputEvent > * >(glfwGetWindowUserPointer(window));
	if (!event_queue) return;

	InputEvent event;
	std::memset(&event, '\0', sizeof(event)); //?? explain what memset() does, then explain what this code do

	event.type = InputEvent::MouseWheel;
	event.motion.x = float(xoffset);
	event.motion.y = float(yoffset);

	event_queue->emplace_back(event);
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) {
	std::vector< InputEvent > *event_queue = reinterpret_cast< std::vector< InputEvent > * >(glfwGetWindowUserPointer(window)); // what does this do, what is reinterpret_cast //vv "trust me, this void* is actually a std::vector<InputEvent>*"
	if (!event_queue) return;

	InputEvent event;
	std::memset(&event, '\0', sizeof(event));

	if (action == GLFW_PRESS) {
		event.type = InputEvent::KeyDown;
	} else if (action == GLFW_RELEASE) {
		event.type = InputEvent::KeyUp;
	} else if (action == GLFW_REPEAT) {
		// ignore repeats
		return;
	} else {
		std::cerr << "Strange: unknown mouse button action." << std::endl;
		return;
	}
  
	event.key.key = uint8_t(key); 
	event.key.mods = uint8_t(mods);

	event_queue->emplace_back(event);
}

void RTG::HeadlessSwapchainImage::save() const {
	if (save_to == "") return;

	if (image.format == VK_FORMAT_B8G8R8A8_SRGB) {
		//get a pointer to the image data copied to the buffer:
		char const *bgra = reinterpret_cast< char const * >(buffer.allocation.data());

		//convert bgra -> rgb data: //TODO: understand this reordeing
		// To convert BGRA data to RGB data we need to re-order the first three bytes of every pixel and discard the last byte. 
		// We make a temporary std::vector to hold the converted data and size it appropriately.
		std::vector< char > rgb(image.extent.height * image.extent.width * 3);
		for (uint32_t y = 0; y < image.extent.height; ++y) {
			for (uint32_t x = 0; x < image.extent.width; ++x) {
				rgb[(y * image.extent.width + x) * 3 + 0] = bgra[(y * image.extent.width + x) * 4 + 2];
				rgb[(y * image.extent.width + x) * 3 + 1] = bgra[(y * image.extent.width + x) * 4 + 1];
				rgb[(y * image.extent.width + x) * 3 + 2] = bgra[(y * image.extent.width + x) * 4 + 0];
			}
		}

		//write ppm file:
		//  we use a stream in std::ios::binary mode -- 
		// otherwise any \n bytes will be expanded into \r\b on Windows, causing color and alignment shifts in the output file.
		std::ofstream ppm(save_to, std::ios::binary);
		ppm << "P6\n"; //magic number + newline
		ppm << image.extent.width << " " << image.extent.height << "\n"; //image size + newline
		ppm << "255\n"; //max color value + newline
		ppm.write(rgb.data(), rgb.size()); //rgb data in row-major order, starting from the top left
	} else {
		std::cerr << "WARNING: saving format " << string_VkFormat(image.format) << " not supported." << std::endl;
	}
}

// the "harness" that connects an RTG::Application (like Tutorial) to the windowing system and GPU.
void RTG::run(Application &application) {
	// refsol::RTG_run(*this, application);
	// initial on_swapchain:
	// give the application the chance to create framebuffers for the current state of the swapchain.
	auto on_swapchain = [&, this]() {
		application.on_swapchain(*this, SwapchainEvent{
			.extent = swapchain_extent, 
			.images = swapchain_images,
			.image_views = swapchain_image_views,
		});
	};
	on_swapchain();

	// setup event handling
	std::vector< InputEvent > event_queue;
	if (!configuration.headless) {
		glfwSetWindowUserPointer(window, &event_queue);

		glfwSetCursorPosCallback(window, cursor_pos_callback);
		glfwSetMouseButtonCallback(window, mouse_button_callback);
		glfwSetScrollCallback(window, scroll_callback);
		glfwSetKeyCallback(window, key_callback);
	}

	uint32_t headless_next_image = 0;

	// setup time handling:
	std::chrono::high_resolution_clock::time_point before = std::chrono::high_resolution_clock::now();

	while (configuration.headless || !glfwWindowShouldClose(window)) { // run until GLFW lets us know the window should be closed via the glfwWindowShouldClose call.
		float headless_dt = 0.0f;
		std::string headless_save = "";
		
		// event handling
		if (configuration.headless) {
			//read events from stdin
			std::string line;
			while (std::getline(std::cin, line)) { // what is getline and std::cin //??
				// parse event from line
				try {
					std::istringstream iss(line); // what are istringstream//??
					iss.imbue(std::locale::classic()); //ensure floating point numbers always parse with '.' as the separator // what does this line do //??

					// read type:
					std::string type;
					if (!(iss >> type)) throw std::runtime_error("failed to read event type");

					// type-specific parsing:
					if (type == "AVAILABLE") {  //AVAILABLE dt [save.ppm]
						// read dt:
						if (!(iss >> headless_dt)) throw std::runtime_error("failed to read dt");
						if (headless_dt < 0.0f) throw std::runtime_error("dt less than zero");

						// check for save file name:
						if (iss >> headless_save) {
							if (!headless_save.ends_with(".ppm")) throw std::runtime_error("output filename ("" + headless_save + "") must end with .ppm");
						}

						// check for trailing junk
						char junk;
						if (iss >> junk) throw std::runtime_error("trailing junk in event line");

						//stop parsing events so a frame can draw
						break;
					} else {
						throw std::runtime_error("unrecognized type");
					}

				} catch (std::exception &e) {
					std::cerr << "WARNING: failed to parse event (" << e.what() << ") from: "" << line << ""; ignoring it." << std::endl;
				}
			}
			//if we've run out of events, stop running the main loop:
			if (!std::cin) break;
		} else {
			glfwPollEvents();
		}

		// deliver all input events to application:
		for (InputEvent const &input : event_queue) {
			application.on_input(input); // what does on_input do //vv Tutorial::on_input seems to be empty because the tutorial currently doesn't need user input
		}
		event_queue.clear();

		{ // elapsed time handling
			std::chrono::high_resolution_clock::time_point after = std::chrono::high_resolution_clock::now();

			// - At 60 FPS: dt ≈ 0.0167 seconds (~16.7 ms)                                                              
  			// - At 30 FPS: dt ≈ 0.0333 seconds (~33.3 ms) 
			float dt = float(std::chrono::duration< double >(after - before).count()); // seconds passed (in float)
			before = after;
			dt = std::min(dt, 0.1f); // lag if frame wrate dips too slow

			//in headless mode, override dt:
			if (configuration.headless) dt = headless_dt;

			application.update(dt); // the Tutorial::update(float dt)
		}

		// render handling (with on_swapchain as needed):
		uint32_t workspace_index;
		{
			// acquire a workspace i.e. getting a set of buffers that aren't being used in a current rendering operation
			// How do we know a workspace isn't being used? Each workspace has an associated workspace_available fence, which is signaled when the rendering work on this workspace is done.
			assert(next_workspace < workspaces.size());
			workspace_index = next_workspace;
			next_workspace = (next_workspace + 1) % workspaces.size();

			// wait until the worksapce is not being used:
			VK( vkWaitForFences(device, 1, &workspaces[workspace_index].workspace_available, VK_TRUE, UINT64_MAX) );

			// mark the workspace as in use:
			VK( vkResetFences(device, 1, &workspaces[workspace_index].workspace_available) );
		}

		uint32_t image_index = -1U;

		if (configuration.headless) {
			// assert(swapchain == VK_NULL_HANDLE);

			//acquire the least-recently-used headless swapchain image:
			assert(headless_next_image < uint32_t(headless_swapchain.size()));
			image_index = headless_next_image;
			headless_next_image = (headless_next_image + 1) % uint32_t(headless_swapchain.size());

			//wait for image to be done copying to buffer
			VK( vkWaitForFences(device, 1, &headless_swapchain[image_index].image_presented, VK_TRUE, UINT64_MAX) );

			//save buffer, if needed:
			if (headless_swapchain[image_index].save_to != "") {
				headless_swapchain[image_index].save(); // handle writing the previous frame
				headless_swapchain[image_index].save_to = ""; // setting save_to for the next frame
			}

			//remember if next frame should be saved:
			headless_swapchain[image_index].save_to = headless_save;

			// mark next copy as pending
			VK( vkResetFences(device, 1, &headless_swapchain[image_index].image_presented) );

			//signal GPU that image is "available for rendering to"
			VkSubmitInfo submit_info{
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.signalSemaphoreCount = 1,
				.pSignalSemaphores = &workspaces[workspace_index].image_available
			};
			VK( vkQueueSubmit(graphics_queue, 1, &submit_info, nullptr) );
		} else {
			// acquire an image (resize swapchain if needed):
			retry:          
			// ask the swapchain for the next image index - note careful return handling:
			if (VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, workspaces[workspace_index].image_available, VK_NULL_HANDLE, &image_index); 
				result == VK_ERROR_OUT_OF_DATE_KHR) {
				// if the swapchain is out-of-date, 
				std::cerr << "Recreating swapchain because vkAquireNextImageKHR returned" << string_VkResult(result) << "." << std::endl; // what is std::cerr //??
				
				// recreate it.
				// These two functions work together when the swapchain becomes invalid (e.g., window resize):  
				recreate_swapchain(); // Destroys the old swapchain and creates a new one with updated parameters. manages RTG's internal Vulkan resources   
				on_swapchain(); // Calls the application's on_swapchain method to let it recreate any resources dependent on the swapchain (like framebuffers). manages Application's dependent resources

				goto retry; // and run the loop again
			} else if (result == VK_SUBOPTIMAL_KHR) {
				// if the swapchain is suboptimal, render to it and recreate it later:
				std::cerr << "Suboptimal swapchain format - ignoring for the moment." << std::endl;
			} else if (result != VK_SUCCESS) {
				// other non-success results are genuine errors:
				throw std::runtime_error("Failed to acquire swapchain image (" + std::string(string_VkResult(result)) + ")!");
			}
		}

		//call render function: //?? is this the correct place for it
		// assembles a RenderParams parameter structure and hands it to the application:
		application.render(*this, RenderParams{
			.workspace_index = workspace_index,
			.image_index = image_index,
			.image_available = workspaces[workspace_index].image_available,
			.image_done = swapchain_image_dones[image_index],
			.workspace_available = workspaces[workspace_index].workspace_available,
		});

		// queue the rendering work for presentation:
		 if (configuration.headless) {
			//in headless mode, submit the copy command we recorded previously:

			//will wait in the transfer stage for image_done to be signaled:
			VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo submit_info{
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.waitSemaphoreCount = 1,
				.pWaitSemaphores = &swapchain_image_dones[image_index],
				.pWaitDstStageMask = &wait_stage,
				.commandBufferCount = 1,
				.pCommandBuffers = &headless_swapchain[image_index].copy_command, // kicks off the copy command buffer
			};
			
			// submit GPU work that waits for the image to be done rendering, 
			VK( vkQueueSubmit(graphics_queue, 1, &submit_info, headless_swapchain[image_index].image_presented) ); // signals the copy-finished fence

		} else { 
			VkPresentInfoKHR present_info{
				.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				.waitSemaphoreCount = 1, 
				.pWaitSemaphores = &swapchain_image_dones[image_index],
				.swapchainCount = 1,
				.pSwapchains = &swapchain,
				.pImageIndices = &image_index,
			};

			assert(present_queue);

			// note, again, the careful return handling:
			if (VkResult result = vkQueuePresentKHR(present_queue, &present_info);
				result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
				std::cerr << "Recreating swapchain because vkQueuePresentKHR returned" << string_VkResult(result) << "." << std::endl;
				recreate_swapchain();
				on_swapchain();
			} else if (result != VK_SUCCESS) {
				throw std::runtime_error("failed to queue presentation of image (" + std::string(string_VkResult(result)) + ")!");
			}
		}

		//TODO: present image (resize swapchain if needed)
	}

	// tear down event handling
	if (!configuration.headless) {
		glfwSetCursorPosCallback(window, nullptr);
		glfwSetMouseButtonCallback(window, nullptr);
		glfwSetScrollCallback(window, nullptr);
		glfwSetKeyCallback(window, nullptr);

		glfwSetWindowUserPointer(window, nullptr);
	}
}
