
#include "RTG.hpp"
#include "S72.hpp"

#include "Tutorial.hpp"

#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

void print_info(S72 &s72){
	std::cout << "--- S72 Scene Objects ---"<< std::endl;
	std::cout << "Scene: " << s72.scene.name << std::endl;
	std::cout << "Roots: ";
	for (S72::Node* root : s72.scene.roots) {
		std::cout << root->name << ", ";
	}
	std::cout << std::endl;

	std::cout << "Nodes: ";
	for (auto const& pair : s72.nodes) {
		std::cout << pair.first << ", ";
	}
	std::cout << std::endl;

	std::cout << "Meshes: ";
	for (auto const& pair : s72.meshes) {
		std::cout << pair.first << ", ";
	}
	std::cout << std::endl;

	std::cout << "Cameras: ";
	for (auto const& pair : s72.cameras) {
		std::cout << pair.first << ", ";
	}
	std::cout << std::endl;

	 std::cout << "Drivers: ";
	for (auto const& driver : s72.drivers) {
		std::cout << driver.name << ", ";
	}
	std::cout << std::endl;

	std::cout << "Materials: ";
	for (auto const& pair : s72.materials) {
		std::cout << pair.first << ", ";
	}
	std::cout << std::endl;

	std::cout << "Environment: ";
	for (auto const& pair : s72.environments) {
		std::cout << pair.first << ", ";
	}
	std::cout << std::endl;

	std::cout << "Lights: ";
	for (auto const& pair : s72.lights) {
		std::cout << pair.first << ", ";
	}
	std::cout << std::endl;
}

void traverse_children(S72 &s72, S72::Node* node, std::string prefix){
	//Print node information
	std::cout << prefix << node->name << ": {";
	if(node->camera != nullptr){
		std::cout << "Camera: " << node->camera->name;
	}
	if(node->mesh != nullptr){
		std::cout << "Mesh: " << node->mesh->name;
		if(node->mesh->material != nullptr){
			std::cout << " {Material: " <<node->mesh->material->name << "}";
		}
	}
	if(node->environment != nullptr){
		std::cout << "Environment: " << node->environment->name;
	}
	if(node->light != nullptr){
		std::cout << "Light: " << node->light->name;
	}

	std::cout << "}" <<std::endl;

	std::string new_prefix = prefix + "- ";
	for(S72::Node* child : node->children){
		traverse_children(s72, child, new_prefix);
	}
}

void print_scene_graph(S72 &s72){
	std::cout << std::endl << "--- S72 Scene Graph ---"<< std::endl;
	for (S72::Node* root : s72.scene.roots) {
		std::cout << "Root: ";
		std::string prefix = "";
		traverse_children(s72, root, prefix);
	}
}

int main(int argc, char **argv) {
	//main wrapped in a try-catch so we can print some debug info about uncaught exceptions:
	try {

		//configure application:
		RTG::Configuration configuration;

		configuration.application_info = VkApplicationInfo{
			.pApplicationName = "nakluV Tutorial",
			.applicationVersion = VK_MAKE_VERSION(0,0,0),
			.pEngineName = "Unknown",
			.engineVersion = VK_MAKE_VERSION(0,0,0),
			.apiVersion = VK_API_VERSION_1_3
		};

		bool print_usage = false;

		try {
			configuration.parse(argc, argv);
		} catch (std::runtime_error &e) {
			std::cerr << "Failed to parse arguments:\n" << e.what() << std::endl;
			print_usage = true;
		}

		if (print_usage) {
			std::cerr << "Usage:" << std::endl;
			RTG::Configuration::usage( [](const char *arg, const char *desc){ 
				std::cerr << "    " << arg << "\n        " << desc << std::endl;
			});
			return 1;
		}

		// load s72 scene:
		S72 s72;
		try {
			s72 = S72::load(configuration.scene_file);
			s72.process_meshes(); // extract vertices from binary data
			s72.process_textures(); // load texture images from disk
		} catch (std::exception &e) {
			// - e — the caught exception object
			// - .what() — returns a const char* (C-string) containing the message passed when the exception was thrown
			std::cerr <<  "Failed to load s72-format scene from '" << configuration.scene_file << e.what() << std::endl;
			return 1;
		}

		if (configuration.print_s72 == true) {
			// TODO: print out some scene info
			print_info(s72);
			print_scene_graph(s72);
		}

		//loads vulkan library, creates surface, initializes helpers:
		RTG rtg(configuration); // Creates an RTG object named rtg; Passes configuration as a parameter to the constructor

		//initializes global (whole-life-of-application) resources:
		Tutorial application(rtg, s72);

		//main loop -- handles events, renders frames, etc:
		rtg.run(application);

	} catch (std::exception &e) {
		std::cerr << "Exception: " << e.what() << std::endl;
		return 1;
	}
}
