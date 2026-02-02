#include "RTG.hpp"

#include "VK.hpp"
#include "refsol.hpp"

#include <vulkan/vulkan_core.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_beta.h> //for portability subset
#include <vulkan/vulkan_metal.h> //for VK_EXT_METAL_SURFACE_EXTENSION_NAME
#endif
#include <vulkan/vk_enum_string_helper.h> //useful for debug output
#include <GLFW/glfw3.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
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
		} else {
			throw std::runtime_error("Unrecognized argument '" + arg + "'.");
		}
	}
}

void RTG::Configuration::usage(std::function< void(const char *, const char *) > const &callback) {
	callback("--debug, --no-debug", "Turn on/off debug and validation layers.");
	callback("--physical-device <name>", "Run on the named physical device (guesses, otherwise).");
	callback("--drawing-size <w> <h>", "Set the size of the surface to draw to.");
}

RTG::RTG(Configuration const &configuration_) : helpers(*this) {

	//copy input configuration:
	configuration = configuration_;

	//fill in flags/extensions/layers information:

	//create the `instance` (main handle to Vulkan library):
	refsol::RTG_constructor_create_instance(
		configuration.application_info,
		configuration.debug,
		&instance,
		&debug_messenger
	);

	//create the `window` and `surface` (where things get drawn):
	refsol::RTG_constructor_create_surface(
		configuration.application_info,
		configuration.debug,
		configuration.surface_extent,
		instance,
		&window,
		&surface
	);

	//select the `physical_device` -- the gpu that will be used to draw:
	refsol::RTG_constructor_select_physical_device(
		configuration.debug,
		configuration.physical_device_name,
		instance,
		&physical_device
	);

	//select the `surface_format` and `present_mode` which control how colors are represented on the surface and how new images are supplied to the surface:
	refsol::RTG_constructor_select_format_and_mode(
		configuration.debug,
		configuration.surface_formats,
		configuration.present_modes,
		physical_device,
		surface,
		&surface_format,
		&present_mode
	);

	//create the `device` (logical interface to the GPU) and the `queue`s to which we can submit commands:
	refsol::RTG_constructor_create_device(
		configuration.debug,
		physical_device,
		surface,
		&device,
		&graphics_queue_family,
		&graphics_queue,
		&present_queue_family,
		&present_queue
	);

	//run any resource creation required by Helpers structure:
	helpers.create();

	//create initial swapchain:
	recreate_swapchain();

	//create workspace resources:
	workspaces.resize(configuration.workspaces);
	for (auto &workspace : workspaces) {
		refsol::RTG_constructor_per_workspace(device, &workspace);
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
		refsol::RTG_destructor_per_workspace(device, &workspace);
	}
	workspaces.clear();

	//destroy the swapchain:
	destroy_swapchain();

	//destroy Helpers structure resources:
	helpers.destroy();

	//destroy the rest of the resources:
	refsol::RTG_destructor( &device, &surface, &window, &debug_messenger, &instance );

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

	// we don't need to call vkDestroyImage on the swapchain image handles, since these are owned by the swapchain itself. 
	// (We do need to destroy the image views, since we created those ourselves.)
	// deallocate the swapchain and (thus) its images:
	if (swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(device, swapchain, nullptr);
		swapchain = VK_NULL_HANDLE;
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
	glfwSetWindowUserPointer(window, &event_queue);

	glfwSetCursorPosCallback(window, cursor_pos_callback);
	glfwSetMouseButtonCallback(window, mouse_button_callback);
	glfwSetScrollCallback(window, scroll_callback);
	glfwSetKeyCallback(window, key_callback);

	// setup time handling:
	std::chrono::high_resolution_clock::time_point before = std::chrono::high_resolution_clock::now();

	while (!glfwWindowShouldClose(window)) { // run until GLFW lets us know the window should be closed via the glfwWindowShouldClose call.
		// event handling
		glfwPollEvents();

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

		//call render function: //?? is this the correct place for it
		// assembles a RenderParams parameter structure and hands it to the application:
		application.render(*this, RenderParams{
			.workspace_index = workspace_index,
			.image_index = image_index,
			.image_available = workspaces[workspace_index].image_available,
			.image_done = swapchain_image_dones[image_index],
			.workspace_available = workspaces[workspace_index].workspace_available,
		});

		{ // queue the rendering work for presentation:
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
	glfwSetCursorPosCallback(window, nullptr);
	glfwSetMouseButtonCallback(window, nullptr);
	glfwSetScrollCallback(window, nullptr);
	glfwSetKeyCallback(window, nullptr);

	glfwSetWindowUserPointer(window, nullptr);
}
