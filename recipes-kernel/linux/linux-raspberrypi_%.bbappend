FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://st7735r.cfg"

KERNEL_CONFIG_FRAGMENTS += "${WORKDIR}/st7735r.cfg"
