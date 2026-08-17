#

comment "Android-style boot image depends on linux kernel"
	depends on !RK_KERNEL

menu "Boot (Android-style boot image)"
	depends on RK_KERNEL

config RK_BOOT_IMG
	string
	default "zboot.img" if RK_BOOT_COMPRESSED
	default "boot.img"

config RK_BOOT_COMPRESSED
	bool "compressed boot image (zboot)"
	default y if RK_CHIP_FAMILY = "rv1126_rv1109" || \
		RK_CHIP_FAMILY = "px30" || RK_CHIP_FAMILY = "px3se" || \
		RK_CHIP_FAMILY = "rk3036" || RK_CHIP_FAMILY = "rk3128h" || \
		RK_CHIP_FAMILY = "rk312x" || RK_CHIP_FAMILY = "rk3229" || \
		RK_CHIP_FAMILY = "rk3288" || RK_CHIP_FAMILY = "rk3308" || \
		RK_CHIP_FAMILY = "rk3326" || RK_CHIP_FAMILY = "rk3358"

if RK_USE_FIT_IMG

config RK_BOOT_FIT_ITS_NAME
	string "its script for FIT boot image"
	depends on RK_USE_FIT_IMG
	default "boot.its" if RK_CHIP_FAMILY = "rv1126_rv1109"
	default "zboot.its" if RK_BOOT_COMPRESSED
	default "boot.its"

config RK_BOOT_FIT_ITS
	string
	default "$RK_CHIP_DIR/$RK_BOOT_FIT_ITS_NAME"

config RK_BOOT_FIT_RECOVERY_DTS_NAME
	string "recovery device-tree name for a multi-configuration FIT"
	help
	  Optional recovery DTB built together with the normal kernel DTB.

config RK_BOOT_FIT_PACK_SCRIPT
	string "custom FIT pack script"
	help
	  Optional SDK-relative script used instead of mk-fitimage.sh. The output
	  path is passed as "--output PATH".

config RK_BOOT_FIT_MAX_SIZE
	hex "maximum FIT image size"
	default 0x0
	help
	  Maximum allowed size of the packed FIT image in bytes. A value of zero
	  disables the size check. Custom FIT pack scripts may use this to enforce
	  the limit imposed by the selected partition layout.

endif # FIT image

endmenu # Boot
