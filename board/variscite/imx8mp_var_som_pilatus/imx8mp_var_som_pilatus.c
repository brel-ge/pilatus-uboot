// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2019 NXP
 * Copyright 2020-2024 Variscite Ltd.
 */

#include <efi_loader.h>
#include <errno.h>
#include <fdtdec.h>
#include <miiphy.h>
#include <netdev.h>
#include <asm/io.h>
#include <asm/mach-imx/iomux-v3.h>
#include <asm-generic/gpio.h>
#include <asm/arch/imx8mp_pins.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/gpio.h>
#include <asm/mach-imx/mxc_i2c.h>
#include <asm/arch/clock.h>
#include <spl.h>
#include <asm/mach-imx/dma.h>
#include <power/pmic.h>
#include <usb.h>
#include <dwc3-uboot.h>
#include <power/regulator.h>
#include <linux/delay.h>
#include <imx_sip.h>
#include <linux/arm-smccc.h>
#include <mmc.h>
#include <fs.h>
#include <video.h>

#include "../common/extcon-ptn5150.h"
#include "../common/imx8_eeprom.h"
#include "imx8mp_var_som_pilatus.h"

int var_setup_mac(struct var_eeprom *eeprom);

#ifdef CONFIG_VIDEO
#ifdef CONFIG_VIDEO_LOGO
#include <bmp_logo.h>
#endif
#include <splash.h>
#include <backlight.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

#define WDOG_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_ODE | PAD_CTL_PUE | PAD_CTL_PE)
#define GPIO_PAD_CTRL	(PAD_CTL_DSE1 | PAD_CTL_PUE | PAD_CTL_PE  | PAD_CTL_HYS)

#define DISPMIX				13
#define MIPI				15

#define LOADADDR_BMP			0x4F600000

static iomux_v3_cfg_t const wdog_pads[] = {
	MX8MP_PAD_GPIO1_IO02__WDOG1_WDOG_B  | MUX_PAD_CTRL(WDOG_PAD_CTRL),
};

#ifdef CONFIG_SPL_BUILD

#define BOARD_DETECT_GPIO IMX_GPIO_NR(2, 11)
#define SOM_WIFI_EN_GPIO IMX_GPIO_NR(2, 19)

static iomux_v3_cfg_t const board_detect_pads[] = {
	MX8MP_PAD_SD1_STROBE__GPIO2_IO11 | MUX_PAD_CTRL(GPIO_PAD_CTRL),
	MX8MP_PAD_SD2_RESET_B__GPIO2_IO19 | MUX_PAD_CTRL(GPIO_PAD_CTRL),
};
#endif

int var_detect_board_id(void)
{
	static int board_id = BOARD_ID_UNDEF;

	if (board_id != BOARD_ID_UNDEF)
		return board_id;

#ifdef CONFIG_SPL_BUILD
	imx_iomux_v3_setup_multiple_pads(board_detect_pads,
				ARRAY_SIZE(board_detect_pads));
	/*
	 * For VAR-SOM-MX8M-PLUS 2.x, the IW612 will pull BOARD_DETECT_GPIO
	 * low when the module is powered down (PDn is asserted low).
	 * To avoid this, assert PDn high so BOARD_DETECT_GPIO can be read.
	 */
	gpio_request(SOM_WIFI_EN_GPIO, "wifi_en");
	gpio_direction_output(SOM_WIFI_EN_GPIO, 1);
	udelay(10);

	gpio_request(BOARD_DETECT_GPIO, "board_detect");
	gpio_direction_input(BOARD_DETECT_GPIO);
	board_id = gpio_get_value(BOARD_DETECT_GPIO) ? BOARD_ID_SOM : BOARD_ID_DART;

	if (board_id == BOARD_ID_SOM)
		gpio_set_value(SOM_WIFI_EN_GPIO, 0);

	gpio_free(BOARD_DETECT_GPIO);
	gpio_free(SOM_WIFI_EN_GPIO);
#else
	if (of_machine_is_compatible("variscite,imx8mp-var-som"))
		board_id = BOARD_ID_SOM;
	else if (of_machine_is_compatible("variscite,imx8mp-var-dart"))
		board_id = BOARD_ID_DART;
#endif

	return board_id;
}

#ifdef CONFIG_POWER_PCA9450
#define PCA9450_I2C_BUS	0
#define PCA9450_I2C_ADDR	0x25
#define PCA9450_LSW_CTRL_ADDR	0x2A
#define REG_DATA_SIZE		1

int setup_sw_en_pmic(void)
{
	struct udevice *bus;
	struct udevice *dev;
	u8 reg_data[REG_DATA_SIZE];
	int ret;

	struct var_eeprom *ep = VAR_EEPROM_DATA;
	int som_rev = SOMREV_MAJOR(ep->somrev);

	/* On DART-MX8M-PLUS v2.0, SW_EN must be set by software to ensure
	 * that the ethernet PHY is powered up. Set bit 0 of LOADSW_CTRL.
	 */
	if (som_rev >= 2) {
		ret = uclass_get_device_by_seq(UCLASS_I2C, PCA9450_I2C_BUS, &bus);
		if (ret) {
			printf("Can't find I2C bus %d\n", PCA9450_I2C_BUS);
			return ret;
		}

		ret = dm_i2c_probe(bus, PCA9450_I2C_ADDR, 0, &dev);
		if (ret) {
			printf("Can't find device at address 0x%x\n", PCA9450_I2C_ADDR);
			return ret;
		}

		ret = dm_i2c_read(dev, PCA9450_LSW_CTRL_ADDR, reg_data, REG_DATA_SIZE);
		if (ret) {
			printf("Failed to read address 0x%x\n", PCA9450_I2C_ADDR);
			return ret;
		}

		/* Enable SW_EN by setting LOADSW_CTRL bit 0 */
		reg_data[0] |= 0x01;

		ret = dm_i2c_write(dev, PCA9450_LSW_CTRL_ADDR, reg_data, REG_DATA_SIZE);
		if (ret) {
			printf("Failed to write at address 0x%x\n", PCA9450_I2C_ADDR);
			return ret;
		}
	}

	return 0;
}
#endif

#if CONFIG_IS_ENABLED(EFI_HAVE_CAPSULE_SUPPORT)
struct efi_fw_image fw_images[] = {
	{
		.image_type_id = IMX_BOOT_IMAGE_GUID,
		.fw_name = u"IMX8MP-VAR-DART-RAW",
		.image_index = 1,
	},
};

struct efi_capsule_update_info update_info = {
	.dfu_string = "mmc 2=1 raw 0x40 0x1000",
	.num_images = ARRAY_SIZE(fw_images),
	.images = fw_images,
};

#endif /* EFI_HAVE_CAPSULE_SUPPORT */

int board_early_init_f(void)
{
	struct wdog_regs *wdog = (struct wdog_regs *)WDOG1_BASE_ADDR;

	imx_iomux_v3_setup_multiple_pads(wdog_pads, ARRAY_SIZE(wdog_pads));

	set_wdog_reset(wdog);

	return 0;
}

#ifdef CONFIG_OF_BOARD_SETUP
int ft_board_setup(void *blob, struct bd_info *bd)
{
#ifdef CONFIG_IMX8M_DRAM_INLINE_ECC
	int rc;
	phys_addr_t ecc0_start = 0xb0000000;
	phys_addr_t ecc1_start = 0x130000000;
	phys_addr_t ecc2_start = 0x1b0000000;
	size_t ecc_size = 0x10000000;

	rc = add_res_mem_dt_node(blob, "ecc", ecc0_start, ecc_size);
	if (rc < 0) {
		printf("Could not create ecc0 reserved-memory node.\n");
		return rc;
	}

	rc = add_res_mem_dt_node(blob, "ecc", ecc1_start, ecc_size);
	if (rc < 0) {
		printf("Could not create ecc1 reserved-memory node.\n");
		return rc;
	}

	rc = add_res_mem_dt_node(blob, "ecc", ecc2_start, ecc_size);
	if (rc < 0) {
		printf("Could not create ecc2 reserved-memory node.\n");
		return rc;
	}
#endif

	return 0;
}
#endif

#ifdef CONFIG_USB_DWC3

#define USB_PHY_CTRL0			0xF0040
#define USB_PHY_CTRL0_REF_SSP_EN	BIT(2)

#define USB_PHY_CTRL1			0xF0044
#define USB_PHY_CTRL1_RESET		BIT(0)
#define USB_PHY_CTRL1_COMMONONN		BIT(1)
#define USB_PHY_CTRL1_ATERESET		BIT(3)
#define USB_PHY_CTRL1_VDATSRCENB0	BIT(19)
#define USB_PHY_CTRL1_VDATDETENB0	BIT(20)

#define USB_PHY_CTRL2			0xF0048
#define USB_PHY_CTRL2_TXENABLEN0	BIT(8)

#define USB_PHY_CTRL6			0xF0058

#define HSIO_GPR_BASE					(0x32F10000U)
#define HSIO_GPR_REG_0					(HSIO_GPR_BASE)
#define HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN_SHIFT	(1)
#define HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN		(0x1U << HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN_SHIFT)

static struct dwc3_device dwc3_device_data = {
#ifdef CONFIG_SPL_BUILD
	.maximum_speed = USB_SPEED_HIGH,
#else
	.maximum_speed = USB_SPEED_SUPER,
#endif
	.base = USB1_BASE_ADDR,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.power_down_scale = 2,
};

int dm_usb_gadget_handle_interrupts(struct udevice *dev)
{
	dwc3_uboot_handle_interrupt(dev);
	return 0;
}

static void dwc3_nxp_usb_phy_init(struct dwc3_device *dwc3)
{
	u32 RegData;

	/* enable usb clock via hsio gpr */
	RegData = readl(HSIO_GPR_REG_0);
	RegData |= HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN;
	writel(RegData, HSIO_GPR_REG_0);

	/* USB3.0 PHY signal fsel for 100M ref */
	RegData = readl(dwc3->base + USB_PHY_CTRL0);
	RegData = (RegData & 0xfffff81f) | (0x2a << 5);
	writel(RegData, dwc3->base + USB_PHY_CTRL0);

	RegData = readl(dwc3->base + USB_PHY_CTRL6);
	RegData &= ~0x1;
	writel(RegData, dwc3->base + USB_PHY_CTRL6);

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_VDATSRCENB0 | USB_PHY_CTRL1_VDATDETENB0 |
			USB_PHY_CTRL1_COMMONONN);
	RegData |= USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET;
	writel(RegData, dwc3->base + USB_PHY_CTRL1);

	RegData = readl(dwc3->base + USB_PHY_CTRL0);
	RegData |= USB_PHY_CTRL0_REF_SSP_EN;
	writel(RegData, dwc3->base + USB_PHY_CTRL0);

	RegData = readl(dwc3->base + USB_PHY_CTRL2);
	RegData |= USB_PHY_CTRL2_TXENABLEN0;
	writel(RegData, dwc3->base + USB_PHY_CTRL2);

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET);
	writel(RegData, dwc3->base + USB_PHY_CTRL1);
}
#endif

#if defined(CONFIG_USB_DWC3) || defined(CONFIG_USB_XHCI_IMX8M)

#ifdef CONFIG_EXTCON_PTN5150
static struct extcon_ptn5150 usb_ptn5150;
#endif

int board_usb_init(int index, enum usb_init_type init)
{
	int ret = 0;

	imx8m_usb_power(index, true);

#if (!defined(CONFIG_SPL_BUILD) && defined(CONFIG_EXTCON_PTN5150))
	if (index == 0) {
		/* Verify port is in proper mode */
		int phy_mode = extcon_ptn5150_phy_mode(&usb_ptn5150);

		/* Only verify phy_mode if ptn5150 is initialized */
		if (phy_mode >= 0 && phy_mode != init)
			return -ENODEV;
	}
#endif

	if (index == 0 && init == USB_INIT_DEVICE) {
#ifdef CONFIG_USB_TCPC
		ret = tcpc_setup_ufp_mode(&port1);
		if (ret)
			return ret;
#endif
		dwc3_nxp_usb_phy_init(&dwc3_device_data);
		return dwc3_uboot_init(&dwc3_device_data);
	} else if (index == 0 && init == USB_INIT_HOST) {
#ifdef CONFIG_USB_TCPC
		ret = tcpc_setup_dfp_mode(&port1);
#endif
		return ret;
	}

	return 0;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	int ret = 0;

	if (index == 0 && init == USB_INIT_DEVICE) {
		dwc3_uboot_exit(index);
	} else if (index == 0 && init == USB_INIT_HOST) {
#ifdef CONFIG_USB_TCPC
		ret = tcpc_disable_src_vbus(&port1);
#endif
	}

	imx8m_usb_power(index, false);

	return ret;
}


#ifdef CONFIG_EXTCON_PTN5150
int board_ehci_usb_phy_mode(struct udevice *dev)
{
	int usb_phy_mode = extcon_ptn5150_phy_mode(&usb_ptn5150);

	/* Default to host mode if not connected */
	if (usb_phy_mode < 0)
		usb_phy_mode = USB_INIT_HOST;

	return usb_phy_mode;
}
#endif
#endif

#ifdef CONFIG_OF_BOARD_FIXUP
int vendor_board_fix_fdt(void *fdt_blob)
{
	struct var_eeprom *ep = VAR_EEPROM_DATA;
	int som_rev = SOMREV_MAJOR(ep->somrev);

	if (!fdt_blob) {
		printf("ERROR: Device tree blob not found.\n");
		return -EINVAL;
	}

	if ((var_detect_board_id() == BOARD_ID_DART) && (som_rev >= 2)) {
		int node_offset, subnode_offset, ret;
		const char *node_path = "/soc@0/bus@30000000/gpio@30210000";
		const char *node_name = "eth0_phy_pwr_hog";

		node_offset = fdt_path_offset(fdt_blob, node_path);
		if (node_offset < 0) {
			printf("WARNING: couldn't find %s: %s\n", node_path,
			       fdt_strerror(node_offset));
			return -ENOENT;
		}

		subnode_offset = fdt_subnode_offset(fdt_blob, node_offset, node_name);
		if (subnode_offset < 0) {
			printf("WARNING: couldn't find %s node: %s\n",
			       node_name, fdt_strerror(subnode_offset));
			return -ENOENT;
		}

		ret = fdt_del_node(fdt_blob, subnode_offset);
		if (ret < 0) {
			printf("WARNING: Couldn't delete subnode %s: %s\n",
			       node_name, fdt_strerror(ret));
			return ret;
		}
	}

	return 0;
}
#endif

int board_init(void)
{
	struct arm_smccc_res res;
#ifdef CONFIG_EXTCON_PTN5150
		extcon_ptn5150_setup(&usb_ptn5150);
#endif
#ifdef CONFIG_POWER_PCA9450
	int ret;

	ret = setup_sw_en_pmic();
	if (ret)
		return ret;
#endif

#if defined(CONFIG_USB_DWC3) || defined(CONFIG_USB_XHCI_IMX8M)
	init_usb_clk();
#endif

	/* enable the dispmix & mipi phy power domain */
	arm_smccc_smc(IMX_SIP_GPC, IMX_SIP_GPC_PM_DOMAIN,
		      DISPMIX, true, 0, 0, 0, 0, &res);
	arm_smccc_smc(IMX_SIP_GPC, IMX_SIP_GPC_PM_DOMAIN,
		      MIPI, true, 0, 0, 0, 0, &res);

	return 0;
}

#define SDRAM_SIZE_STR_LEN 5

int board_late_init(void)
{
	int board_id;
	char sdram_size_str[SDRAM_SIZE_STR_LEN];
	struct var_eeprom *ep = VAR_EEPROM_DATA;
	struct var_carrier_eeprom carrier_eeprom;
	char carrier_rev[CARRIER_REV_LEN] = {0};
	char som_rev[CARRIER_REV_LEN] = {0};

#ifdef CONFIG_ENV_IS_IN_MMC
	board_late_mmc_env_init();
#endif
#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
	env_set("board_name", "DART-MX8MP");
	env_set("board_rev", "iMX8MP");
#endif

	snprintf(sdram_size_str, SDRAM_SIZE_STR_LEN, "%d",
			(int)(gd->ram_size / 1024 / 1024));
	env_set("sdram_size", sdram_size_str);

	board_id = var_detect_board_id();
	if (board_id != BOARD_ID_UNDEF) {
		if (board_id == BOARD_ID_SOM) {
			env_set("board_name", "VAR-SOM-MX8M-PLUS");
			env_set("console", "ttymxc1,115200");

			var_carrier_eeprom_read(CARRIER_EEPROM_BUS_SOM, CARRIER_EEPROM_ADDR,
						&carrier_eeprom);
		} else if (board_id == BOARD_ID_DART) {
			env_set("board_name", "DART-MX8M-PLUS");

			var_carrier_eeprom_read(CARRIER_EEPROM_BUS_DART, CARRIER_EEPROM_ADDR,
						&carrier_eeprom);
		}

		var_carrier_eeprom_get_revision(&carrier_eeprom, carrier_rev, sizeof(carrier_rev));
		env_set("carrier_rev", carrier_rev);

		/* SoM Features ENV */
		env_set("som_has_wbe", (ep->features & VAR_EEPROM_F_WBE) ? "1" : "0");

		/* SoM Rev ENV*/
		snprintf(som_rev, CARRIER_REV_LEN, "%ld.%ld", SOMREV_MAJOR(ep->somrev),
			 SOMREV_MINOR(ep->somrev));
		env_set("som_rev", som_rev);
	}

	var_setup_mac(ep);
	var_eeprom_print_prod_info(ep);

#ifdef CONFIG_BMP_LOGO_EXT4_EN
	struct mmc      *mmc = NULL;
	int             err = false;
	loff_t		act_read = 0;
	char dev_part_str[10];
	struct udevice *backlight;
	int x = 0;
	int y = 0;

	/* load bmp from mmc dev */
	snprintf(dev_part_str, sizeof(dev_part_str), "%d:%d", CONFIG_BMP_LOGO_MMC_DEV,
		 CONFIG_BMP_LOGO_MMC_PART);

	mmc = find_mmc_device(CONFIG_BMP_LOGO_MMC_DEV);
	if (!mmc)
		printf("MMC dev %d not found\n", CONFIG_BMP_LOGO_MMC_DEV);

	err = mmc_init(mmc);
	if (err)
		printf("MMC dev %d could not be initialized\n", CONFIG_BMP_LOGO_MMC_DEV);

	/* Load from data partition*/
	if (fs_set_blk_dev("mmc", dev_part_str, FS_TYPE_ANY))
		printf("MMC dev %d:%d not found\n", CONFIG_BMP_LOGO_MMC_DEV,
		       CONFIG_BMP_LOGO_MMC_PART);

	err = fs_read(CONFIG_BMP_LOGO_FILENAME, LOADADDR_BMP, 0, 0, &act_read);
	if (err)
		printf("BMP file %s could not be read\n", CONFIG_BMP_LOGO_FILENAME);

	splash_get_pos(&x, &y);

	err = bmp_display(LOADADDR_BMP, x, y);
  if (err ) {
	  printf("Failed to display BMP file %s \n", CONFIG_BMP_LOGO_FILENAME);
  } else {
	  printf("Display shows BMP file %s \n", CONFIG_BMP_LOGO_FILENAME);
  }
#endif

	/* Delay needed to prevent flickering */
	mdelay(30);

	/* Enable backlight */
	err = uclass_get_device_by_name(UCLASS_PANEL_BACKLIGHT, "backlight", &backlight);
	if (err) {
		printf("backlight device not found in device tree\n");
	} else {
		err = backlight_enable(backlight);
		if (err)
			printf("backlight could not be enabled\n");
	}

	return 0;
}

#ifdef CONFIG_FSL_FASTBOOT
#ifdef CONFIG_ANDROID_RECOVERY
int is_recovery_key_pressing(void)
{
	return 0; /*TODO*/
}
#endif /*CONFIG_ANDROID_RECOVERY*/
#endif /*CONFIG_FSL_FASTBOOT*/

#ifdef CONFIG_ANDROID_SUPPORT
bool is_power_key_pressed(void)
{
	return (bool)(!!(readl(SNVS_HPSR) & (0x1 << 6)));
}
#endif

#ifdef CONFIG_SPL_MMC

#define UBOOT_RAW_SECTOR_OFFSET 0x40
unsigned long spl_mmc_get_uboot_raw_sector(struct mmc *mmc, unsigned long raw_sect)
{
	u32 boot_dev = spl_boot_device();

	switch (boot_dev) {
	case BOOT_DEVICE_MMC2:
		return CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR - UBOOT_RAW_SECTOR_OFFSET;
	default:
		return CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR;
	}
}
#endif
