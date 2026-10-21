SUMMARY = "Read-only web administration UI for face attendance"
DESCRIPTION = "Lightweight Python standard-library web UI for attendance status and CSV records"
LICENSE = "CLOSED"

SRC_URI = " \
    file://app.py \
    file://attendance-web.init \
    file://attendance-web.conf \
"

S = "${WORKDIR}"

inherit update-rc.d

INITSCRIPT_NAME = "attendance-web"
INITSCRIPT_PARAMS = "defaults 90"

RDEPENDS:${PN} += "python3-core python3-netserver"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/app.py ${D}${bindir}/attendance-web

    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/attendance-web.init ${D}${sysconfdir}/init.d/attendance-web

    install -d ${D}${sysconfdir}
    install -m 0600 ${WORKDIR}/attendance-web.conf ${D}${sysconfdir}/attendance-web.conf

    install -d ${D}${localstatedir}/lib/attendance-web
}

FILES:${PN} += " \
    ${sysconfdir}/init.d/attendance-web \
    ${sysconfdir}/attendance-web.conf \
    ${localstatedir}/lib/attendance-web \
"
