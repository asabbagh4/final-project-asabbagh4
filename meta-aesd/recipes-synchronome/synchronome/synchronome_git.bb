DESCRIPTION = "Machine Vision Synchronome real-time V4L2 camera capture pipeline"
HOMEPAGE = "https://github.com/lt47/machine-vision-synchronome"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "git://github.com/lt47/machine-vision-synchronome;protocol=https;branch=main \
           file://mnt-ramdisk.mount \
           file://synchronome-tmpfiles.conf"
SRCREV = "${AUTOREV}"

PV = "1.0+git${SRCPV}"
S = "${WORKDIR}/git"

inherit systemd

SYSTEMD_SERVICE:${PN} = "mnt-ramdisk.mount"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_compile() {
    oe_runmake CC="${CC}" LD="${CC}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/sequencer ${D}${bindir}/sequencer

    install -d ${D}${systemd_unitdir}/system/
    install -m 0644 ${WORKDIR}/mnt-ramdisk.mount ${D}${systemd_unitdir}/system/

    install -d ${D}${sysconfdir}/tmpfiles.d/
    install -m 0644 ${WORKDIR}/synchronome-tmpfiles.conf \
        ${D}${sysconfdir}/tmpfiles.d/synchronome.conf

    install -d ${D}/mnt/ramdisk
}

FILES:${PN} += "${bindir}/sequencer \
                ${systemd_unitdir}/system/mnt-ramdisk.mount \
                ${sysconfdir}/tmpfiles.d/synchronome.conf \
                /mnt/ramdisk"
