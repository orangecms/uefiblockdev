#!/bin/sh

mount -t udf /dev/uefi1 /boot
chainload -v /boot/efi/boot/bootx64.efi
