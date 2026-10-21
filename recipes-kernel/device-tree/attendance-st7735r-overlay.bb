SUMMARY = "Device Tree Overlay for ST7735R TFT display"
LICENSE = "CLOSED"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI = "file://attendance-st7735r-overlay.dts"

DEPENDS = "dtc-native"

inherit deploy

S = "${WORKDIR}"

do_compile() {
    install -d ${B}

    ${STAGING_BINDIR_NATIVE}/dtc \
        -@ \
        -I dts \
        -O dtb \
        -o ${B}/attendance-st7735r.dtbo \
        ${WORKDIR}/attendance-st7735r-overlay.dts
}

do_deploy() {
    install -d ${DEPLOYDIR}

    install -m 0644 \
        ${B}/attendance-st7735r.dtbo \
        ${DEPLOYDIR}/attendance-st7735r.dtbo
}

addtask deploy after do_compile before do_build
