SUMMARY = "Reconnect-capable RTSP Wi-Fi camera client"
DESCRIPTION = "Receives and decodes H.264 RTSP video using GStreamer appsink"
LICENSE = "CLOSED"

SRC_URI = "file://wifi_camera.c \
           file://CMakeLists.txt \
           file://wifi-camera.conf"

S = "${WORKDIR}"

DEPENDS = "gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad"

inherit cmake pkgconfig

do_install:append() {
    install -d ${D}${sysconfdir}
    install -m 0600 ${WORKDIR}/wifi-camera.conf ${D}${sysconfdir}/wifi-camera.conf
}

FILES:${PN} += "${sysconfdir}/wifi-camera.conf"

RDEPENDS:${PN} += " \
    gstreamer1.0 \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-base-videoconvert \
    gstreamer1.0-plugins-good-rtsp \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-bad-videoparsersbad \
"
