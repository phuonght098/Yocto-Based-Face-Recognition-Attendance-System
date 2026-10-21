SUMMARY = "Wi-Fi configuration "
DESCRIPTION = "Install NetworkManager Wi-Fi connection profiles"
LICENSE = "CLOSED"

SRC_URI = " \
    file://phuong-wifi.nmconnection \
    file://phuong-wifi-2.nmconnection \
"

S = "${WORKDIR}"

inherit allarch

do_install() {
    install -d ${D}${sysconfdir}/NetworkManager/system-connections

    install -m 0600 \
        ${WORKDIR}/phuong-wifi.nmconnection \
        ${D}${sysconfdir}/NetworkManager/system-connections/phuong-wifi.nmconnection

    install -m 0600 \
        ${WORKDIR}/phuong-wifi-2.nmconnection \
        ${D}${sysconfdir}/NetworkManager/system-connections/phuong-wifi-2.nmconnection
}

FILES:${PN} += " \
    ${sysconfdir}/NetworkManager/system-connections/phuong-wifi.nmconnection \
    ${sysconfdir}/NetworkManager/system-connections/phuong-wifi-2.nmconnection \
"
