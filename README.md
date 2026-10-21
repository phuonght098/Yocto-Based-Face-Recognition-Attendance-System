# Yocto-Based Face Recognition Attendance System

An embedded face recognition attendance system built with the Yocto Project for Raspberry Pi 4B. The system integrates a Wi-Fi RTSP camera, on-device face detection and recognition, a PIR motion sensor, attendance logging, and an SPI TFT display.

The project provides a custom Yocto layer, `meta-phuong`, containing application recipes, system configuration, device-tree overlays, and kernel configuration required to build and deploy the system.
<img width="1600" height="720" alt="9e099f21f50775592c16" src="https://github.com/user-attachments/assets/a6f36716-1ae6-48d3-837f-6a40f13eb322" />
<img width="1851" height="1008" alt="image" src="https://github.com/user-attachments/assets/b6c07fa9-d143-4fc1-9d84-1f2fae6bea3d" />

## 1. Features

- **Embedded face recognition:** Uses OpenCV, YuNet face detection, SFace face recognition, and ONNX Runtime.
- **Attendance logging:** Records recognized employees and attendance timestamps.
- **Web dashboard:** Provides browser-based access to attendance information and CSV export.
- **Backup and restore:** Supports backing up and restoring attendance-related data.
- **Performance reporting:** Collects performance measurements for analysis.
- **Custom Yocto integration:** Packages applications, dependencies, configuration files, and services into the target image.

## 2. Hardware and Software Requirements

### Hardware

- Raspberry Pi 4 Model B
- microSD card
- Wi-Fi RTSP camera
- PIR motion sensor
- SPI TFT display
- Network connection for the Raspberry Pi and camera

### Software

The build configuration used for this project includes:

- Yocto release: Kirkstone
- Poky version: 4.0.x
- Machine: `raspberrypi4-64`
- Image: `core-image-weston`


## 3. Repository Structure and Recipes

The custom layer is organized as follows:

```text
meta-phuong/
├── conf/
│   └── layer.conf
│
├── recipes-ai/
│   ├── face-app/
│   │   ├── face-app_1.0.bb
│   │   └── files/
│   │       ├── face_ai.cpp
│   │       └── CMakeLists.txt
│   │
│   ├── model/
│   │   ├── face-models_1.0.bb
│   │   └── files/
│   │       ├── face_detection_yunet_2023mar.onnx
│   │       └── face_recognition_sface_2021dec.onnx
│   │
│   └── onnxruntime/
│       └── onnxruntime_1.18.1.bb
│
├── recipes-apps/
│   ├── attendance-app/
│   │   ├── attendance-app_1.0.bb
│   │   └── files/
│   │       ├── attendance_app.cpp
│   │       └── attendance-app.init
│   │
│   ├── attendance-web/
│   │   ├── attendance-web_1.0.bb
│   │   └── files/
│   │       ├── app.py
│   │       ├── attendance-web.conf
│   │       └── attendance-web.init
│   │
│   └── wifi-camera/
│       ├── wifi-camera_1.0.bb
│       └── files/
│           ├── wifi_camera.c
│           └── wifi-camera.conf
│
├── recipes-connectivity/
│   ├── networkmanager/
│   └── phuong-wifi/
│
├── recipes-core/
│   └── images/
│       └── core-image-weston.bbappend
│
├── recipes-kernel/
│   ├── device-tree/
│   │   ├── attendance-st7735r-overlay.bb
│   │   └── files/
│   │       └── attendance-st7735r-overlay.dts
│   │
│   └── linux/
│       ├── linux-raspberrypi_%.bbappend
│       └── files/
│           └── st7735r.cfg
│
└── recipes-sensors/
    └── pir-monitor/
        ├── pir-monitor_1.0.bb
        └── files/
            ├── pir_monitor.c
            └── pir-monitor.service
```

### Recipe Descriptions

| Recipe | Purpose |
|---|---|
| `face-app` | Builds and installs the C++ face detection and recognition application. |
| `face-models` | Installs the YuNet face detection and SFace face recognition ONNX models. |
| `onnxruntime` | Builds and packages ONNX Runtime for inference on the target device. |
| `attendance-app` | Builds the attendance management application, including backup, restore, and performance reporting. |
| `attendance-web` | Installs the Python web dashboard and its configuration and startup files. |
| `wifi-camera` | Builds the camera configuration utility and installs `/etc/wifi-camera.conf`. |
| `phuong-wifi` | Provides project-specific Wi-Fi configuration integration. |
| `networkmanager` | Applies project-specific NetworkManager integration or configuration. |
| `core-image-weston.bbappend` | Extends the base image to include the project's required packages and components. |
| `attendance-st7735r-overlay` | Builds and installs the SPI TFT device-tree overlay. |
| `linux-raspberrypi_%.bbappend` | Extends the Raspberry Pi kernel recipe with project-specific configuration. |
| `pir-monitor` | Builds and installs the PIR sensor monitoring application and its service definition. |

The recipes are responsible for integrating the applications and hardware support into the Yocto build. Runtime configuration files should be modified on the target only when their respective application supports the changes.

## 4. Build Environment Setup

### Initialize the Yocto Environment

Set up the Yocto build environment and initialize the build directory.


```bash
cd ~/yocto
source poky/oe-init-build-env build
```

### Configure the Target Machine

Ensure the build configuration targets the Raspberry Pi 4B.

In `conf/local.conf`:

```bitbake
MACHINE = "raspberrypi4-64"
```
###  Add the Custom Layer

Add `meta-phuong` to the build configuration.

From the initialized build environment:

```bash
bitbake-layers add-layer ../meta-phuong
```


## 5. Build, Install, Configure, and Operate the System

### Build the Image

Build the target image:

```bash
bitbake core-image-weston
```


###  Verify Installed Applications

On the Raspberry Pi, check the available application commands:

```bash
/usr/bin/attendance-app --help
```

The attendance application supports the following command-line operations:

```text
/usr/bin/attendance-app
/usr/bin/attendance-app --backup [BACKUP_ROOT]
/usr/bin/attendance-app --restore BACKUP_DIR
/usr/bin/attendance-app --perf-report
/usr/bin/attendance-app --help
```


### Configure the Wi-Fi Camera

Edit the camera configuration:

```bash
vi /etc/wifi-camera.conf
```

Set the RTSP URL to match the camera's address and stream path.

For example:

```bash
RTSP_URL="rtsp://USERNAME:PASSWORD@CAMERA_IP:554/stream2"
```

Replace the example credentials, IP address, and stream path with the actual camera settings.

After updating the configuration, restart the relevant camera or attendance service if required by the installed implementation.

Verify that the camera is reachable and that the RTSP stream can be opened before testing face recognition.


###  Check Attendance Data

Attendance records are stored at:

```text
/var/lib/face-ai/attendance.csv
```

Performance measurements are stored at:

```text
/var/lib/face-ai/performance.csv
```

### Configure the PIR Sensor

The PIR monitor communicates motion events through:

```text
/run/pir-monitor/events.sock
```

The current hardware configuration uses GPIO 17 on `gpiochip0`.

Verify that the GPIO mapping matches the actual wiring and device-tree configuration before operating the sensor.

### Configure the SPI TFT Display

The custom layer provides the display device-tree overlay and kernel configuration.

The overlay includes the compatible string:

```text
jianda,jd-t18003-t01
```

### Access the Web Dashboard

The web application is provided by the `attendance-web` recipe.

Inspect its installation and configuration:

```bash
ls -l /etc/attendance-web.conf
```

The existing configuration file is:

```text
/etc/attendance-web.conf
```


Determine the configured listening address and port from the application and its installed configuration. Then access the dashboard from a browser using the Raspberry Pi's reachable IP address and configured port.

### Back Up Attendance Data

Create a backup using the attendance application:

```bash
/usr/bin/attendance-app --backup
```

### Restore Attendance Data

Restore from an existing backup directory:

```bash
/usr/bin/attendance-app --restore /path/to/backup-directory
```

A restore operation may overwrite existing records. Perform this operation only when the selected backup is correct.

### Generate a Performance Report

Run:

```bash
/usr/bin/attendance-app --perf-report
```

The application reports performance information collected by the system.

The underlying performance data is stored at:

```text
/var/lib/face-ai/performance.csv
```

Use the report to investigate processing delays, resource constraints, and changes in runtime behavior.

## 6. Testing and Performance

The following results were reported during project testing:

| Test | Reported result |
|---|---|
| Average boot-to-service-ready time | 30 seconds over 50 trials |
| Continuous operation | 8 days |
| Reboot testing | 50 reboot tests |
| Power interruption testing | 20 interruptions while running |
| Wi-Fi disconnect and recovery | 50 tests |
| Face recognition rate | 96% preliminary result |

### Performance Evaluation

| Metric | Result |
|---|---:|
| Yocto image file size | 185 MB |
| Root filesystem size | 803.4 MB |
| Average CPU usage at idle | 1.86% |
| Average CPU usage during recognition | Approximately 34% |
| Peak CPU usage during recognition | 58% |
| Average system RAM usage at idle | 212 MB |
| Peak system RAM usage during recognition | 235 MB |
| Average RTSP camera stream frame rate | 15.02 FPS |
| Number of recognition performance samples | 400 |
| Average recognition processing time | 4,491 ms |



