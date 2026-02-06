
#include "RTG.hpp"
#include "S72.hpp"

#include "Tutorial.hpp"

#include <iostream>

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

		// load scene:
		S72 s72;
		try {
			s72 = S72::load(configuration.scene_file);
		} catch (std::exception &e) {
			// - e — the caught exception object                                        
			// - .what() — returns a const char* (C-string) containing the message passed when the exception was thrown  
			std::cerr <<  "Failed to load s72-format scene from '" << configuration.scene_file << e.what() << std::endl;
			return 1;
		}

		if (configuration.print_s72 == true) {
			// TODO: print out some scene info
		}

		//loads vulkan library, creates surface, initializes helpers:
		RTG rtg(configuration); // Creates an RTG object named rtg; Passes configuration as a parameter to the constructor

		//initializes global (whole-life-of-application) resources:
		Tutorial application(rtg);

		//main loop -- handles events, renders frames, etc:
		rtg.run(application);

	} catch (std::exception &e) {
		std::cerr << "Exception: " << e.what() << std::endl;
		return 1;
	}
}
