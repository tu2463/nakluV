#pragma once // need this so that this won't be included multiple times

#include "sejp.hpp"
#include "PosNorTexTanVertex.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>
#include <optional>

struct S72 {
    //NOTE: redefine these for your vector and quaternion types of choice:
    // we use this for easy substitution (in the future, can easily switch to e.g. using vec2 = glm::vec3)
	using vec3 = struct vec3_internal{ float x, y, z; };
	using quat = struct quat_internal{ float x, y, z, w; };
	using color = struct color_internal{ float r, g, b; };

    static S72 load(std::string const &file);
    void process_meshes(); // extract vertices from binary data into pooled buffer
    void process_textures(); // load texture images from disk using stb_image
    void process_drivers();

    // Pooled vertex data (populated by process_meshes):
    std::vector<PosNorTexTanVertex> vertices;

    //forward declarations so we can write the scene's objects in the same order as in the spec:
	struct Node;
	struct Mesh;
	struct DataFile;
	struct Camera;
	struct Driver;
	struct Texture;
	struct Material;
	struct Environment;
	struct Light;

    //-------------------------------------------------
	//s72 Scenes contain:

    /* exactly one scene: 
    {
        "type":"SCENE",
        "name":"Cube Pyramid",
        "roots":["node A","node B"]
    },
    */
    struct Scene {
        std::string name;
        std::vector< Node * > roots;
    };
    Scene scene;

    /* zero or more "NODE"s, all with unique names:
    {
        "type":"NODE",
        "name":"bottom",
        "translation":[0,0,0],
        "rotation":[0,0,0,1],
        "scale":[1,1,1],
        "children":["rug","chair","table","carton"],
        "camera":"main-camera",
        "mesh":"room",
        "environment":"outdoors",
        "light":"lamp1"
    },
    */
    struct Node{
        std::string name;
        vec3 translation = vec3{ .x = 0.0f, .y = 0.0f, .z = 0.0f};
        quat rotation = quat{ .x = 0.0f, .y = 0.0f, .z = 0.0f, .w = 1.0f};
        vec3 scale = vec3{ .x = 1.0f, .y = 1.0f, .z = 1.0f};
        std::vector< Node * > children;

        // optional, null of not specified:
        Mesh *mesh = nullptr;
        Camera *camera = nullptr;
        Environment *environment = nullptr;
        Light *light = nullptr;
    };
    std::unordered_map< std::string, Node> nodes;

    /* zero or more "MESH"s, all with unique names:
    {
        "type":"MESH",
        "name":"cube",
        "topology":"TRIANGLE_LIST",
        "count":12, // the number of vertices in the mesh.
        "indices": { "src":"cube.b72", "offset":576, "format":"UINT32" },
        "attributes":{
            "POSITION": { "src":"cube.b72", "offset":0,  "stride":48, "format":"R32G32B32_SFLOAT" },
            "NORMAL":   { "src":"cube.b72", "offset":12, "stride":48, "format":"R32G32B32_SFLOAT" },
            "TANGENT":  { "src":"cube.b72", "offset":24, "stride":48, "format":"R32G32B32A32_SFLOAT" },
            "TEXCOORD": { "src":"cube.b72", "offset":40, "stride":48, "format":"R32G32_SFLOAT" },
        },
        "material":"dull-red",
    },
    */
    struct Mesh{
        std::string name;
        VkPrimitiveTopology topology;
        uint32_t count;
        struct Indices {
            DataFile &src;
            uint32_t offset;
            VkIndexType format;
        };
        std::optional< Indices > indices; // mesh index stream, optional

        struct Attribute {
            DataFile &src;
            uint32_t offset;
            uint32_t stride;
            VkFormat format;
        };
        std::unordered_map< std::string, Attribute > attributes;
        Material *material = nullptr; // optional, null if not specified

        // Computed during process_meshes():
        uint32_t first_vertex = 0; // index into pooled vertices buffer

        // Bounding box in local space (computed during process_meshes):
        vec3 bbox_min = vec3{.x = 0.0f, .y = 0.0f, .z = 0.0f};
        vec3 bbox_max = vec3{.x = 0.0f, .y = 0.0f, .z = 0.0f};
    };
    std::unordered_map< std::string, Mesh> meshes;

    //data files referenced by meshes:
	struct DataFile {
		std::string src; //src used in the s72 file

		//computed during loading:
		std::string path; //path to data file, taking into account path to s72 file (relative to current working directory)

        /*
        The example code does compute the correct (s72-file-relative) paths to load them from; see `DataFile::path` and `Texture::path`. It also unifies multiple references to the same file into the same object.

        It would be reasonable to (e.g.) add extra data members to `DataFile` and `Texture` and load them at the end of `S72::load` just after the path computation code.
        */
        std::vector< uint8_t > data; //raw bytes loaded from the file
	};
    //we organize the data files by "src" so that multiple attributes with the same src resolve to the same DataFile:
	std::unordered_map< std::string, DataFile > data_files;

    /* zero or more "CAMERA"s, all with unique names:
    {
        "type":"CAMERA",
        "name":"main view",
        "perspective":{
            "aspect": 1.777,
            "vfov": 1.04719,
            "near": 0.1,
            "far":10.0
        }
    },
    */
    struct Camera{
        std::string name;
        struct Perspective {
            float aspect;
            float vfov;
            float near;
            float far = std::numeric_limits< float >::infinity(); // far clipping plane distance, optional, if not specified will be set to infinity
        };
		//(s72 leaves open the possibility of other camera projections, but does not define any)
        std::variant< Perspective > projection;
    };
    std::unordered_map< std::string, Camera > cameras;

    /* zero or more "DRIVER"s, all with unique names:
    {
        "type":"DRIVER",
        "name":"camera move",
        "node":"camera transform",
        "channel":"translation",
        "times":[0, 1, 2, 3, 4],
        "values":[0,0,0, 0,0,1, 0,1,1, 1,1,1, 0,0,0],
        "interpolation":"LINEAR"
    },
    */ 
    struct Driver {
		std::string name;

		Node &node;

		enum class Channel {
			translation,
			scale,
			rotation
		} channel;

		std::vector< float > times;
		std::vector< float > values;

		enum class Interpolation {
			STEP,
			LINEAR,
			SLERP,
		} interpolation = Interpolation::LINEAR;
	};
	//NOTE: drivers are stored in a vector in the order they appear in the file.
	//      This is because drivers are applied in file order: //vv
    std::vector< Driver > drivers;

    //textures are not objects from the scene, but referenced by materials:
	struct Texture {
		std::string src; //src used in the s72 file
		enum class Type {
			flat, //"2D" in the spec, but identifier can't start with a number
			cube,
		} type = Type::flat;
		enum class Format { // understand these formats / colorspaces //??
			linear,
			srgb,
			rgbe,
		} format = Format::linear;

		//computed during loading:
		std::string path; //path to data file, taking into account path to s72 file (relative to current working directory)

		// Image data loaded from disk (populated by process_textures):
		int width = 0;
		int height = 0;
		int channels = 0; // number of channels in the original image
		std::vector<uint8_t> pixels; // RGBA pixels (always 4 channels after loading)
	};
	//we organize textures by src + type + format, so that two materials using to the same image *in the same way* end up referring to the same texture object:
    std::unordered_map< std::string, Texture > textures;

    /* zero or more "MATERIAL"s, all with unique names:
    {
        "type":"MATERIAL",
        "name":"boring blue",
        "normalMap":{ "src":"normal.png" },
        "displacementMap":{ "src":"displacement.png" },
        "pbr":{
            "albedo": [0.5, 0.5, 0.85],
            // xor 
            "albedo": { "src":"blue.png" },

            "roughness": 0.5,
            // xor 
            "roughness": { "src":"roughness-map.png" },

            "metalness": 0.0,
            // xor 
            "metalness": { "src":"metalness-map.png" }
        },
        // xor 
        "lambertian": {
            "albedo": [0.5, 0.5, 0.85],
            // xor 
            "albedo": { "src":"blue.png" }
        },
        // xor 
        "mirror": { // no parameters  },
        // xor 
        "environment": { // no parameters  },
    },
    */
    struct Material {
		std::string name;

		Texture *normal_map = nullptr; //optional, set to null if not specified
		Texture *displacement_map = nullptr; //optional, set to null if not specified

		//Materials are one of these types:
		// NOTE: if any of these parameters are the Texture * branch of their variant, they are not null
		struct PBR {
			std::variant< color, Texture * > albedo = color{.r = 1.0f, .g = 1.0f, .b = 1.0f};
			std::variant< float, Texture * > roughness = 1.0f;
			std::variant< float, Texture * > metalness = 0.0f;
		};
		struct Lambertian {
			std::variant< color, Texture * > albedo = color{.r = 1.0f, .g = 1.0f, .b = 1.0f};
		};

        // no parameters:
		struct Mirror {};
		struct Environment {};

		std::variant< PBR, Lambertian, Mirror, Environment > brdf;
	};
    std::unordered_map< std::string, Material > materials;

    /* 
    {
        "type":"ENVIRONMENT",
        "name":"sky",
        "radiance": {"src":"sky.png", "type":"cube", "format":"rgbe"}
    },
    */
    struct Environment{
        std::string name;
        Texture *radiance;
    };
    std::unordered_map< std::string, Environment> environments;

    /* zero or more "LIGHT"s, all with unique names:
    {
        "type":"LIGHT",
        "name":"yellow-sun",
        "tint":[1.0, 1.0, 0.9],
        "sun":{
            "angle":0.00918,
            "strength":10.0
        },
        // xor
        "sphere":{
            "radius":0.1,
            "power":60.0,
            "limit":4.0
        },
        // xor
        "spot":{
            "radius":0.5,
            "power":60.0,
            "fov":0.785,
            "blend":0.15,
            "limit":11.0
        },
        "shadow":256
    },
    */
    struct Light {
		std::string name;

		color tint = color{ .r = 1.0f, .g = 1.0f, .b = 1.0f };

		uint32_t shadow = 0; //optional, if not set will be '0'

		//light has exactly one of these sources:
		struct Sun {
			float angle;
			float strength;
		};
		struct Sphere {
			float radius;
			float power;
			float limit = std::numeric_limits< float >::infinity(); //optional, will be infinity if not specified
		};
		struct Spot {
			float radius;
			float power;
			float limit = std::numeric_limits< float >::infinity(); //optional, will be infinity if not specified
			float fov;
			float blend;
		};
		std::variant< Sun, Sphere, Spot > source;
	};
    std::unordered_map< std::string, Light> lights;
};