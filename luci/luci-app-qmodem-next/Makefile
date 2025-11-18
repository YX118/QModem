# Copyright (C) 2024 Tom <fjrcn@outlook.com>
# This is free software, licensed under the GNU General Public License v3.

include $(TOPDIR)/rules.mk

# Include unified version
include ../../version.mk

LUCI_TITLE:=LuCI Modern UI for QWRT Modem
LUCI_DEPENDS:=+qmodem +luci-base
LUCI_PKGARCH:=all
PKG_VERSION:=$(QMODEM_VERSION)
PKG_RELEASE:=1
PKG_LICENSE:=GPLv3
PKG_MAINTAINER:=Tom <fjrcn@outlook.com>

include ../../luci.mk

# call BuildPackage - OpenWrt buildroot signature
