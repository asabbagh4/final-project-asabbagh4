generate_ssh_host_keys() {
    if [ ! -f ${IMAGE_ROOTFS}/etc/ssh/ssh_host_rsa_key ]; then
        /usr/bin/ssh-keygen -t rsa -f ${IMAGE_ROOTFS}/etc/ssh/ssh_host_rsa_key -N "" -q
    fi
    if [ ! -f ${IMAGE_ROOTFS}/etc/ssh/ssh_host_ecdsa_key ]; then
        /usr/bin/ssh-keygen -t ecdsa -f ${IMAGE_ROOTFS}/etc/ssh/ssh_host_ecdsa_key -N "" -q
    fi
    if [ ! -f ${IMAGE_ROOTFS}/etc/ssh/ssh_host_ed25519_key ]; then
        /usr/bin/ssh-keygen -t ed25519 -f ${IMAGE_ROOTFS}/etc/ssh/ssh_host_ed25519_key -N "" -q
    fi
}

# Enable systemd-networkd with DHCP on all wired interfaces
configure_network() {
    install -d ${IMAGE_ROOTFS}/etc/systemd/network/
    printf '[Match]\nType=ether\n\n[Network]\nDHCP=yes\n' \
        > ${IMAGE_ROOTFS}/etc/systemd/network/10-wired.network

    install -d ${IMAGE_ROOTFS}/etc/systemd/system/multi-user.target.wants/
    ln -sf /lib/systemd/system/systemd-networkd.service \
        ${IMAGE_ROOTFS}/etc/systemd/system/multi-user.target.wants/systemd-networkd.service

    install -d ${IMAGE_ROOTFS}/etc/systemd/system/network-online.target.wants/
    ln -sf /lib/systemd/system/systemd-networkd-wait-online.service \
        ${IMAGE_ROOTFS}/etc/systemd/system/network-online.target.wants/systemd-networkd-wait-online.service
}

# Allow root login via SSH with password
configure_sshd() {
    local conf="${IMAGE_ROOTFS}/etc/ssh/sshd_config"
    if [ -f "$conf" ]; then
        sed -i '/^#*PermitRootLogin/d' "$conf"
        sed -i '/^#*PasswordAuthentication/d' "$conf"
        echo 'PermitRootLogin yes' >> "$conf"
        echo 'PasswordAuthentication yes' >> "$conf"
        echo 'PermitEmptyPasswords yes' >> "$conf"
    fi
}

clear_root_password() {
    sed -i 's/^root:[^:]*:/root::/' ${IMAGE_ROOTFS}/etc/shadow
}

ROOTFS_POSTPROCESS_COMMAND:append = " generate_ssh_host_keys; configure_network; configure_sshd; clear_root_password;"

IMAGE_INSTALL:append = " synchronome"