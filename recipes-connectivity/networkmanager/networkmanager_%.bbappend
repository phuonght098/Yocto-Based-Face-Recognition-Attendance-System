FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://10-ifupdown-managed.conf"

do_install:append() {
    install -d ${D}${sysconfdir}/NetworkManager/conf.d

    install -m 0644 \
        ${WORKDIR}/10-ifupdown-managed.conf \
        ${D}${sysconfdir}/NetworkManager/conf.d/10-ifupdown-managed.conf
}
