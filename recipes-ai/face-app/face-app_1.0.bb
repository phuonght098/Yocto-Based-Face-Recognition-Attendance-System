SUMMARY = "GStreamer and ONNX Runtime face detection and recognition"
DESCRIPTION = "Uses YuNet for face detection and SFace for face embeddings from an RTSP camera"
LICENSE = "CLOSED"

SRC_URI = " \
    file://face_ai.cpp \
    file://CMakeLists.txt \
"

S = "${WORKDIR}"

DEPENDS = " \
    onnxruntime \
    face-models \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
"

inherit cmake pkgconfig

FILES:${PN} += "${bindir}/face-ai"

RDEPENDS:${PN} += " \
    onnxruntime \
    face-models \
    gstreamer1.0 \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-base-videoconvert \
    gstreamer1.0-plugins-good-rtsp \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-bad-videoparsersbad \
"
