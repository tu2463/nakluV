#include "S72.hpp"

// The underscore distinguishes the raw pointer parameter from the nicer reference object created later in this fn
std::string extract_string(std::map< std::string, sejp::value > *object_, std::string const &key, std::string const &what)
 {
    // TODO:
    //pull out a string property of a sejp object as a string.
    // throws if the property is missing
    // deletes property from the object and returns the value if all is well
 }

S72 S72::load(std::string const &scene_file) {
    S72 s72; // the loaded scene, will be returned at end of function

    sejp::value json = sejp::load(scene_file);

    // try to interpret JSON as array; returns an "std::optional<std::map<std::string, value>> empty" if not an array:
    std::optional< std::vector< sejp::value > > array = json.as_array();

    // fetch top-level array from the scene file:
    if (!array) {
        throw std::runtime_error("Top-level value of s72 file should be an array.");
    }

    // check magic value:
    if (!(array->size() >= 1 && array->at(0).as_string() && array->at(0).as_string.value() == "s72-v2")) {
        throw std::runtime_error("First element of s72 array should be \"s72-v2\".");
    }

    // parse the remaining objects
    for (size_t i = 1; i < array->size(); i++) {
        // make a copy of the object and erase numbers as they are parsed:
        std::map< std::string, sejp::value > object;
        try {
            object = array->at(i).as_object().value();
        } catch (std::exception &) {
            throw std::runtime_error("Array element" + std::to_string(i) + " is not an object.");
        }

        // all objects must have a "type" and "name". extract them from the object
        std::string type = extract_string(&object, "type", "Object at index" + std::to_string(i) + "'s \"type\"");
		std::string name = extract_string(&object, "name", "Object at index " + std::to_string(i) + "'s \"name\"");

        if (type == "SCENE") {
			//reference to the object we are parsing into:
            // TODO: handle scene, create scene struct
            Scene &scene = s72.scene;

            // check that we haven't already parsed the scene info (we will populate the *name* field for a new scene):
            if (scene.name != "") {
                throw std::runtime_error("Multiple \"SCENE\" objects in s72 file."); 
            }

            // mark scene as parsed:
            scene.name = name;

            // "roots":[...] (optional, default is []) -- array of references to nodes at which to start drawing the scene:
            if (auto f = object.find("roots"); f != object.end()) {
                std::vector< std::string > refs; // will store the names of the nodes that are scene roots
                try {
                    // f->first;   // the key: "roots"                                      
                    // f->second;  // the value: the JSON array ["node A", "node B"] 
                    std::vector< sejp::value > const &vec = f->second.as_array().value();
                    for (auto const &value : vec) {
                        refs.emplace_back(value.as_string().value());
                    }
                } catch (std::exception &) { // "&" instead of "&e", meaning we catch all exceptions but ignore the details, since we don't care why parsing failed, just that it did 
                    throw std::runtime_error("Scene \"" + name + "\"'s roots are not an array of strings.");
                }
                scene.roots.reserve(refs.size());
                for (auto const &ref : refs) {
                    scene.roots.emplace_back(&s72.nodes[ref]); //NOTE: creates new (empty) nodes if not yet parsed; will check for this later
                }
                object.erase(f); // useful mainly for debugging unhandled properties; we will call warn_on_unhandled at end of loop iteration to check for any properties we forgot to parse
            }

        } else if (type == "NODE") {
            // TODO: node struct
        } else if (type == "MESH") {
            // TODO: mesh struct
        } else if (type == "CAMERA") {
            // TODO: camera struct
        } else if (type == "DRIVER") {
            // TODO: driver struct
        }  else if (type == "MATERIAL") {
            // TODO: MATERIAL struct
        }  else if (type == "ENVIRONMENT") {
            // TODO: ENVIRONMENT struct
        }  else if (type == "LIGHT") {
            // TODO: LIGHT struct
        } else {
            std::runtime_error("Unknown object type"); // TODO: is this a good way to throw errors?
        }
    }
}