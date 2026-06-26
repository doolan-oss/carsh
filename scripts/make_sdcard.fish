#!/usr/bin/env fish
#
# make_sdcard.fish — assemble a bootable carsh SD card for the Raspberry Pi 4.
#
# The Pi 4 EEPROM bootloader can only read a FAT16/FAT32 boot partition, so this
# (re)creates an MBR + single FAT32 partition on the target device and copies the
# Circle firmware, the carsh kernel image, and a Pi 4 config.txt onto it.
#
#   Usage: scripts/make_sdcard.fish /dev/sdX [--force]
#
# DESTROYS ALL DATA on the target device. Requires root for format/mount, so it
# re-invokes the destructive steps via sudo.
#
# --force  allow a non-removable target (otherwise refused as a safety guard).

function die
    echo "make_sdcard: $argv" >&2
    exit 1
end

# --- args ---------------------------------------------------------------------
set -l dev ""
set -l force 0
for a in $argv
    switch $a
        case --force
            set force 1
        case '-*'
            die "unknown option: $a"
        case '*'
            test -z "$dev"; or die "extra argument: $a"
            set dev $a
    end
end
test -n "$dev"; or die "usage: make_sdcard.fish /dev/sdX [--force]"
test -b "$dev"; or die "$dev is not a block device"

# --- locate sources -----------------------------------------------------------
set -l repo (realpath (status dirname)/..)
set -l boot $repo/third_party/circle-stdlib/libs/circle/boot
set -l kernel $repo/device/kernel8-rpi4.img

set -l srcs \
    $kernel \
    $boot/start4.elf \
    $boot/fixup4.dat \
    $boot/bcm2711-rpi-4-b.dtb \
    $boot/armstub8-rpi4.bin
for f in $srcs
    test -f $f; or die "missing source file: $f (build the kernel / fetch firmware first)"
end

# --- safety guards ------------------------------------------------------------
# Never touch a disk that backs a live system mount.
for mp in / /home /boot /boot/efi /var
    set -l src (findmnt -no SOURCE $mp 2>/dev/null)
    if string match -q "$dev*" -- "$src"
        die "refusing: $dev backs $mp"
    end
end

set -l base (basename $dev)
set -l removable (cat /sys/block/$base/removable 2>/dev/null)
if test "$removable" != 1; and test $force -eq 0
    die "$dev is not removable — pass --force if you are certain"
end

# Partition node: mmcblk0 -> mmcblk0p1, sda -> sda1.
set -l part {$dev}1
if string match -qr '[0-9]$' -- $dev
    set part {$dev}p1
end

# --- confirm ------------------------------------------------------------------
set -l model (lsblk -dno MODEL $dev 2>/dev/null | string trim)
set -l size (lsblk -dno SIZE $dev 2>/dev/null | string trim)
echo "About to ERASE and reformat:"
echo "  device : $dev  ($size  $model)"
echo "  as     : MBR + FAT32 'CARSHBOOT', then copy the carsh boot files"
read -l -P "Type ERASE to continue: " reply
test "$reply" = ERASE; or die "aborted"

# --- do it --------------------------------------------------------------------
echo ">> unmounting any existing partitions on $dev"
for p in (lsblk -lno NAME $dev | tail -n +2)
    sudo umount /dev/$p 2>/dev/null
end

echo ">> wiping signatures + writing MBR/FAT32 partition"
sudo wipefs -a $dev; or die "wipefs failed"
sudo parted -s $dev mklabel msdos; or die "mklabel failed"
sudo parted -s $dev mkpart primary fat32 1MiB 100%; or die "mkpart failed"
sudo parted -s $dev set 1 lba on; or die "set lba failed"
sudo partprobe $dev; sleep 1

echo ">> formatting $part as FAT32"
sudo mkfs.vfat -F 32 -n CARSHBOOT $part; or die "mkfs.vfat failed"

# Let udev re-probe the new FAT superblock; the auto-mounter may also grab it,
# so settle then ensure it is free before we mount it ourselves.
sudo udevadm settle 2>/dev/null
sudo umount $part 2>/dev/null

set -l mnt (mktemp -d)
echo ">> mounting $part at $mnt"
sudo mount -t vfat $part $mnt; or die "mount failed"

echo ">> copying boot files"
for f in $srcs
    sudo cp -v $f $mnt/
end

echo ">> writing config.txt"
printf '%s\n' \
    '# carsh boot config — Raspberry Pi 4, AArch64 (Circle bare metal).' \
    'arm_64bit=1' \
    'kernel_address=0x80000' \
    'initial_turbo=0' \
    '' \
    '[pi4]' \
    'armstub=armstub8-rpi4.bin' \
    'kernel=kernel8-rpi4.img' \
    'max_framebuffers=2' \
    'enable_uart=1          # boot log on PL011 GPIO14/15 @ 115200' \
    'display_auto_detect=1' \
    | sudo tee $mnt/config.txt >/dev/null; or die "config.txt write failed"

echo ">> syncing"
sudo sync
sudo umount $mnt; and rmdir $mnt

echo ""
echo "Done. Contents of the boot partition:"
sudo mount -t vfat $part $mnt 2>/dev/null; and ls -la $mnt; and sudo umount $mnt; and rmdir $mnt
echo ""
echo "Eject, insert into the Pi 4, and watch the green ACT LED + HDMI."
echo "Serial: 3.3V USB-TTL on pins 8/10/6 -> picocom -b 115200 /dev/ttyUSB0"
