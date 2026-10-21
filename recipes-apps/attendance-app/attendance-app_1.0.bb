SUMMARY = "PIR-triggered face attendance application"
DESCRIPTION = "Connects PIR events to face-ai and records attendance on Raspberry Pi TFT"
LICENSE = "CLOSED"

SRC_URI = " \
    file://attendance_app.cpp \
    file://attendance-app.init \
"

S = "${WORKDIR}"

inherit update-rc.d

INITSCRIPT_NAME = "attendance-app"
INITSCRIPT_PARAMS = "defaults 95 5"

do_compile() {
    ${CXX} ${CPPFLAGS} ${CXXFLAGS} \
        -std=c++17 \
        ${S}/attendance_app.cpp \
        -o attendance-app \
        ${LDFLAGS}
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/attendance-app \
        ${D}${bindir}/attendance-app

    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/attendance-app.init \
        ${D}${sysconfdir}/init.d/attendance-app
}

FILES:${PN} += " \
    ${sysconfdir}/init.d/attendance-app \
"

RDEPENDS:${PN} += " \
    face-app \
    pir-monitor \
"
