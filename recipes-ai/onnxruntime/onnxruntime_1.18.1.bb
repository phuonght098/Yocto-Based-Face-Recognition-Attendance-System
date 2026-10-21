SUMMARY = "ONNX Runtime for Raspberry Pi 4 ARM64"
HOMEPAGE = "https://onnxruntime.ai/"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "https://github.com/microsoft/onnxruntime/releases/download/v${PV}/onnxruntime-linux-aarch64-${PV}.tgz"
SRC_URI[sha256sum] = "c1dcd8ab29e8d227d886b6ee415c08aea893956acf98f0758a42a84f27c02851"

S = "${WORKDIR}/onnxruntime-linux-aarch64-${PV}"

COMPATIBLE_HOST = "aarch64.*-linux"

inherit bin_package

do_install() {
    install -d ${D}${libdir}
    install -d ${D}${includedir}/onnxruntime

    cp -a ${S}/lib/. ${D}${libdir}/
    cp -a ${S}/include/. ${D}${includedir}/onnxruntime/

    chown -R root:root ${D}
}

FILES:${PN} += "${libdir}/libonnxruntime.so*"
FILES:${PN}-dev += "${includedir}/onnxruntime"
INSANE_SKIP:${PN} += "already-stripped"
