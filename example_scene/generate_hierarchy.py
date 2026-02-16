import sys
import json

def generate_hierarchy_scene(num_nodes, output_file):
    objects = []

    # Camera
    objects.append({
        "type": "CAMERA",
        "name": "Camera",
        "perspective": {
            "aspect": 1.77778,
            "vfov": 1.0,
            "near": 0.1,
            "far": 1000
        }
    })

    objects.append({
        "type": "NODE",
        "name": "Camera",
        "translation": [0, 0, max(50, num_nodes * 0.15)],
        "rotation": [0, 0, 0, 1],
        "scale": [1, 1, 1],
        "camera": "Camera"
    })

    # Material
    objects.append({
        "type": "MATERIAL",
        "name": "lambertian:Red",
        "lambertian": {
            "albedo": [0.8, 0.2, 0.1]
        }
    })

    # Mesh (uses existing cube from materials scene)
    objects.append({
        "type": "MESH",
        "name": "Cube",
        "topology": "TRIANGLE_LIST",
        "count": 36,
        "attributes": {
            "POSITION": {"src": "materials.Cube.pnTt.b72", "offset": 0, "stride": 48, "format": "R32G32B32_SFLOAT"},
            "NORMAL": {"src": "materials.Cube.pnTt.b72", "offset": 12, "stride": 48, "format": "R32G32B32_SFLOAT"},
            "TANGENT": {"src": "materials.Cube.pnTt.b72", "offset": 24, "stride": 48, "format": "R32G32B32A32_SFLOAT"},
            "TEXCOORD": {"src": "materials.Cube.pnTt.b72", "offset": 40, "stride": 48, "format": "R32G32_SFLOAT"}
        },
        "material": "lambertian:Red"
    })

    # Generate nodes in reverse order (leaf to root) so children are defined before parents
    for i in range(num_nodes - 1, -1, -1):
        node = {
            "type": "NODE",
            "name": f"Node_{i}",
            "translation": [0.1, 0.1, 0.1] if i > 0 else [0, 0, 0],
            "rotation": [0, 0, 0, 1],
            "scale": [1, 1, 1],
            "mesh": "Cube"
        }
        if i < num_nodes - 1:
            node["children"] = [f"Node_{i + 1}"]
        objects.append(node)

    # Scene
    objects.append({
        "type": "SCENE",
        "name": f"hierarchy-{num_nodes}",
        "roots": ["Camera", "Node_0"]
    })

    # Write output
    output = ["s72-v2"] + objects
    with open(output_file, 'w') as f:
        f.write(json.dumps(output, indent='\t'))

    print(f"Generated {output_file} with {num_nodes} nodes in hierarchy")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python generate_hierarchy.py <num_nodes> <output_file>")
        print("Example: python generate_hierarchy.py 1000 hierarchy-1000.s72")
        sys.exit(1)

    num_nodes = int(sys.argv[1])
    output_file = sys.argv[2]
    generate_hierarchy_scene(num_nodes, output_file)
