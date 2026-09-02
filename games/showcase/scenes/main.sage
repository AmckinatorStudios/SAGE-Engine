{
  "name": "Showcase",
  "sage_scene_version": 5,
  "lighting": {
    "ambientStrength": 0.2,
    "ambientColor": {
      "x": 0.5,
      "y": 0.56,
      "z": 0.66
    },
    "ambientSky": {
      "x": 0.42,
      "y": 0.56,
      "z": 0.82
    },
    "ambientGround": {
      "x": 0.26,
      "y": 0.25,
      "z": 0.24
    },
    "sun": {
      "direction": {
        "x": -0.55,
        "y": -0.52,
        "z": -0.44
      },
      "color": {
        "x": 1.0,
        "y": 0.9,
        "z": 0.74
      },
      "intensity": 2.7
    },
    "fog": {
      "enabled": true,
      "color": {
        "x": 0.78,
        "y": 0.76,
        "z": 0.72
      },
      "start": 100.0,
      "end": 320.0
    },
    "skybox": {
      "enabled": true,
      "cubemapDir": "",
      "intensity": 1.0,
      "rotation": 0.0,
      "top": {
        "x": 0.22,
        "y": 0.4,
        "z": 0.74
      },
      "horizon": {
        "x": 0.86,
        "y": 0.82,
        "z": 0.74
      }
    },
    "pointLights": [],
    "spotLights": [],
    "shadows": {
      "distance": 90.0
    }
  },
  "objects": [
    {
      "id": 1,
      "name": "World",
      "position": {
        "x": 0,
        "y": 0,
        "z": 0
      },
      "rotation": {
        "x": 0,
        "y": 0,
        "z": 0
      },
      "scale": {
        "x": 1,
        "y": 1,
        "z": 1
      },
      "color": {
        "x": 1,
        "y": 1,
        "z": 1
      },
      "mesh": {
        "type": "none",
        "path": ""
      },
      "script": "assets/scripts/world.lua"
    },
    {
      "id": 2,
      "name": "Player",
      "position": {
        "x": 0,
        "y": 1.0,
        "z": -6.0
      },
      "rotation": {
        "x": 0,
        "y": 0,
        "z": 0
      },
      "scale": {
        "x": 1,
        "y": 1,
        "z": 1
      },
      "color": {
        "x": 1,
        "y": 1,
        "z": 1
      },
      "mesh": {
        "type": "none",
        "path": ""
      }
    },
    {
      "id": 3,
      "name": "Player Camera",
      "parent": 2,
      "position": {
        "x": 0,
        "y": 1.65,
        "z": 0
      },
      "rotation": {
        "x": -4,
        "y": 0,
        "z": 0
      },
      "scale": {
        "x": 1,
        "y": 1,
        "z": 1
      },
      "color": {
        "x": 1,
        "y": 1,
        "z": 1
      },
      "mesh": {
        "type": "none",
        "path": ""
      },
      "camera": {
        "projection": "perspective",
        "fov": 72.0,
        "near": 0.08,
        "far": 400.0,
        "orthoHeight": 10.0,
        "primary": true
      }
    }
  ]
}