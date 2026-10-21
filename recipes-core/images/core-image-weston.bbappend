IMAGE_FEATURES:append = " ssh-server-dropbear"

ENABLE_SPI_BUS = "1"

ENABLE_UART = "1"

IMAGE_INSTALL:append = " libgpiod libgpiod-tools pir-monitor"

RPI_EXTRA_CONFIG:append = "\ndtoverlay=attendance-st7735r"

IMAGE_INSTALL:append = " gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad wifi-camera face-models face-app onnxruntime attendance-app chrony attendance-web networkmanager wpa-supplicant phuong-wifi"

IMAGE_BOOT_FILES:append = " attendance-st7735r.dtbo;overlays/attendance-st7735r.dtbo"

PACKAGECONFIG:append:pn-opencv = " dnn"
