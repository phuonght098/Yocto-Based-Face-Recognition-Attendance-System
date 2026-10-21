SUMMARY = "Reusable PIR motion event service"
DESCRIPTION = "Reads HC-SR501 on GPIO17 and publishes JSON events"
LICENSE = "CLOSED"

DEPENDS = "libgpiod"

SRC_URI = " \
    file://pir-monitor.c \
    file://pir-monitor.service \
"

S = "${WORKDIR}"

inherit systemd pkgconfig

SYSTEMD_SERVICE:${PN} = "pir-monitor.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_compile() {
    ${CC} ${CPPFLAGS} ${CFLAGS} \
        $(pkg-config --cflags libgpiod) \
        ${S}/pir-monitor.c -o pir-monitor \
        ${LDFLAGS} \
        $(pkg-config --libs libgpiod)
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/pir-monitor ${D}${bindir}/pir-monitor

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/pir-monitor.service \
        ${D}${systemd_system_unitdir}/pir-monitor.service
}

FILES:${PN} += " \
    ${systemd_system_unitdir}/pir-monitor.service \
"
