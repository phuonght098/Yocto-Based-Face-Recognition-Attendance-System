SUMMARY = "YuNet and SFace ONNX models"
LICENSE = "CLOSED"

FILESEXTRAPATHS:prepend := "${THISDIR}/face-detection:${THISDIR}/face-recognition:"

SRC_URI = " \
    file://face_detection_yunet_2023mar.onnx \
    file://face_recognition_sface_2021dec.onnx \
"

S = "${WORKDIR}"

do_install() {
    install -d ${D}${datadir}/face-recognition/models

    install -m 0644 \
        ${WORKDIR}/face_detection_yunet_2023mar.onnx \
        ${D}${datadir}/face-recognition/models/face_detection_yunet_2023mar.onnx

    install -m 0644 \
        ${WORKDIR}/face_recognition_sface_2021dec.onnx \
        ${D}${datadir}/face-recognition/models/face_recognition_sface_2021dec.onnx
}

FILES:${PN} = "${datadir}/face-recognition/models/*"
