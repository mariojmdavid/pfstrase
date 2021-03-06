#!/bin/bash

## Start with CentOS 7.5 VM
## MDS, OSS, and OSC kernel install

yum -y install firewalld
systemctl start firewalld
firewall-cmd --zone=public --add-port=988/tcp --permanent
firewall-cmd --reload

cat >> /etc/yum.repos.d/lustre.repo <<EOF
[lustre-server]
name=CentOS-$releasever - Lustre
baseurl=https://downloads.hpdd.intel.com/public/lustre/latest-feature-release/el7/server/
gpgcheck=0

[e2fsprogs]
name=CentOS-$releasever - Ldiskfs
baseurl=https://downloads.hpdd.intel.com/public/e2fsprogs/latest/el7/
gpgcheck=0

[lustre-client]
name=CentOS-$releasever - Lustre
baseurl=https://downloads.hpdd.intel.com/public/lustre/latest-feature-release/el7/client/
gpgcheck=0
EOF

yum -y upgrade e2fsprogs
yum -y install lustre-tests

echo "options lnet networks=tcp0(eth0)" >> /etc/modprobe.d/lnet.conf
cat >> /etc/sysconfig/modules/lnet.modules <<EOF
#!/bin/sh
if [ ! -c /dev/lnet ] ; then
    exec /sbin/modprobe lnet >/dev/null 2>&1
fi
EOF
fi

## Reboot at this point
# After reboot mount new disk device
# Do not reboot after device is mounted on Jetstream

## MDS/MGC and OSS filesystem format and 
# kernel modules differ.  The rest of this
# script might make sense to separate into
# independent script. 

## Set device to mount to correct value
# Set mgsnode arg to MGS ethernet ip (this differs on jetstream
# from public ip)

if [ "$1" == "mds" ]; then
mkfs.lustre --fsname=work --mgs --mdt --index=0 /dev/sdb1
mkdir /mnt/mdt && mount -t lustre /dev/sdb1 /mnt/mdt
fi

if [ "$1" == "oss" ]; then
mkfs.lustre --ost --fsname=work --mgsnode=172.29.12.5@tcp0 --index=0 /dev/sdb1
mkdir /ostoss_mount && mount -t lustre /dev/sdb1 /ostoss_mount
fi

if [ "$1" == "osc" ]; then
cat >> /etc/sysconfig/modules/lustre.modules <<EOF
#!/bin/sh
/sbin/lsmod | /bin/grep lustre 1>/dev/null 2>&1
if [ ! $? ] ; then
   /sbin/modprobe lustre >/dev/null 2>&1
fi
EOF
mkdir /mnt/lustre
mount -t lustre 172.29.12.5@tcp0:/work /mnt/lustre
fi


