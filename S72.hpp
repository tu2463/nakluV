#include "sejp.hpp"

#include <string>
#include <vector>
#include <optional>

struct S72 {
    //NOTE: redefine these for your vector and quaternion types of choice:
    // we use this for easy substitution (in the future, can easily switch to e.g. using vec2 = glm::vec3)
	using vec3 = struct vec3_internal{ float x, y, z; };
	using quat = struct quat_internal{ float x, y, z, w; };

    static S72 load(std::string const &file);

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
        Camera *camera = nullptr;

        // optional, null of not specified:
        Mesh *mesh = nullptr;
        Camera *camera = nullptr;
        Environment *environment = nullptr;
        Light *light = nullptr;
    };
    std::unordered_map< std::string, Node> nodes;

    struct Mesh{

    };
    Mesh mesh;

    struct Camera{

    };
    Camera camera;

    struct Environment{

    };
    Environment environment;

    struct Light{

    };
    Light light;
};