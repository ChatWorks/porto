debootstrap --foreign --variant=minbase --exclude systemd-sysv --include sysvinit-core --arch amd64 jessie . http://mirror.yandex.ru/debian/

# Do not create devices
tar cz -T /dev/null > debootstrap/devices.tar.gz

# Do not mount/umount anything
tee -a debootstrap/functions <<EOF
mount () { warning "" "skip mount \$*"; }
umount () { warning "" "skip umount \$*"; }
EOF
