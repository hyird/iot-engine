include $(TOPDIR)/rules.mk

PKG_NAME:=iot-edge
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

NANOPB_VERSION:=0.4.9.1
PKG_SOURCE:=nanopb-$(NANOPB_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/nanopb/nanopb/tar.gz/refs/tags/$(NANOPB_VERSION)?
PKG_HASH:=4575944a468718ef25f05eb01d994364650b581563089a9841986bb1e460eac3
PKG_BUILD_DIR:=$(BUILD_DIR)/nanopb-$(NANOPB_VERSION)

PKG_LICENSE:=Zlib MIT
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_FLAGS:=lto

include $(INCLUDE_DIR)/package.mk

define Package/iot-edge
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Small multi-platform IoT edge node
  URL:=https://github.com/hyird/iot-engine
  DEPENDS:=+libev +libuci +libuwsc-mbedtls +libmbedtls
endef

define Package/iot-edge/description
 A small OpenWrt edge daemon using WSS binary frames and nanopb. The node is
 identified by IMEI and writes raw nanopb config/outbox messages to tmpfs without a database.
endef

define Package/iot-edge/conffiles
/etc/config/iot-edge
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/edge/src $(PKG_BUILD_DIR)/edge/generated
	$(CP) $(CURDIR)/src/* $(PKG_BUILD_DIR)/edge/src/
	$(CP) $(CURDIR)/generated/* $(PKG_BUILD_DIR)/edge/generated/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Os -flto \
		-ffunction-sections -fdata-sections -fno-unwind-tables \
		-I$(PKG_BUILD_DIR) -I$(PKG_BUILD_DIR)/edge/src \
		-I$(PKG_BUILD_DIR)/edge/generated \
		$(PKG_BUILD_DIR)/pb_common.c \
		$(PKG_BUILD_DIR)/pb_encode.c \
		$(PKG_BUILD_DIR)/pb_decode.c \
		$(PKG_BUILD_DIR)/edge/generated/edge.pb.c \
		$(PKG_BUILD_DIR)/edge/src/edge_protocol.c \
		$(PKG_BUILD_DIR)/edge/src/edge_config.c \
		$(PKG_BUILD_DIR)/edge/src/edge_memory.c \
		$(PKG_BUILD_DIR)/edge/src/edge_spool.c \
		$(PKG_BUILD_DIR)/edge/src/edge_ws.c \
		$(PKG_BUILD_DIR)/edge/src/edge_device_runtime.c \
		$(PKG_BUILD_DIR)/edge/src/edge_modbus.c \
		$(PKG_BUILD_DIR)/edge/src/edge_s7.c \
		$(PKG_BUILD_DIR)/edge/src/main.c \
		$(TARGET_LDFLAGS) -Wl,--gc-sections -flto \
		-lev -luwsc -luci -lmbedcrypto \
		-o $(PKG_BUILD_DIR)/iot-edge
endef

define Package/iot-edge/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/iot-edge $(1)/usr/sbin/iot-edge
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(CURDIR)/files/iot-edge.init $(1)/etc/init.d/iot-edge
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(CURDIR)/files/iot-edge.config $(1)/etc/config/iot-edge
	$(INSTALL_DIR) $(1)/etc/iot-edge/credentials
	$(INSTALL_DIR) $(1)/usr/share/iot-edge
	$(INSTALL_DATA) $(CURDIR)/files/NOTICE $(1)/usr/share/iot-edge/NOTICE
endef

$(eval $(call BuildPackage,iot-edge))
