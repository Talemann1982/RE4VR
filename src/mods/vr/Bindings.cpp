#include "../VR.hpp"

std::string VR::actions_json = R"(
{
  "actions": [
    {
      "name": "/actions/default/in/HeadsetOnHead",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/SkeletonLeftHand",
      "type": "skeleton",
      "skeleton": "/skeleton/hand/left"
    },
    {
      "name": "/actions/default/in/SkeletonRightHand",
      "type": "skeleton",
      "skeleton": "/skeleton/hand/right"
    },
    {
      "name": "/actions/default/in/Pose",
      "type": "pose"
    },
    {
      "name": "/actions/default/in/Trigger",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/Grip",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/GripValue",
      "type": "vector1"
    },
    {
      "name": "/actions/default/in/GripForce",
      "type": "vector1"
    },
    {
      "name": "/actions/default/in/Touchpad",
      "type": "vector2"
    },
    {
      "name": "/actions/default/in/TouchpadClick",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/TouchpadForce",
      "type": "vector1"
    },
    {
      "name": "/actions/default/in/Joystick",
      "type": "vector2"
    },
    {
      "name": "/actions/default/in/JoystickClick",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/AButton",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/BButton",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/SystemButton",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/Squeeze",
      "type": "vector1"
    },
    {
      "name": "/actions/default/in/Teleport",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/DPad_Up",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/DPad_Right",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/DPad_Down",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/DPad_Left",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/Frame_LeftA",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/Frame_LeftB",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/WeaponDial_Start",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/RE3_Dodge",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/RE2_Quickturn",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/RE2_FirstPerson_Toggle",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/RE2_Reset_View",
      "type": "boolean" 
    },
    {
      "name": "/actions/default/in/RE2_Change_Ammo",
      "type": "boolean" 
    },
    {
      "name": "/actions/default/in/RE2_Toggle_Flashlight",
      "type": "boolean" 
    },
    {
      "name": "/actions/default/in/MiniMap",
      "type": "boolean" 
    },
    {
      "name": "/actions/default/in/Block",
      "type": "boolean"
    },
    {
      "name": "/actions/default/in/Heal",
      "type": "boolean"
    },
    {
      "name": "/actions/default/out/Haptic",
      "type": "vibration"
    }
  ],
  "action_sets": [
    {
      "name": "/actions/default",
      "usage": "single"
    }
  ],
  "default_bindings": [
    {
      "controller_type": "vive",
      "binding_url": "binding_vive.json"
    },
    {
      "controller_type": "vive_controller",
      "binding_url": "bindings_vive_controller.json"
    },
    {
      "controller_type": "oculus_touch",
      "binding_url": "bindings_oculus_touch.json"
    },
    {
      "controller_type": "knuckles",
      "binding_url": "bindings_knuckles.json"
    },
    {
      "controller_type": "rift",
      "binding_url": "binding_rift.json"
    },
    {
      "controller_type": "frame_controller",
      "binding_url": "bindings_frame.json"
    }
  ],
  "localization": []
})";

std::string VR::binding_rift_json = R"(
{
   "alias_info" : {},
   "bindings" : {
      "/actions/default" : {
         "chords" : [],
         "haptics" : [],
         "poses" : [],
         "skeleton" : [],
         "sources" : [
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/headsetonhead"
                  }
               },
               "mode" : "button",
               "path" : "/user/head/proximity"
            }
         ]
      }
   },
   "controller_type" : "rift",
   "description" : "",
   "name" : "rift defaults",
   "options" : {},
   "simulated_actions" : []
})";

std::string VR::bindings_oculus_touch_json = R"(
{
   "alias_info" : {},
   
   "bindings" : {
      "/actions/default" : {
         "chords": [],
         "haptics" : [
            {
               "output" : "/actions/default/out/haptic",
               "path" : "/user/hand/left/output/haptic"
            },
            {
               "output" : "/actions/default/out/haptic",
               "path" : "/user/hand/right/output/haptic"
            }
         ],
         "poses" : [
            {
               "output" : "/actions/default/in/pose",
               "path" : "/user/hand/left/pose/raw"
            },
            {
               "output" : "/actions/default/in/pose",
               "path" : "/user/hand/right/pose/raw"
            }
         ],
         "skeleton" : [
            {
               "output" : "/actions/default/in/skeletonrighthand",
               "path" : "/user/hand/right/input/skeleton/right"
            },
            {
               "output" : "/actions/default/in/skeletonlefthand",
               "path" : "/user/hand/left/input/skeleton/left"
            }
         ],
         "sources" : [
            {
               "inputs" : {
                  "pull" : {
                     "output" : "/actions/default/in/squeeze"
                  }
               },
               "mode" : "trigger",
               "path" : "/user/hand/left/input/grip"
            },
            {
               "inputs" : {
                  "pull" : {
                     "output" : "/actions/default/in/squeeze"
                  }
               },
               "mode" : "trigger",
               "path" : "/user/hand/right/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/trigger"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/trigger"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/weapondial_start"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/trigger"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/trigger"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/trigger"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/grip"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/grip"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/abutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/x"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/abutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/x"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/bbutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/y"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/bbutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/y"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/abutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/a"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/re3_dodge"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/a"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/joystickclick"
                  },
                  "position" : {
                     "output" : "/actions/default/in/joystick"
                  }
               },
               "mode" : "joystick",
               "path" : "/user/hand/left/input/joystick"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/joystickclick"
                  },
                  "position" : {
                     "output" : "/actions/default/in/joystick"
                  }
               },
               "mode" : "joystick",
               "path" : "/user/hand/right/input/joystick"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/bbutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/b"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/systembutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/system"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/systembutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/system"
            }
         ]
      }
   },
   "controller_type" : "oculus_touch",
   "description" : "",
   "name" : "oculus_touch",
   "options" : {},
   "simulated_actions" : []
})";

std::string VR::binding_vive = R"(
{
   "alias_info" : {},
   "bindings" : {
      "/actions/default" : {
         "chords" : [],
         "haptics" : [],
         "poses" : [],
         "skeleton" : [],
         "sources" : [
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/headsetonhead"
                  }
               },
               "mode" : "button",
               "path" : "/user/head/proximity"
            }
         ]
      }
   },
   "controller_type" : "vive",
   "description" : "",
   "name" : "vive defaults",
   "options" : {},
   "simulated_actions" : []
})";

std::string VR::bindings_vive_controller = R"(
{
   "alias_info" : {},
   
   "bindings" : {
      "/actions/default" : {
         "chords": [],
         "haptics" : [
            {
               "output" : "/actions/default/out/haptic",
               "path" : "/user/hand/left/output/haptic"
            },
            {
               "output" : "/actions/default/out/haptic",
               "path" : "/user/hand/right/output/haptic"
            }
         ],
         "poses" : [
            {
               "output" : "/actions/default/in/pose",
               "path" : "/user/hand/left/pose/raw"
            },
            {
               "output" : "/actions/default/in/pose",
               "path" : "/user/hand/right/pose/raw"
            }
         ],
         "skeleton" : [
            {
               "output" : "/actions/default/in/skeletonlefthand",
               "path" : "/user/hand/left/input/skeleton/left"
            },
            {
               "output" : "/actions/default/in/skeletonrighthand",
               "path" : "/user/hand/right/input/skeleton/right"
            }
         ],
         "sources" : [
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/grip"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/bbutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/application_menu"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/joystickclick"
                  },
                  "position" : {
                     "output" : "/actions/default/in/joystick"
                  }
               },
               "mode" : "trackpad",
               "path" : "/user/hand/left/input/trackpad"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/trigger"
                  }
               },
               "mode" : "trigger",
               "path" : "/user/hand/left/input/trigger"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/bbutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/application_menu"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/grip"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/joystickclick"
                  },
                  "position" : {
                     "output" : "/actions/default/in/joystick"
                  }
               },
               "mode" : "trackpad",
               "path" : "/user/hand/right/input/trackpad"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/trigger"
                  }
               },
               "mode" : "trigger",
               "path" : "/user/hand/right/input/trigger"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/systembutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/system"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/systembutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/system"
            }
         ]
      }
   },
   "controller_type" : "vive_controller",
   "description" : "",
   "name" : "vive_controller",
   "options" : {},
   "simulated_actions" : []
})";

std::string VR::bindings_knuckles = R"(
{
   "alias_info" : {},
   
   "bindings" : {
      "/actions/default" : {
         "chords": [],
         "haptics" : [
            {
               "output" : "/actions/default/out/haptic",
               "path" : "/user/hand/left/output/haptic"
            },
            {
               "output" : "/actions/default/out/haptic",
               "path" : "/user/hand/right/output/haptic"
            }
         ],
         "poses" : [
            {
               "output" : "/actions/default/in/pose",
               "path" : "/user/hand/left/pose/raw"
            },
            {
               "output" : "/actions/default/in/pose",
               "path" : "/user/hand/right/pose/raw"
            }
         ],
         "skeleton" : [
            {
               "output" : "/actions/default/in/skeletonrighthand",
               "path" : "/user/hand/right/input/skeleton/right"
            },
            {
               "output" : "/actions/default/in/skeletonlefthand",
               "path" : "/user/hand/left/input/skeleton/left"
            }
         ],
         "sources" : [
            {
               "inputs" : {
                  "force" : {
                     "output" : "/actions/default/in/squeeze"
                  }
               },
               "mode" : "force_sensor",
               "path" : "/user/hand/left/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/trigger"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/trigger"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/weapondial_start"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/trigger"
            },
            {
               "inputs" : {
                  "position" : {
                     "output" : "/actions/default/in/touchpad"
                  }
               },
               "mode" : "trackpad",
               "path" : "/user/hand/left/input/trackpad"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/touchpadclick"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/trackpad"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/joystickclick"
                  },
                  "position" : {
                     "output" : "/actions/default/in/joystick"
                  }
               },
               "mode" : "joystick",
               "path" : "/user/hand/left/input/thumbstick"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/bbutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/b"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/abutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/a"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/grip"
                  }
               },
               "mode" : "button",
               "parameters" : {
                  "click_activate_threshold" : "0.3",
                  "click_deactivate_threshold" : "0.25",
                  "force_input" : "force",
                  "haptic_amplitude" : "0.6"
               },
               "path" : "/user/hand/left/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/abutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/a"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/re3_dodge"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/a"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/bbutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/b"
            },
            {
               "inputs" : {
                  "force" : {
                     "output" : "/actions/default/in/squeeze"
                  }
               },
               "mode" : "force_sensor",
               "path" : "/user/hand/right/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/grip"
                  }
               },
               "mode" : "button",
               "parameters" : {
                  "click_activate_threshold" : "0.3",
                  "click_deactivate_threshold" : "0.25",
                  "force_input" : "force",
                  "haptic_amplitude" : "0.6"
               },
               "path" : "/user/hand/right/input/grip"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/joystickclick"
                  },
                  "position" : {
                     "output" : "/actions/default/in/joystick"
                  }
               },
               "mode" : "joystick",
               "path" : "/user/hand/right/input/thumbstick"
            },
            {
               "inputs" : {
                  "position" : {
                     "output" : "/actions/default/in/touchpad"
                  }
               },
               "mode" : "trackpad",
               "path" : "/user/hand/right/input/trackpad"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/touchpadclick"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/trackpad"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/trigger"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/trigger"
            },
            {
               "inputs" : {
                  "north" : {
                     "output" : "/actions/default/in/teleport"
                  }
               },
               "mode" : "dpad",
               "parameters" : {
                  "sub_mode" : "touch"
               },
               "path" : "/user/hand/left/input/thumbstick"
            },
            {
               "inputs" : {
                  "north" : {
                     "output" : "/actions/default/in/teleport"
                  }
               },
               "mode" : "dpad",
               "parameters" : {
                  "sub_mode" : "touch"
               },
               "path" : "/user/hand/right/input/thumbstick"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/systembutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/left/input/system"
            },
            {
               "inputs" : {
                  "click" : {
                     "output" : "/actions/default/in/systembutton"
                  }
               },
               "mode" : "button",
               "path" : "/user/hand/right/input/system"
            }
         ]
      }
   },
   "controller_type" : "knuckles",
   "description" : "",
   "name" : "knuckles_default",
   "options" : {},
   "simulated_actions" : []
})";

// [STEAM_FRAME 2026-09-28] Steam-Frame-Controller (OpenVR controller_type
// "frame_controller"). Links sitzt statt X/Y ein D-Pad -> auf die DPad-
// Actions (= Xbox-D-Pad; LT + rechter Stick bleibt zusaetzlich). Rechts hat
// er A/B/X/Y wie ein Xbox-Pad: A = rechtes A, B = bisheriges linkes A
// (Frame_LeftA), X = rechtes B (Magdrop/Reload), Y = bisheriges linkes B
// (Frame_LeftB). View (links) und Menu (rechts) = SystemButton.
std::string VR::bindings_frame_controller = R"(
{
   "alias_info": {},
   "bindings": {
      "/actions/default": {
         "chords": [],
         "haptics": [
            {
               "output": "/actions/default/out/haptic",
               "path": "/user/hand/left/output/haptic"
            },
            {
               "output": "/actions/default/out/haptic",
               "path": "/user/hand/right/output/haptic"
            }
         ],
         "poses": [
            {
               "output": "/actions/default/in/pose",
               "path": "/user/hand/left/pose/raw"
            },
            {
               "output": "/actions/default/in/pose",
               "path": "/user/hand/right/pose/raw"
            }
         ],
         "skeleton": [
            {
               "output": "/actions/default/in/skeletonrighthand",
               "path": "/user/hand/right/input/skeleton/right"
            },
            {
               "output": "/actions/default/in/skeletonlefthand",
               "path": "/user/hand/left/input/skeleton/left"
            }
         ],
         "sources": [
            {
               "inputs": {
                  "pull": {
                     "output": "/actions/default/in/squeeze"
                  }
               },
               "mode": "trigger",
               "path": "/user/hand/left/input/grip"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/grip"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/grip"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/trigger"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/trigger"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/joystickclick"
                  },
                  "position": {
                     "output": "/actions/default/in/joystick"
                  }
               },
               "mode": "joystick",
               "path": "/user/hand/left/input/thumbstick"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/systembutton"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/system"
            },
            {
               "inputs": {
                  "pull": {
                     "output": "/actions/default/in/squeeze"
                  }
               },
               "mode": "trigger",
               "path": "/user/hand/right/input/grip"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/grip"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/grip"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/trigger"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/trigger"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/joystickclick"
                  },
                  "position": {
                     "output": "/actions/default/in/joystick"
                  }
               },
               "mode": "joystick",
               "path": "/user/hand/right/input/thumbstick"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/systembutton"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/system"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/weapondial_start"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/trigger"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/abutton"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/a"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/re3_dodge"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/a"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/frame_lefta"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/b"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/bbutton"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/x"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/frame_leftb"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/y"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/dpad_up"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/dpad_up"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/dpad_down"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/dpad_down"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/dpad_left"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/dpad_left"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/dpad_right"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/dpad_right"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/systembutton"
                  }
               },
               "mode": "button",
               "path": "/user/hand/left/input/view"
            },
            {
               "inputs": {
                  "click": {
                     "output": "/actions/default/in/systembutton"
                  }
               },
               "mode": "button",
               "path": "/user/hand/right/input/menu"
            }
         ]
      }
   },
   "controller_type": "frame_controller",
   "description": "",
   "name": "frame_controller_default",
   "options": {},
   "simulated_actions": []
})";
