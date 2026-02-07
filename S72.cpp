#include "S72.hpp"

//functions that do the inverse of those in vk_enum_string_helper.h :

//used for mesh topologies:
VkPrimitiveTopology topology_to_VkPrimitiveTopology(std::string const &topology) {
    /* equivalent to:
    static std::map< std::string, VkPrimitiveTopology > const map = {                                        
        { "POINT_LIST",                      VK_PRIMITIVE_TOPOLOGY_POINT_LIST },                             
        { "LINE_LIST",                       VK_PRIMITIVE_TOPOLOGY_LINE_LIST },                              
        { "LINE_STRIP",                      VK_PRIMITIVE_TOPOLOGY_LINE_STRIP },                             
        { "TRIANGLE_LIST",                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },                          
        { "TRIANGLE_STRIP",                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP },                         
        { "TRIANGLE_FAN",                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN },                           
        { "LINE_LIST_WITH_ADJACENCY",        VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY },               
        { "LINE_STRIP_WITH_ADJACENCY",       VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY },              
        { "TRIANGLE_LIST_WITH_ADJACENCY",    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY },           
        { "TRIANGLE_STRIP_WITH_ADJACENCY",   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY },          
        { "PATCH_LIST",                      VK_PRIMITIVE_TOPOLOGY_PATCH_LIST },                             
    };  
    */
    static std::map< std::string, VkPrimitiveTopology > const map = {
#define DO( X ) { #X, VK_PRIMITIVE_TOPOLOGY_ ## X }
		DO( POINT_LIST ),
		DO( LINE_LIST ),
		DO( LINE_STRIP ),
		DO( TRIANGLE_LIST ),
		DO( TRIANGLE_STRIP ),
		DO( TRIANGLE_FAN ),
		DO( LINE_LIST_WITH_ADJACENCY ),
		DO( LINE_STRIP_WITH_ADJACENCY ),
		DO( TRIANGLE_LIST_WITH_ADJACENCY ),
		DO( TRIANGLE_STRIP_WITH_ADJACENCY ),
		DO( PATCH_LIST ),
#undef DO
    };

    auto f = map.find(topology);
    if (f == map.end()) throw new std::runtime_error("Unrecognized topology \"" + topology + "\".");
    return f->second
}

//used for index formats:
VkIndexType format_to_VkIndexType(std::string const &format) {
	static std::map< std::string, VkIndexType > const map = {
#define DO( X ) { #X, VK_INDEX_TYPE_ ## X }
		DO( UINT16 ),
		DO( UINT32 ),
		DO( UINT8 ),
		//not including: VK_INDEX_TYPE_NONE_KHR,
#undef DO
	};
	auto f = map.find(format);
	if (f == map.end()) throw new std::runtime_error("Unrecognized index format \"" + format + "\".");
	return f->second;
}

//used for vertex data formats:
VkFormat format_to_VkFormat(std::string const &format) {
	static std::map< std::string, VkFormat > const map = {
#define DO( X ) { #X, VK_FORMAT_ ## X }
		//formats for which VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT is always set.
		// see: https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#features-required-format-support
		DO( R8_UNORM ),
		DO( R8_SNORM ),
		DO( R8_UINT ),
		DO( R8_SINT ),
		DO( R8G8_UNORM ),
		DO( R8G8_SNORM ),
		DO( R8G8_UINT ),
		DO( R8G8_SINT ),

		DO( R8G8B8A8_UNORM ),
		DO( R8G8B8A8_SNORM ),
		DO( R8G8B8A8_UINT ),
		DO( R8G8B8A8_SINT ),
		DO( B8G8R8A8_UNORM ),

		DO( A8B8G8R8_UNORM_PACK32 ),
		DO( A8B8G8R8_SNORM_PACK32 ),
		DO( A8B8G8R8_UINT_PACK32 ),
		DO( A8B8G8R8_SINT_PACK32 ),

		DO( A2B10G10R10_UNORM_PACK32 ),

		DO( R16_UNORM ),
		DO( R16_SNORM ),
		DO( R16_UINT ),
		DO( R16_SINT ),
		DO( R16_SFLOAT ),
		DO( R16G16_UNORM ),
		DO( R16G16_SNORM ),
		DO( R16G16_UINT ),
		DO( R16G16_SINT ),
		DO( R16G16_SFLOAT ),
		DO( R16G16B16A16_UNORM ),
		DO( R16G16B16A16_SNORM ),
		DO( R16G16B16A16_UINT ),
		DO( R16G16B16A16_SINT ),
		DO( R16G16B16A16_SFLOAT ),

		DO( R32_UINT ),
		DO( R32_SINT ),
		DO( R32_SFLOAT ),
		DO( R32G32_UINT ),
		DO( R32G32_SINT ),
		DO( R32G32_SFLOAT ),
		DO( R32G32B32_UINT ),
		DO( R32G32B32_SINT ),
		DO( R32G32B32_SFLOAT ),
		DO( R32G32B32A32_UINT ),
		DO( R32G32B32A32_SINT ),
		DO( R32G32B32A32_SFLOAT ),
#undef DO
	};
	auto f = map.find(format);
	if (f == map.end()) throw new std::runtime_error("Unrecognized index format \"" + format + "\".");
	return f->second;
}

//helpers used in loading:

//warn if any members of an object haven't been handled (+ deleted):
void warn_on_unhandled(std::map< std::string, sejp::value > &object, std::string const &what) {
	if (object.empty()) return;
	std::cerr << "WARNING: " << what << " contained unhandled properties: ";
	bool first = true;
	for (auto const &[key, value] : object) {
		if (!first) std::cerr << ", ";
		first = false;
		std::cerr << key;
	}
	std::cerr << '.' << std::endl;
}

//pull out a string property of a sejp object as a string.
    // throws if the property is missing
    // deletes property from the object and returns the value if all is well
// The underscore distinguishes the raw pointer parameter from the nicer reference object created later in this fn
std::string extract_string(std::map< std::string, sejp::value > *object_, std::string const &key, std::string const &what) {
    assert(object_);
    auto &object = *object_;

    std::string string;
    try {
        string = object.at(key).as_string().value();
    } catch(std::exception &) {
        throw std::runtime_error(what + " is missing or not a string");
    }
    object.erase(object.find(key));
    return string;
}

//pull out a number property of a sejp object as an uint32_t.
// throws if the property is missing or can't fit in a uint32_t
// deletes property from the object and returns the value if all is well
uint32_t extract_uint32_t(std::map< std::string, sejp::value > *object_, std::string const &key, std::string const &what) {
	assert(object_);
	auto &object = *object_;

	double number;
	try {
		number = object.at(key).as_number().value();
	} catch (std::exception &) {
		throw std::runtime_error(what + " is not a number.");
	}
	if (std::round(number) != number || number < 0.0 || number > double(std::numeric_limits< uint32_t >::max())) {
		throw std::runtime_error(what + " does not fit in an unsigned 32-bit integer.");
	}
	object.erase(object.find(key));
	return uint32_t(number);
};

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
            //get a reference to the object we are parsing into:
            Node &node = s72.nodes[name]; //NOTE: creates new (empty) node if not yet parsed; will check for this later 
            
            //check that we haven't already parsed this node's information:
			if (node.name != "") {
				throw std::runtime_error("Multiple \"NODE\" objects with name \"" + name + "\".");
			}

			//mark the node as parsed:
			node.name = name;

            if (auto f = object.find("translation"); f != object.end()) {
                try {
                    std::vector< sejp::value > const &vec = f->second.as_array().value();
                    node.translation = vec3{
                        .x = float(vec.at(0).as_number().value()),
                        .y = float(vec.at(1).as_number().value()),
                        .z = float(vec.at(2).as_number().value()),
                    };
                    if (vec.size() != 3) throw std::runtime_error("trailing values");
                } catch (std::exception &) { 
                    throw std::runtime_error("node \"" + name + "\"'s translation should be an array of three numbers.");
                }
                object.erase(f); // useful mainly for debugging unhandled properties; we will call warn_on_unhandled at end of loop iteration to check for any properties we forgot to parse
            }

            if (auto f = object.find("rotation"); f != object.end()) {
				try {
					std::vector< sejp::value > const &vec = f->second.as_array().value();
					node.rotation = quat{
						.x = float(vec.at(0).as_number().value()),
						.y = float(vec.at(1).as_number().value()),
						.z = float(vec.at(2).as_number().value()),
						.w = float(vec.at(3).as_number().value()),
					};
					if (vec.size() != 4) throw std::runtime_error("trailing values");
				} catch (std::exception &) {
					throw std::runtime_error("Node \"" + name + "\"'s rotation should be an array of four numbers.");
				}
				object.erase(f);
			}

			if (auto f = object.find("scale"); f != object.end()) {
				try {
					std::vector< sejp::value > const &vec = f->second.as_array().value();
					node.scale = vec3{
						.x = float(vec.at(0).as_number().value()),
						.y = float(vec.at(1).as_number().value()),
						.z = float(vec.at(2).as_number().value()),
					};
					if (vec.size() != 3) throw std::runtime_error("trailing values");
				} catch (std::exception &) {
					throw std::runtime_error("Node \"" + name + "\"'s scale should be an array of three numbers.");
				}
				object.erase(f);
			}

            if (auto f = object.find("children"); f != object.end()) {
                std::vector< std::string > refs; // will store the names of the children nodes
				try {
					std::vector< sejp::value > const &vec = f->second.as_array().value();
                    for (auto const &value : vec) {
                        refs.emplace_back(value.as_string().value());
                    }
				} catch (std::exception &) {
					throw std::runtime_error("Node \"" + name + "\"'s children should be an array of strings.");
				}

				//create/fixup pointers to other nodes:
                node.children.reserve(refs.size());
                for (auto const &ref : refs) {
				    node.children.emplace_back(&s72.nodes[ref]); //NOTE: creates new (empty) nodes if not yet parsed; will check for this later
                }
				object.erase(f);
			}

            if (auto f = object.find("mesh"); f != object.end()) {
                std::string ref;
				try {
					ref = f->second.as_string().value();
				} catch (std::exception &) {
					throw std::runtime_error("Node \"" + name + "\"'s mesh should be a string.");
				}
                node.mesh = &s72.meshes[ref];
				object.erase(f);
			}

            if (auto f = object.find("camera"); f != object.end()) {
                std::string ref;
				try {
					ref = f->second.as_string().value();
				} catch (std::exception &) {
					throw std::runtime_error("Node \"" + name + "\"'s camera should be a string.");
				}
                node.camera = &s72.cameras[ref];
				object.erase(f);
			}

            if (auto f = object.find("environment"); f != object.end()) {
				std::string ref;
				try {
					ref = f->second.as_string().value();
				} catch (std::exception &) {
					throw std::runtime_error("Node \"" + name + "\"'s environment should be a string.");
				}
				node.environment = &s72.environments[ref];
				object.erase(f);
			}

			if (auto f = object.find("light"); f != object.end()) {
				std::string ref;
				try {
					ref = f->second.as_string().value();
				} catch (std::exception &) {
					throw std::runtime_error("Node \"" + name + "\"'s light should be a string.");
				}
				node.light = &s72.lights[ref];
				object.erase(f);
			}

        } else if (type == "MESH") {
            //reference to the thing we are parsing into:
			Mesh &mesh = s72.meshes[name];

			//check that we haven't already parsed this:
			if (mesh.name != "") {
				throw std::runtime_error("Multiple \"MESH\" objects with name \"" + name + "\".");
			}

			//mark as parsed:
			mesh.name = name;

            std::string topology = extract_string(&object, "topology", "Mesh \"" + name + "\"'s topology");
			mesh.topology = topology_to_VkPrimitiveTopology(topology);

			mesh.count = extract_uint32_t(&object, "count", "Mesh \"" + name + "\"'s count");

            if (auto f = object.find("indices"); f != object.end()) {
				std::map< std::string, sejp::value > obj;
				try {
					obj = f->second.as_object().value();
				} catch (std::exception &) {
					throw std::runtime_error("Mesh \"" + name + "\"'s indices should be an object.");
				}

				std::string src = extract_string(&obj, "src", "Mesh \"" + name + "\"'s indices.src");
				uint32_t offset = extract_uint32_t(&obj, "offset", "Mesh \"" + name + "\"'s indices.offset");
				std::string format = extract_string(&obj, "format", "Mesh \"" + name + "\"'s indices.format");
				mesh.indices.emplace(Mesh::Indices{
					.src = s72.data_files[src],
					.offset = offset,
					.format = format_to_VkIndexType(format),
				});

				warn_on_unhandled(obj, "Mesh \"" + name + "\"'s indices");

				object.erase(f);
			}

            std::map< std::string, sejp::value > attributes;
			try {
				attributes = object.at("attributes").as_object().value();
			} catch (std::exception &) {
				throw std::runtime_error("Mesh \"" + name + "\"'s attributes should be an object.");
			}
			object.erase(object.find("attributes"));

			for (auto const &[key, value] : attributes) {
				std::map< std::string, sejp::value > obj;
				try {
					obj = value.as_object().value();
				} catch (std::exception &) {
					throw std::runtime_error("Mesh \"" + name + "\"'s attribute \"" + key + "\" is not an object.");
				}

				std::string src = extract_string(&obj, "src", "Mesh \"" + name + "\"'s attribute \"" + key + "\"'s src");
				uint32_t offset = extract_uint32_t(&obj, "offset", "Mesh \"" + name + "\"'s attribute \"" + key + "\"' offset");
				uint32_t stride = extract_uint32_t(&obj, "stride", "Mesh \"" + name + "\"'s attribute \"" + key + "\"' stride");
				std::string format = extract_string(&obj, "format", "Mesh \"" + name + "\"'s attribute \"" + key + "\"'s format");
				mesh.attributes.emplace(key, Mesh::Attribute{
					.src = s72.data_files[src],
					.offset = offset,
					.stride = stride,
					.format = format_to_VkFormat(format),
				});

				warn_on_unhandled(obj, "Mesh \"" + name + "\"'s attribute \"" + key + "\"");
			}

            if (auto f = object.find("material"); f != object.end()) {
				std::string material;
				try {
					material = f->second.as_string().value();
				} catch (std::exception &) {
					throw std::runtime_error("Mesh \"" + name + "\"'s material is not a string.");
				}
				object.erase(f);
				mesh.material = &s72.materials[material];
			}
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