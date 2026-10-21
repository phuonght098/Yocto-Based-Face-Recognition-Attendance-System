# Yocto-Based Face Recognition Attendance System

An embedded face recognition attendance system built with the Yocto Project for Raspberry Pi 4B. The system integrates a Wi-Fi RTSP camera, on-device face detection and recognition, a PIR motion sensor, attendance logging, and an SPI TFT display.

The project provides a custom Yocto layer, `meta-phuong`, containing application recipes, AI model packaging, network configuration, sensor integration, and kernel/device-tree configuration.

## 1. Project Overview

The objective of this project is to build a dedicated embedded Linux platform for face recognition-based attendance tracking.

Instead of installing applications manually on a general-purpose Linux distribution, the system uses Yocto to build a customized Linux image for Raspberry Pi 4B. Application components and hardware-specific configuration are integrated through BitBake recipes in the custom `meta-phuong` layer.

### Key Features

- Custom embedded Linux image built with Yocto Project
- PIR motion detection through GPIO, SPI TFT display integration
- Attendance and performance logging using CSV files
- Command-line backup, restore, and performance reporting
- Web dashboard for viewing attendance records and exporting CSV data


## 2. Hardware and Software

### 2.1 Hardware

| Component           | Purpose                                             |
|---------------------|-----------------------------------------------------|
| Raspberry Pi 4B     | Main embedded computing platform                    |
| Wi-Fi IP camera     | Provides an RTSP video stream                       |
| HC-SR501 PIR sensor | Detects motion and triggers the attendance workflow |
| SPI TFT display     | Displays system status and attendance messages      |
| MicroSD card        | Stores the embedded Linux image and runtime data    |
| Wi-Fi network       | Connects the device to the camera and local network |

The camera must provide an RTSP stream accessible from the Raspberry Pi. The camera address, credentials, and stream path depend on the actual camera configuration.

The PIR application is configured to read GPIO 17 on `gpiochip0`.

### 2.2 Software Stack

| Component                   | Technology                          |
|-----------------------------|-------------------------------------|
| Yocto release series        | Kirkstone                           |
| Target machine              | `raspberrypi4-64`                   |
| Image target                | `core-image-weston`                 |


## 3. System Architecture

### 3.1 Architecture Diagram



### 3.2 Attendance Workflow

1. The PIR monitor detects motion through GPIO.
2. The PIR application publishes a motion event through a local Unix-domain socket.
3. The attendance controller receives the event and coordinates the recognition workflow.
4. The face recognition application obtains video frames from the configured RTSP camera.
5. YuNet detects faces in the video frames.
6. SFace generates face embeddings, which are compared with locally stored face embeddings.
7. The attendance controller records attendance information in CSV files.
8. The controller updates the system status displayed through the SPI TFT framebuffer.
9. The Web dashboard reads attendance data and makes it available for monitoring and CSV export.


## 4. Repository Structure

The custom layer is located at:

```text
meta-phuong/
├── conf/
│   └── layer.conf
├── recipes-ai/
│   ├── face-app/
│   ├── model/
│   └── onnxruntime/
├── recipes-apps/
│   ├── attendance-app/
│   ├── attendance-web/
│   └── wifi-camera/
├── recipes-connectivity/
│   ├── networkmanager/
│   └── phuong-wifi/
├── recipes-core/
│   └── images/
├── recipes-kernel/
│   ├── device-tree/
│   └── linux/
├── recipes-sensors/
│   └── pir-monitor/

```

### recipes-ai/

Contains recipes related to face recognition and AI inference.

- **face-app/** — Builds the C++ face recognition application, installed as `face-ai`.
- **model/** — Packages the YuNet face detection and SFace face recognition ONNX models.
- **onnxruntime/** — Provides the ONNX Runtime 1.18.1 recipe.

### recipes-apps/

Contains the primary application components.

- **attendance-app/** — C++ attendance controller, including attendance logging, performance reporting, and backup/restore operations.
- **attendance-web/** — Python HTTP application for viewing attendance information.
- **wifi-camera/** — C application using GStreamer to connect to an RTSP stream and monitor video-frame activity.

### recipes-connectivity/

Contains network-related integration.

- **networkmanager/** — Extends the NetworkManager recipe with project-specific configuration.
- **phuong-wifi/** — Packages NetworkManager Wi-Fi connection profiles.


### recipes-core/

Contains image-level configuration.

- **images/core-image-weston.bbappend** — Extends the `core-image-weston` image with project-specific packages and configuration.

### recipes-kernel/

Contains kernel and display integration.

- **device-tree/** — Packages the TFT Device Tree overlay.
- **linux/** — Extends the Raspberry Pi Linux kernel recipe and provides a kernel configuration fragment for the display.

The overlay source uses the compatible string `jianda,jd-t18003-t01`. The actual driver binding and framebuffer behavior should be verified on the target hardware.

### recipes-sensors/

Contains the PIR motion monitoring application.

- **pir-monitor/** — Builds the C application and installs its service definition.


## 5. Build Instructions

###  Prerequisites

Prepare a Yocto build environment containing:

- Poky for the Kirkstone release
- Required OpenEmbedded layers, including `meta-oe`, `meta-python`, `meta-networking` and `meta-raspberrypi`
- The custom `meta-phuong` layer


###  Initialize the Build Environment

From the Yocto workspace:

```bash
cd ~/yocto
source poky/oe-init-build-env build
```

###  Configure the Target Machine

Edit `conf/local.conf` and verify:

```bash
MACHINE = "raspberrypi4-64"
```

### Add the Custom Layer


```bash
bitbake-layers add-layer ../poky/meta-phuong
```


### Build the Image

```bash
bitbake core-image-weston
```


The image is normally located under:

```text
tmp/deploy/images/raspberrypi4-64/
```

The project has produced the following image artifact:

```text
core-image-weston-raspberrypi4-64.wic.bz2
```

Use the exact filename generated by your build.


## 6. Installation and Configuration

### Configure the Wi-Fi Camera

The camera configuration file is:

```text
/etc/wifi-camera.conf
```

Example format:

```bash
RTSP_URL="rtsp://USERNAME:PASSWORD@CAMERA_IP:554/stream2"
```

Replace the example values with the correct camera credentials, IP address, port, and stream path. The `face-ai` application reads this file by default. The standalone `wifi-camera` application also uses it by default.


### Configure Wi-Fi

The `phuong-wifi` recipe installs NetworkManager connection profiles under:

```text
/etc/NetworkManager/system-connections/
```

## 7. Runtime Components

### 7.1 face-ai

The face recognition executable is installed at:

```text
/usr/bin/face-ai
```

**Responsibilities:**

- Connecting to the RTSP camera
- Decoding video frames through GStreamer
- Detecting faces with YuNet
- Generating face embeddings with SFace
- Comparing embeddings with the local face registry
- Retrying after camera connection or frame-reading failures

**Models** are installed under:

```text
/usr/share/face-recognition/models/
```

Expected model files:

- `face_detection_yunet_2023mar.onnx`
- `face_recognition_sface_2021dec.onnx`

Default face registry:

```text
/var/lib/face-ai/faces.txt
```

### 7.2 attendance-app

The attendance controller is installed at:

```text
/usr/bin/attendance-app
```

**Supported command-line options:**

```bash
/usr/bin/attendance-app
/usr/bin/attendance-app --backup [BACKUP_ROOT]
/usr/bin/attendance-app --restore BACKUP_DIR
/usr/bin/attendance-app --perf-report
/usr/bin/attendance-app --help
```

The application coordinates the attendance workflow, manages CSV data, supports backup/restore operations, and reports performance information.

### 7.3 pir-monitor

The PIR monitor reads GPIO17 on `gpiochip0`.

It publishes newline-delimited JSON motion events through:

```text
/run/pir-monitor/events.sock
```

The application includes filtering and cooldown logic to reduce repeated motion events.

The recipe installs a systemd unit for the PIR monitor. The attendance controller uses a SysV init script. Startup behavior should be verified against the init system selected for the final image.

### 7.4 wifi-camera

The standalone camera application is built in C with GStreamer.

Its purpose is to connect to an RTSP stream, decode video, report frame activity, and retry failed sessions. It is useful for camera-pipeline diagnostics.

It is separate from the `face-ai` recognition application.

### 7.5 attendance-web

The Web application is installed at:

```text
/usr/bin/attendance-web
```

It uses Python's standard-library HTTP server and reads configuration from:

```text
/etc/attendance-web.conf
```

The current application provides **read-only** monitoring and attendance-data access.

| Endpoint       | Purpose                                              |
|----------------|------------------------------------------------------|
| `/`            | Dashboard and recent attendance information          |
| `/employees`   | Names inferred from attendance records and record counts |
| `/attendance`  | Attendance history                                   |
| `/export.csv`  | CSV export                                           |
| `/health`      | Basic HTTP health response                           |

The `/employees` does **not** represent a separate employee database. The current Web application does **not** provide employee write operations, face enrollment, camera configuration editing, Web-based backup/restore, or role-based user management.

## 8. Usage and Diagnostics

Run these commands on the Raspberry Pi target.

### 8.1 Check Application Help

```bash
/usr/bin/attendance-app --help
/usr/bin/face-ai --help
```

### 8.2 Register a Face

Register a face using a name:

```bash
/usr/bin/face-ai --register "Employee Name"
```

To specify configuration and registry paths explicitly:

```bash
/usr/bin/face-ai \
  --register "Employee Name" \
  --config /etc/wifi-camera.conf \
  --db /var/lib/face-ai/faces.txt
```

Ensure that the person is visible to the camera during registration.

This operation stores a face embedding associated with a name. It does **not** create a complete employee profile or a Web user account.

### 8.3 Run Face Recognition

```bash
/usr/bin/face-ai
```

For controlled testing, the matching threshold can be specified explicitly:

```bash
/usr/bin/face-ai --threshold 0.40
```

The threshold affects matching behavior. Validate any change against the camera, lighting conditions, enrolled identities, and test dataset.

### 8.4 Generate a Performance Report

```bash
/usr/bin/attendance-app --perf-report
```

### 8.5 Inspect Runtime State

```bash
ps | grep -E 'attendance-app|face-ai|pir-monitor|attendance-web'
ls -l /run/pir-monitor/events.sock
ls -l /var/lib/face-ai/
```

Check the camera configuration locally without publishing its contents:

```bash
test -s /etc/wifi-camera.conf && echo "Camera configuration exists"
```

### 8.6 Data Files

| File or Directory                  | Purpose                  |
|------------------------------------|--------------------------|
| `/var/lib/face-ai/faces.txt`       | Face embedding registry  |
| `/var/lib/face-ai/attendance.csv`  | Attendance records       |
| `/var/lib/face-ai/performance.csv` | Performance records      |
| `/var/backups/face-ai`             | Default backup root      |
| `/run/pir-monitor/events.sock`     | PIR event socket         |

## 9. Test Results

The following results were reported during internal project testing.

| Test                              | Reported Result                  |
|-----------------------------------|----------------------------------|
| Average boot-to-service-ready time| 30 seconds over 50 trials        |
| Average recognition processing time | 4.5 seconds over 100 trials    |
| Continuous operation              | 3 days                           |
| Reboot tests                      | 50 cycles                        |
| Power interruption tests          | 20 interruptions while running   |
| Wi-Fi disconnect/recovery tests   | 50 tests                         |
| Reported face recognition rate    | 96%                              |





## 10. Current Limitations and Future Work

The current implementation provides the embedded attendance components and a read-only Web dashboard.

The following Web management features remain future work unless implemented in a later revision:

- Employee creation, editing, and deletion
- Web-based face enrollment
- Camera configuration management through the Web interface
- Web-triggered backup and restore
- User account management
- Role-based access control and permission enforcement


## 12. License and Third-Party Components

Some recipes in this layer declare `LICENSE = "CLOSED"`. This recipe metadata does not, by itself, establish the license under which the project can be published.


---

