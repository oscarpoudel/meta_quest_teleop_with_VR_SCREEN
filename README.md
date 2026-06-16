# Meta Quest Teleop with VR Camera Screen

This repository extends [rail-berkeley/oculus_reader](https://github.com/rail-berkeley/oculus_reader) with:
- A **head-locked camera overlay screen** rendered inside the Quest VR headset
- A **PC-side TCP JPEG stream server** (`camera_stream_server.py`) for sending camera images to the Quest
- **Bidirectional data flow**: PC sends camera frames to the Quest, and the Quest sends controller/hand transforms and button states back to the PC

The system consists of two parts:
1. **APK** (`app_source/`) - Native C++ Oculus Mobile SDK app running on the Quest that renders VR scenes, a camera overlay panel, and streams controller data via ADB logcat
2. **Python reader** (`meta_quest_teleop/`) - Reads the controller data over ADB and provides a Python API for transforms and button states

## Coordinate Systems: ROS vs OpenXR

This library works with two different coordinate systems that are important to understand:

### OpenXR Coordinate System
The Meta Quest device natively uses the **OpenXR coordinate system**:
- **X-axis**: Points to the right
- **Y-axis**: Points up
- **Z-axis**: Points backward (away from the user)

This is the coordinate system used internally by the Meta Quest tracking system and is what you get when calling `get_hand_controller_transform_openxr()`.

### ROS Coordinate System
ROS (Robot Operating System) uses a different convention:
- **X-axis**: Points forward
- **Y-axis**: Points left
- **Z-axis**: Points up

This is the standard coordinate system used in ROS and is what you get when calling `get_hand_controller_transform_ros()`.

### Conversion Between Systems
The conversion from OpenXR to ROS coordinates is performed using a static rotation quaternion `[0.5, -0.5, -0.5, 0.5]`. This transformation:
- Rotates X from right → forward
- Rotates Y from up → left
- Rotates Z from backward → up

### Usage in Code
- **For ROS integration**: Use `get_hand_controller_transform_ros()` to get transforms already converted to ROS coordinates
- **For OpenXR/native data**: Use `get_hand_controller_transform_openxr()` to get transforms in the native OpenXR coordinate system
- **For TF publishing**: The `ros2_tf_publisher.py` node publishes transforms in the `meta_world` frame (OpenXR coordinates) and uses a static transform to link to the ROS `map` frame, allowing tf2 to handle coordinate conversions automatically

When working with transforms, always be aware of which coordinate system you're using. See the docstrings in the code for specific coordinate system information for each function.

## Clone the repository

To pull the APK correctly, Git LFS has to be configured before cloning the repository. The installation is described here https://git-lfs.github.com. On Ubuntu follow these steps:

```bash
curl -s https://packagecloud.io/install/repositories/github/git-lfs/script.deb.sh | sudo bash
sudo apt-get install git-lfs
git lfs install # has to be run only once on a single user account
```

Now you can clone this repository either with HTTPS or SSH.

## Setup of the ADB

[ADB](https://developer.android.com/studio/command-line/adb) is required for the communication between Oculus Quest and the python reader script.

To install ADB on Ubuntu run:
```
sudo apt install android-tools-adb
```

for mac, run:
```
brew install android-platform-tools
```

<details>
<summary>Instructions for new Meta Quest Device (Only run once after purchasing the Quest)</summary>

1. Determine your Meta Quest account name:
If you haven't used Meta Quest before, start it and follow the steps to create your profile and get yourself started. Otherwise follow these steps to find out your username:
    1. Go to: [https://www.oculus.com/](https://www.oculus.com/)
    2. Log in to account:
    ![image_0](https://user-images.githubusercontent.com/14967831/106832581-c7288f00-6646-11eb-91e0-3b74e81a58ba.png)
    3. After logging in **select your profile again** in top right corner and select **'Profile'**
    ![image_1](https://user-images.githubusercontent.com/14967831/106832585-c859bc00-6646-11eb-9a3d-3a55f844ee37.png)
    4. You will be able to see your username on the following screen:
    ![image_2](https://user-images.githubusercontent.com/14967831/106832678-f7702d80-6646-11eb-823e-1001d6bffe01.png)
2. Enable Meta Quest development mode:
    1. If you belong to RAIL, inform me (Jedrzej Orbik) that you need to join the development organization. This is required to activate debugging mode on the device. Otherwise create your own organization <https://developer.oculus.com/manage/organizations/create/> and fill in the appropriate information.
    2. Turn on the device you want to use for development.
    3. Open the Meta app on your phone and then go to **Settings**.
    4. Tap the device and then go to **More Settings** > **Developer Mode**.
    5. Turn on the **Developer Mode** toggle.
    6. Connect your device to your computer using a USB-C cable and then wear the device.
    7. Accept **Allow USB Debugging** and **Always allow from this computer** when prompted to on the device.
        ![image_3](https://user-images.githubusercontent.com/14967831/104061507-048d2e80-51f9-11eb-8327-7917f6a1ab60.png)

</details>

## Dependencies

Install Python dependencies:
```bash
pip install numpy scipy pure-python-adb
```

## How to run

The `MetaQuestReader` handles ADB connection, APK installation, and data reading automatically.

### Basic usage (USB, no camera)

```python
from meta_quest_teleop.reader import MetaQuestReader

reader = MetaQuestReader()

# Get transforms in OpenXR coordinates
head = reader.get_head_transform_openxr()
right_grip = reader.get_grip_transform_openxr("right")
left_grip = reader.get_grip_transform_openxr("left")

# Get button states
a_pressed = reader.get_button_state("A")
grip_value = reader.get_grip_value("right")
trigger_value = reader.get_trigger_value("right")
joystick = reader.get_joystick_value("right")
```

### With camera (requires ADB over network)

The Quest APK can display a head-locked camera overlay. The PC runs a TCP JPEG stream server and the Quest connects to it.

#### Step 1: Get the Quest IP address

Connect the Quest via USB, then:
```bash
adb shell ip route
```
Look for the IP after `src`, e.g. `10.0.32.101`.

#### Step 2: Start the camera stream server on PC

Pick the appropriate camera source:

**RealSense camera:**
```bash
python camera_stream_server.py \
  --source realsense \
  --view-names color,depth \
  --host 0.0.0.0 \
  --port 5566
```

**ZMQ camera (from gear-sonic pipeline):**
```bash
python camera_stream_server.py \
  --source zmq \
  --zmq-ip localhost \
  --zmq-port 5555 \
  --zmq-keys head,ego_view \
  --port 5566
```

**USB camera (OpenCV):**
```bash
python camera_stream_server.py \
  --source opencv \
  --opencv-devices 0,1 \
  --view-names left,right \
  --port 5566
```

#### Step 3: Restart ADB in TCP mode (one time after USB connect)

```bash
adb tcpip 5555
```
You can now disconnect the USB cable.

#### Step 4: Run the reader with camera

```python
from meta_quest_teleop.reader import MetaQuestReader

reader = MetaQuestReader(
    ip_address="<QUEST_IP_ADDRESS>",  # from step 1
    camera_host="<PC_IP_ADDRESS>",    # IP of the machine running camera_stream_server.py
    camera_port=5566,
)
```

The APK will connect to the PC camera server and display the video feed as a head-locked overlay in VR.

### Switching camera views

While wearing the headset:
- **A + B** buttons → switch to next camera view
- **X + Y** buttons → switch to previous camera view

### Stopping the app

```bash
adb shell am force-stop com.rail.oculus.teleop
```

## Camera Streaming Protocol

Server to Quest:
```
uint32_be header_len
header_len bytes JSON header
uint32_be jpeg_len
jpeg_len bytes JPEG
```

Quest to server (optional ASCII commands):
```
VIEW 0\n
VIEW head\n
NEXT\n
PREV\n
```

Each Quest TCP client has independent view tracking, so multiple headsets can switch streams independently.

The JSON header contains: `protocol`, `frame_id`, `timestamp`, `view_index`, `view_name`, `views`, `encoding`, `width`, `height`.

## Data sent from Quest to PC

The APK streams controller data via ADB logcat (tag: `wE9ryARX`). The `MetaQuestReader` parses this into transforms and button states:

- **Head pose**: `4×4` transform matrix
- **Per hand (left/right)**:
  - Grip transform (`lg`/`rg`)
  - Model transform (`lm`/`rm`)
  - Pointer transform (`lp`/`rp`)
  - Button states (A, B, X, Y, joystick click, thumb-up)
  - Analog values (trigger, grip, joystick `x`/`y`)
- **Telemetry**: linear/angular velocities, pose confidence, pinch state, device IDs

## Building the APK from source

See `app_source/README.md` for build instructions. The APK is compiled using the Oculus Mobile SDK v1.50.0.

## ROS2 Visualization

A ROS2 TF publisher is available in `ros_visualiser/`:
```bash
python ros_visualiser/ros2_tf_publisher.py
```

This publishes all hand transforms as TF frames in the `meta_world` frame. A Docker setup is also provided.
