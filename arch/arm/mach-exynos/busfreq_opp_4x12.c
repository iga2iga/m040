/* linux/arch/arm/mach-exynos/busfreq_opp_4x12.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * EXYNOS4 - BUS clock frequency scaling support with OPP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/ktime.h>
#include <linux/tick.h>
#include <linux/kernel_stat.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/opp.h>
#include <linux/clk.h>
#include <linux/pm_qos.h>
#include <linux/platform_data/exynos4_ppmu.h>

#include <mach/busfreq_exynos4.h>

#include <asm/mach-types.h>

#include <mach/ppmu.h>
#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/gpio.h>
#include <mach/regs-mem.h>
#include <mach/dev.h>
#include <mach/asv.h>
#include <mach/smc.h>

#include <plat/map-s5p.h>
#include <plat/gpio-cfg.h>
#include <plat/cpu.h>

#define UP_THRESHOLD			30
#define EXYNOS4412_DMC_MAX_THRESHOLD	35
#define EXYNOS4212_DMC_MAX_THRESHOLD	35
#define PPMU_THRESHOLD                  5
#define IDLE_THRESHOLD			4
#define UP_CPU_THRESHOLD		11
#define MAX_CPU_THRESHOLD		20
#define CPU_SLOPE_SIZE			7

#if defined(CONFIG_REGULATOR_MAX77686)
extern int max77686_update_dvs_voltage(struct regulator_dev *,
			const int *,const unsigned int);
#endif

static unsigned int dmc_max_threshold;

/* To save/restore DMC_PAUSE_CTRL register */
static unsigned int dmc_pause_ctrl;

static uint abb_int = ABB_MODE_130V;
static uint abb_mif = ABB_MODE_130V;
static uint abb_min = ABB_MODE_100V;
module_param_named(abb_int, abb_int, uint, 0644);
module_param_named(abb_mif, abb_mif, uint, 0644);
module_param_named(abb_min, abb_min, uint, 0644);

static unsigned int int_add = 0;	//add 12500uV every seeting
module_param_named(int_add, int_add, uint, 0644);

enum busfreq_level_idx {
	LV_0,
	LV_1,
	LV_2,
	LV_3,
	LV_4,
	LV_5,
	LV_6,
	LV_END
};

static struct busfreq_table *exynos4_busfreq_table;

static struct busfreq_table exynos4_busfreq_table_orig[] = {
	{LV_0, 400266, 1100000, 0, 0, 0}, /* MIF : 400MHz INT : 266MHz */
	{LV_1, 400200, 1100000, 0, 0, 0}, /* MIF : 400MHz INT : 200MHz */
	{LV_2, 267200, 1000000, 0, 0, 0}, /* MIF : 267MHz INT : 200MHz */
	{LV_3, 267160, 1000000, 0, 0, 0}, /* MIF : 267MHz INT : 160MHz */
	{LV_4, 160160, 950000, 0, 0, 0},  /* MIF : 160MHz INT : 160MHz */
	{LV_5, 133133, 950000, 0, 0, 0},  /* MIF : 133MHz INT : 133MHz */
	{LV_6, 100100, 950000, 0, 0, 0},  /* MIF : 100MHz INT : 100MHz */
};
#ifdef CONFIG_EXYNOS4412_MPLL_880MHZ
static struct busfreq_table exynos4_busfreq_table_rev2[] = {
	{LV_0, 440293, 1100000, 0, 0, 0}, /* MIF : 440MHz INT : 220MHz */
	{LV_1, 440220, 1100000, 0, 0, 0}, /* MIF : 440MHz INT : 220MHz */
	{LV_2, 293220, 1000000, 0, 0, 0}, /* MIF : 293MHz INT : 220MHz */
	{LV_3, 293176, 1000000, 0, 0, 0}, /* MIF : 293MHz INT : 176MHz */
	{LV_4, 176176,  950000, 0, 0, 0},  /* MIF : 176MHz INT : 176MHz */
	{LV_5, 147147,  950000, 0, 0, 0},  /* MIF : 147MHz INT : 147MHz */
	{LV_6, 110110,  950000, 0, 0, 0},  /* MIF : 110MHz INT : 110MHz */
};
#endif
enum busfreq_qos_target {
	BUS_QOS_0,
	BUS_QOS_1,
	BUS_QOS_MAX,
};

static enum busfreq_qos_target busfreq_qos = BUS_QOS_0;
module_param_named(busfreq_qos, busfreq_qos, uint, 0644);

/*
  * QOSAC_GDL
  * [0] Imgae Block: "1" Full Resource, "0" Tidemark
  * [1] TV Block: "1" Full Resource, "0" Tidemark
  * [2] G3D Block: "1" Full Resource, "0" Tidemark
  * [3] MFC_L Block: "1" Full Resource, "0" Tidemark
  *
  * QOSSAC_GDR
  * [0] CAM Block: "1" Full Resource, "0" Tidemark
  * [1] LCD0 Block: "1" Full Resource, "0" Tidemark
  * [2] LCD1 Block: "1" Full Resource, "0" Tidemark
  * [3] FSYS Block: "1" Full Resource, "0" Tidemark
  * [4] MFC_R Block: "1" Full Resource, "0" Tidemark
  * [5] MAUDIO Block: "1" Full Resource, "0" Tidemark
  */
static unsigned int exynos4_qos_value[BUS_QOS_MAX][LV_END][4] = {
	/* Normal Status */
	{
		{0x00, 0x00, 0x00, 0x00},	/* MIF : 400MHz INT : 266MHz */
		{0x00, 0x00, 0x00, 0x00},	/* MIF : 400MHz INT : 200MHz */
		{0x06, 0x03, 0x06, 0x0e},	/* MIF : 267MHz INT : 200MHz */
		{0x06, 0x03, 0x06, 0x0e},	/* MIF : 267MHz INT : 160MHz */
		{0x03, 0x03, 0x03, 0x0e},	/* MIF : 160MHz INT : 160MHz */
		{0x03, 0x03, 0x03, 0x0e},	/* MIF : 133MHz INT : 133MHz */
		{0x03, 0x0b, 0x03, 0x02},	/* MIF : 100MHz INT : 100MHz */
	},
	/* Fimc capture, MFC */
	{
		{0x06, 0x0b, 0x06, 0x0f},	/* MIF : 400MHz INT : 266MHz */
		{0x06, 0x0b, 0x06, 0x0f},	/* MIF : 400MHz INT : 200MHz */
		{0x06, 0x0b, 0x06, 0x0f},	/* MIF : 267MHz INT : 200MHz */
		{0x06, 0x0b, 0x06, 0x0f},	/* MIF : 267MHz INT : 160MHz */
		{0x06, 0x03, 0x06, 0x0e},	/* MIF : 160MHz INT : 160MHz */
		{0x04, 0x03, 0x04, 0x0e},	/* MIF : 133MHz INT : 133MHz */
		{0x03, 0x0b, 0x03, 0x02},	/* MIF : 100MHz INT : 100MHz */
	},
};

#define ASV_GROUP	12
#define PRIME_ASV_GROUP	13

static unsigned int asv_group_index;

static unsigned int (*exynos4_mif_volt)[LV_END];
static unsigned int (*exynos4_int_volt)[LV_END];

static unsigned int exynos4212_mif_volt[ASV_GROUP][LV_END] = {
	/* 400      400      267      267      160     133     100 */
	{1012500, 1012500, 962500,  962500,  912500, 912500, 912500}, /* RESERVED */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV1 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV2 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV3 */
	{1050000, 1050000, 1000000,  1000000,  900000, 900000, 900000}, /* ASV4 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV5 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV6 */
	{950000, 950000, 900000,  900000,  900000, 900000, 900000}, /* ASV7 */
	{950000, 950000, 900000,  900000,  900000, 900000, 850000}, /* ASV8 */
	{950000, 950000, 900000,  900000,  900000, 900000, 850000}, /* ASV9 */
	{950000, 950000, 900000,  900000,  900000, 850000, 850000}, /* ASV10 */
	{937500, 937500, 887500,  887500,  887500, 850000, 850000}, /* RESERVED */
};

static unsigned int exynos4212_int_volt[ASV_GROUP][LV_END] = {
	/* 266      200       200     160    160      133     100 */
	{1300000, 1250000, 1250000, 950000, 950000, 912500, 887500}, /* RESERVED */
	{1062500, 1012500, 1012500, 937500, 937500, 900000, 875000}, /* ASV1 */
	{1050000, 1000000, 1000000, 925000, 925000, 887500, 875000}, /* ASV2 */
	{1050000, 1000000, 1000000, 912500, 912500, 887500, 875000}, /* ASV3 */
	{1062500, 1012500, 1012500, 925000, 925000, 900000, 875000}, /* ASV4 */
	{1050000, 1000000, 1000000, 925000, 925000, 887500, 875000}, /* ASV5 */
	{1050000, 1000000, 1000000, 912500, 912500, 887500, 875000}, /* ASV6 */
	{1037500, 987500, 987500, 912500, 912500, 875000, 875000}, /* ASV7 */
	{1037500, 987500, 987500, 900000, 900000, 875000, 875000}, /* ASV8 */
	{1037500, 987500, 987500, 900000, 900000, 875000, 875000}, /* ASV9 */
	{1037500, 987500, 987500, 900000, 900000, 862500, 850000}, /* ASV10 */
	{1035000, 975000, 975000, 887500, 887500, 850000, 850000}, /* RESERVED */
};

/* 20110105 DVFS table */
static unsigned int exynos4412_mif_volt[ASV_GROUP][LV_END] = {
	/* 400      400      267      267      160     133     100 */
	{1100000, 1100000, 1000000, 1000000, 950000, 950000, 950000}, /* RESERVED */
	{1050000, 1050000, 950000,  950000,  900000, 900000, 900000}, /* RESERVED */
	{1050000, 1050000, 950000,  950000,  900000, 900000, 900000}, /* ASV2 */
	{1050000, 1050000, 950000,  950000,  900000, 900000, 900000}, /* ASV3 */
	{1050000, 1050000, 950000,  950000,  900000, 900000, 900000}, /* ASV4 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV5 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV6 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV7 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 900000}, /* ASV8 */
	{1000000, 1000000, 950000,  950000,  900000, 900000, 850000}, /* ASV9 */
	{1000000, 1000000, 900000,  900000,  900000, 900000, 850000}, /* ASV10 */
	{1000000, 1000000, 900000,  900000,  900000, 900000, 850000}, /* RESERVED */
};

static unsigned int exynos4412_int_volt[ASV_GROUP][LV_END] = {
  /* GDR : 266       200      200     160    160      133     100 */
	{1112500, 1062500, 1062500, 975000, 975000, 937500, 900000}, /* RESERVED */
	{1100000, 1050000, 1050000, 962500, 962500, 925000, 887500}, /* RESERVED */
	{1075000, 1025000, 1025000, 937500, 937500, 912500, 875000}, /* ASV2 */
	{1062500, 1012500, 1012500, 937500, 937500, 900000, 862500}, /* ASV3 */
	{1062500, 1012500, 1012500, 925000, 925000, 900000, 862500}, /* ASV4 */
	{1050000, 1000000, 1000000, 925000, 925000, 887500, 850000}, /* ASV5 */
	{1050000, 1000000, 1000000, 912500, 912500, 875000, 850000}, /* ASV6 */
	{1037500,  987500,  987500, 912500, 912500, 862500, 850000}, /* ASV7 */
	{1037500,  987500,  987500, 900000, 900000, 862500, 850000}, /* ASV8 */
	{1037500,  987500,  987500, 900000, 900000, 862500, 850000}, /* ASV9 */
	{1037500,  987500,  987500, 900000, 900000, 862500, 850000}, /* ASV10 */
	{1025000,  975000,  975000, 887500, 887500, 850000, 850000}, /* RESERVED */
};

#if 0
static unsigned int exynos4412_mif_volt_rev2[ASV_GROUP][LV_END] = {
	/* 440      440      293      293      176     146     110 */
	{1100000, 1100000, 1000000, 1000000,  950000, 950000, 937500}, /* RESERVED */
	{1100000, 1100000, 1000000, 1000000,  950000, 950000, 950000}, /* RESERVED */
	{1075000, 1075000,  975000,  975000,  925000, 925000, 912500}, /* ASV2 */
	{1062500, 1062500,  962500,  962500,  912500, 912500, 900000}, /* ASV3 */
	{1050000, 1050000,  950000,  950000,  900000, 900000, 887500}, /* ASV4 */
	{1037500, 1037500,  937500,  937500,  887500, 887500, 875000}, /* ASV5 */
	{1037500, 1037500,  937500,  937500,  887500, 887500, 875000}, /* ASV6 */
	{1025000, 1025000,  925000,  925000,  875000, 875000, 862500}, /* ASV7 */
	{1037500, 1037500,  937500,  937500,  887500, 887500, 875000}, /* ASV8 */
	{1025000, 1025000,  925000,  925000,  875000, 875000, 862500}, /* ASV9 */
	{1025000, 1025000,  925000,  925000,  875000, 875000, 862500}, /* ASV10 */
	{1012500, 1012500,  912500,  912500,  862500, 862500, 850000}, /* RESERVED */
};

static unsigned int exynos4412_int_volt_rev2[ASV_GROUP][LV_END] = {
  /* GDR : 266       220      220     176    176      146     110 */
	{1112500, 1075000, 1075000,  987500,  987500, 950000, 925000}, /* RESERVED */
	{1100000, 1062500, 1062500,  975000,  975000, 937500, 912500}, /* RESERVED */
	{1075000, 1050000, 1050000,  962500,  962500, 925000, 900000}, /* ASV2 */
	{1062500, 1037500, 1037500,  950000,  950000, 912500, 887500}, /* ASV3 */
	{1062500, 1025000, 1025000,  937500,  937500, 900000, 875000}, /* ASV4 */
	{1050000, 1012500, 1012500,  925000,  925000, 887500, 862500}, /* ASV5 */
	{1050000, 1000000, 1000000,  912500,  912500, 875000, 850000}, /* ASV6 */
	{1037500,  987500,  987500,  900000,  900000, 862500, 837500}, /* ASV7 */
	{1037500, 1025000, 1025000,  937500,  937500, 900000, 875000}, /* ASV8 */
	{1037500, 1012500, 1012500,  925000,  925000, 887500, 862500}, /* ASV9 */
	{1037500, 1000000, 1000000,  912500,  912500, 875000, 850000}, /* ASV10 */
	{1025000,  987500,  987500,  900000,  900000, 862500, 837500}, /* RESERVED */
};
#else
/* 20120822 DVFS table for pega prime */
/* Because buck1 of pmic can be set to 50mV step size, 50mV table is used */
static unsigned int exynos4412_mif_volt_rev2[PRIME_ASV_GROUP][LV_END] = {
	/* 440      440      293      293      176     147     110 */
	{1100000, 1100000, 1000000, 1000000,  950000, 950000, 950000}, /* RESERVED */
	{1100000, 1100000, 1000000, 1000000,  950000, 950000, 950000}, /* ASV1 */
	{1100000, 1100000, 1000000, 1000000,  950000, 950000, 900000}, /* ASV2 */
	{1100000, 1100000, 1000000, 1000000,  950000, 900000, 900000}, /* ASV3 */
	{1050000, 1050000,  950000,  950000,  900000, 900000, 900000}, /* ASV4 */
	{1050000, 1050000,  950000,  950000,  900000, 900000, 900000}, /* ASV5 */
	{1050000, 1050000,  950000,  950000,  900000, 900000, 900000}, /* ASV6 */
	{1050000, 1050000,  950000,  950000,  900000, 900000, 850000}, /* ASV7 */
	{1050000, 1050000,  950000,  950000,  900000, 850000, 850000}, /* ASV8 */
	{1000000, 1000000,  900000,  900000,  850000, 850000, 850000}, /* ASV9 */
	{1000000, 1000000,  900000,  900000,  850000, 850000, 850000}, /* ASV10 */
	{1000000, 1000000,  900000,  900000,  850000, 850000, 850000}, /* ASV11 */
	{ 950000,  950000,  850000,  850000,  850000, 850000, 850000}, /* ASV12 */
};

/* 20120822 DVFS table for pega prime */
static unsigned int exynos4412_int_volt_rev2[PRIME_ASV_GROUP][LV_END] = {
  /* GDR : 293       220      220     176      176      147     110 */
	{1087500, 1062500, 1062500, 1000000, 1000000, 962500, 950000}, /* RESERVED */
	{1075000, 1050000, 1050000,  987500,  987500, 950000, 937500}, /* ASV1 */
	{1062500, 1037500, 1037500,  975000,  975000, 937500, 912500}, /* ASV2 */
	{1050000, 1037500, 1037500,  975000,  975000, 937500, 900000}, /* ASV3 */
	{1037500, 1025000, 1025000,  962500,  962500, 925000, 887500}, /* ASV4 */
	{1025000, 1012500, 1012500,  950000,  950000, 912500, 887500}, /* ASV5 */
	{1012500, 1000000, 1000000,  937500,  937500, 900000, 887500}, /* ASV6 */
	{1000000,  987500,  987500,  925000,  925000, 887500, 875000}, /* ASV7 */
	{1037500,  975000,  975000,  912500,  912500, 875000, 875000}, /* ASV8 */
	{1025000,  962500,  962500,  900000,  900000, 875000, 875000}, /* ASV9 */
	{1012500,  937500,  937500,  875000,  875000, 850000, 850000}, /* ASV10 */
	{1000000,  925000,  925000,  862500,  862500, 850000, 850000}, /* ASV11 */
	{1000000,  912500,  912500,  850000,  850000, 850000, 850000}, /* ASV12 */
};
#endif

static unsigned int exynos4x12_timingrow[LV_END] = {
	/* 400            400                267              267              160             133              100 */
	0x34498691, 0x34498691, 0x24488490, 0x24488490, 0x154882D0, 0x154882D0, 0x0D488210
};

static unsigned int clkdiv_dmc0[LV_END][6] = {
	/*
	 * Clock divider value for following
	 * { DIVACP, DIVACP_PCLK, DIVDPHY, DIVDMC, DIVDMCD
	 *              DIVDMCP}
	 */

	/* DMC L0: 400MHz */
	{3, 1, 1, 1, 1, 1},

	/* DMC L1: 400MHz */
	{3, 1, 1, 1, 1, 1},

	/* DMC L2: 266.7MHz */
	{4, 1, 1, 2, 1, 1},

	/* DMC L3: 266.7MHz */
	{4, 1, 1, 2, 1, 1},

	/* DMC L4: 160MHz */
	{5, 1, 1, 4, 1, 1},

	/* DMC L5: 133MHz */
	{5, 1, 1, 5, 1, 1},

	/* DMC L6: 100MHz */
	{7, 1, 1, 7, 1, 1},
};

static unsigned int clkdiv_dmc1[LV_END][6] = {
	/*
	 * Clock divider value for following
	 * { G2DACP, DIVC2C, DIVC2C_ACLK }
	 */

	/* DMC L0: 400MHz */
	{3, 1, 1},

	/* DMC L1: 400MHz */
	{3, 1, 1},

	/* DMC L2: 266.7MHz */
	{4, 2, 1},

	/* DMC L3: 266.7MHz */
	{4, 2, 1},

	/* DMC L4: 160MHz */
	{5, 4, 1},

	/* DMC L5: 133MHz */
	{5, 5, 1},

	/* DMC L6: 100MHz */
	{7, 7, 1},
};

static unsigned int clkdiv_top[LV_END][5] = {
	/*
	 * Clock divider value for following
	 * { DIVACLK266_GPS, DIVACLK100, DIVACLK160,
		DIVACLK133, DIVONENAND }
	 */

	/* ACLK_GDL/R L0: 266MHz */
	{2, 7, 4, 5, 1},

	/* ACLK_GDL/R L1: 200MHz */
	{2, 7, 4, 5, 1},

	/* ACLK_GDL/R L2: 200MHz */
	{2, 7, 4, 5, 1},

	/* ACLK_GDL/R L3: 160MHz */
	{4, 7, 5, 7, 1},

	/* ACLK_GDL/R L4: 160MHz */
	{4, 7, 5, 7, 1},

	/* ACLK_GDL/R L5: 133MHz */
	{5, 7, 5, 7, 1},

	/* ACLK_GDL/R L6: 100MHz */
	{7, 7, 7, 7, 1},
};

static unsigned int clkdiv_l_bus[LV_END][2] = {
	/*
	 * Clock divider value for following
	 * { DIVGDL, DIVGPL }
	 */

	/* ACLK_GDL L0: 200MHz */
	{3, 1},

	/* ACLK_GDL L1: 200MHz */
	{3, 1},

	/* ACLK_GDL L2: 200MHz */
	{3, 1},

	/* ACLK_GDL L3: 160MHz */
	{4, 1},

	/* ACLK_GDL L4: 160MHz */
	{4, 1},

	/* ACLK_GDL L5: 133MHz */
	{5, 1},

	/* ACLK_GDL L6: 100MHz */
	{7, 1},
};

static unsigned int clkdiv_r_bus[LV_END][2] = {
	/*
	 * Clock divider value for following
	 * { DIVGDR, DIVGPR }
	 */

	/* ACLK_GDR L0: 266MHz */
	{2, 1},

	/* ACLK_GDR L1: 200MHz */
	{3, 1},

	/* ACLK_GDR L2: 200MHz */
	{3, 1},

	/* ACLK_GDR L3: 160MHz */
	{4, 1},

	/* ACLK_GDR L4: 160MHz */
	{4, 1},

	/* ACLK_GDR L5: 133MHz */
	{5, 1},

	/* ACLK_GDR L6: 100MHz */
	{7, 1},
};

static unsigned int clkdiv_sclkip[LV_END][3] = {
	/*
	 * Clock divider value for following
	 * { DIVMFC, DIVJPEG, DIVFIMC0~3}
	 */

	/* SCLK_MFC: 200MHz */
	{3, 3, 4},

	/* SCLK_MFC: 200MHz */
	{3, 3, 4},

	/* SCLK_MFC: 200MHz */
	{3, 3, 4},

	/* SCLK_MFC: 160MHz */
	{4, 4, 5},

	/* SCLK_MFC: 160MHz */
	{4, 4, 5},

	/* SCLK_MFC: 133MHz */
	{5, 5, 5},

	/* SCLK_MFC: 100MHz */
	{7, 7, 7},
};

static void exynos4x12_set_bus_volt(void)
{
	unsigned int i;
	bool mif_locking = false, int_locking = false;

	asv_group_index = exynos_result_of_asv;

	if (asv_group_index == 0xff)
		asv_group_index = 0;
	
	if ((is_special_flag() >> MIF_LOCK_FLAG) & 0x1)
		mif_locking = true;

	if ((is_special_flag() >> INT_LOCK_FLAG) & 0x1)
		int_locking = true;

	pr_info("DVFS : VDD_INT Voltage table set with %d Group\n", asv_group_index);

	for (i = 0 ; i < LV_END ; i++) {
		exynos4_busfreq_table[i].volt =
			exynos4_mif_volt[asv_group_index][i] + int_add;
		
		if (mif_locking)
			exynos4_busfreq_table[i].volt += 50000;

		if (int_locking)
			exynos4_int_volt[asv_group_index][i] += 25000;
	}

	return;
}

static void exynos4x12_target(int index)
{
	unsigned int tmp;

	/* Change Divider - DMC0 */
	tmp = exynos4_busfreq_table[index].clk_dmc0div;

	__raw_writel(tmp, EXYNOS4_CLKDIV_DMC0);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_DMC0);
	} while (tmp & 0x111111);

	/* Change Divider - DMC1 */
	tmp = __raw_readl(EXYNOS4_CLKDIV_DMC1);

	tmp &= ~(EXYNOS4_CLKDIV_DMC1_G2D_ACP_MASK |
		EXYNOS4_CLKDIV_DMC1_C2C_MASK |
		EXYNOS4_CLKDIV_DMC1_C2CACLK_MASK);

	tmp |= ((clkdiv_dmc1[index][0] << EXYNOS4_CLKDIV_DMC1_G2D_ACP_SHIFT) |
		(clkdiv_dmc1[index][1] << EXYNOS4_CLKDIV_DMC1_C2C_SHIFT) |
		(clkdiv_dmc1[index][2] << EXYNOS4_CLKDIV_DMC1_C2CACLK_SHIFT));

	__raw_writel(tmp, EXYNOS4_CLKDIV_DMC1);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_DMC1);
	} while (tmp & 0x1011);

	/* Change Divider - TOP */
	tmp = __raw_readl(EXYNOS4_CLKDIV_TOP);

	tmp &= ~(EXYNOS4_CLKDIV_TOP_ACLK266_GPS_MASK |
		EXYNOS4_CLKDIV_TOP_ACLK100_MASK |
		EXYNOS4_CLKDIV_TOP_ACLK160_MASK |
		EXYNOS4_CLKDIV_TOP_ACLK133_MASK |
		EXYNOS4_CLKDIV_TOP_ONENAND_MASK);

	tmp |= ((clkdiv_top[index][0] << EXYNOS4_CLKDIV_TOP_ACLK266_GPS_SHIFT) |
		(clkdiv_top[index][1] << EXYNOS4_CLKDIV_TOP_ACLK100_SHIFT) |
		(clkdiv_top[index][2] << EXYNOS4_CLKDIV_TOP_ACLK160_SHIFT) |
		(clkdiv_top[index][3] << EXYNOS4_CLKDIV_TOP_ACLK133_SHIFT) |
		(clkdiv_top[index][4] << EXYNOS4_CLKDIV_TOP_ONENAND_SHIFT));

	__raw_writel(tmp, EXYNOS4_CLKDIV_TOP);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_TOP);
	} while (tmp & 0x11111);

	/* Change Divider - LEFTBUS */
	tmp = __raw_readl(EXYNOS4_CLKDIV_LEFTBUS);

	tmp &= ~(EXYNOS4_CLKDIV_BUS_GDLR_MASK | EXYNOS4_CLKDIV_BUS_GPLR_MASK);

	tmp |= ((clkdiv_l_bus[index][0] << EXYNOS4_CLKDIV_BUS_GDLR_SHIFT) |
		(clkdiv_l_bus[index][1] << EXYNOS4_CLKDIV_BUS_GPLR_SHIFT));

	__raw_writel(tmp, EXYNOS4_CLKDIV_LEFTBUS);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_LEFTBUS);
	} while (tmp & 0x11);

	/* Change Divider - RIGHTBUS */
	tmp = __raw_readl(EXYNOS4_CLKDIV_RIGHTBUS);

	tmp &= ~(EXYNOS4_CLKDIV_BUS_GDLR_MASK | EXYNOS4_CLKDIV_BUS_GPLR_MASK);

	tmp |= ((clkdiv_r_bus[index][0] << EXYNOS4_CLKDIV_BUS_GDLR_SHIFT) |
		(clkdiv_r_bus[index][1] << EXYNOS4_CLKDIV_BUS_GPLR_SHIFT));

	__raw_writel(tmp, EXYNOS4_CLKDIV_RIGHTBUS);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_RIGHTBUS);
	} while (tmp & 0x11);

	/* Change Divider - MFC */
	tmp = __raw_readl(EXYNOS4_CLKDIV_MFC);

	tmp &= ~(EXYNOS4_CLKDIV_MFC_MASK);

	tmp |= ((clkdiv_sclkip[index][0] << EXYNOS4_CLKDIV_MFC_SHIFT));

	__raw_writel(tmp, EXYNOS4_CLKDIV_MFC);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_MFC);
	} while (tmp & 0x1);

	/* Change Divider - JPEG */
	tmp = __raw_readl(EXYNOS4_CLKDIV_CAM1);

	tmp &= ~(EXYNOS4_CLKDIV_CAM1_JPEG_MASK);

	tmp |= ((clkdiv_sclkip[index][1] << EXYNOS4_CLKDIV_CAM1_JPEG_SHIFT));

	__raw_writel(tmp, EXYNOS4_CLKDIV_CAM1);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_CAM1);
	} while (tmp & 0x1);

	/* Change Divider - FIMC0~3 */
	tmp = __raw_readl(EXYNOS4_CLKDIV_CAM);

	tmp &= ~(EXYNOS4_CLKDIV_CAM_FIMC0_MASK | EXYNOS4_CLKDIV_CAM_FIMC1_MASK |
		EXYNOS4_CLKDIV_CAM_FIMC2_MASK | EXYNOS4_CLKDIV_CAM_FIMC3_MASK);

	tmp |= ((clkdiv_sclkip[index][2] << EXYNOS4_CLKDIV_CAM_FIMC0_SHIFT) |
		(clkdiv_sclkip[index][2] << EXYNOS4_CLKDIV_CAM_FIMC1_SHIFT) |
		(clkdiv_sclkip[index][2] << EXYNOS4_CLKDIV_CAM_FIMC2_SHIFT) |
		(clkdiv_sclkip[index][2] << EXYNOS4_CLKDIV_CAM_FIMC3_SHIFT));

	__raw_writel(tmp, EXYNOS4_CLKDIV_CAM);

	do {
		tmp = __raw_readl(EXYNOS4_CLKDIV_STAT_CAM1);
	} while (tmp & 0x1111);

	if (soc_is_exynos4412() && (exynos_result_of_asv > 3)) {
		if (100100 == exynos4_busfreq_table[index].mem_clk) {/* MIF:100 / INT:100 */
			exynos4x12_set_abb_member(ABB_INT, abb_min);
			exynos4x12_set_abb_member(ABB_MIF, abb_min);
		} else {
			exynos4x12_set_abb_member(ABB_INT, abb_int);
			exynos4x12_set_abb_member(ABB_MIF, abb_mif);
		}
	}
}

static unsigned int exynos4x12_get_table_index(struct opp *opp)
{
	unsigned int index;

	for (index = LV_0; index < LV_END; index++)
		if (opp_get_freq(opp) == exynos4_busfreq_table[index].mem_clk)
			break;

	return index;
}

/*need?*/
static void exynos4x12_prepare(unsigned int index)
{
	unsigned int timing0 = 0;

	if (trustzone_on()) {
		exynos_smc_readsfr(EXYNOS4_PA_DMC0_4212 + TIMINGROW_OFFSET, &timing0);
		timing0 |= exynos4x12_timingrow[index];
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC0_4212 + TIMINGROW_OFFSET),
				timing0, 0);
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC0_4212 + TIMINGROW_OFFSET),
			exynos4x12_timingrow[index], 0);
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC1_4212 + TIMINGROW_OFFSET),
				timing0, 0);
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC1_4212 + TIMINGROW_OFFSET),
				exynos4x12_timingrow[index], 0);
	} else {
		timing0 = __raw_readl(S5P_VA_DMC0 + TIMINGROW_OFFSET);
		timing0 |= exynos4x12_timingrow[index];
		__raw_writel(timing0, S5P_VA_DMC0 + TIMINGROW_OFFSET);
		__raw_writel(exynos4x12_timingrow[index], S5P_VA_DMC0 + TIMINGROW_OFFSET);
		__raw_writel(timing0, S5P_VA_DMC1 + TIMINGROW_OFFSET);
		__raw_writel(exynos4x12_timingrow[index], S5P_VA_DMC1 + TIMINGROW_OFFSET);
	}	
}

static void exynos4x12_post(unsigned int index)
{
	unsigned int timing0 = 0;

	if (trustzone_on()) {
		exynos_smc_readsfr(EXYNOS4_PA_DMC0_4212 + TIMINGROW_OFFSET, &timing0);
		timing0 |= exynos4x12_timingrow[index];
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC0_4212 + TIMINGROW_OFFSET),
				timing0, 0);
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC0_4212 + TIMINGROW_OFFSET),
				exynos4x12_timingrow[index], 0);
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC1_4212 + TIMINGROW_OFFSET),
				timing0, 0);
		exynos_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(EXYNOS4_PA_DMC1_4212 + TIMINGROW_OFFSET),
				exynos4x12_timingrow[index], 0);
	} else {
		timing0 = __raw_readl(S5P_VA_DMC0 + TIMINGROW_OFFSET);
		timing0 |= exynos4x12_timingrow[index];
		__raw_writel(timing0, S5P_VA_DMC0 + TIMINGROW_OFFSET);
		__raw_writel(exynos4x12_timingrow[index], S5P_VA_DMC0 + TIMINGROW_OFFSET);
		__raw_writel(timing0, S5P_VA_DMC1 + TIMINGROW_OFFSET);
		__raw_writel(exynos4x12_timingrow[index], S5P_VA_DMC1 + TIMINGROW_OFFSET);
	}
}

static void exynos4x12_set_qos(unsigned int index)
{
	__raw_writel(exynos4_qos_value[busfreq_qos][index][0], S5P_VA_GDL + 0x400);
	__raw_writel(exynos4_qos_value[busfreq_qos][index][1], S5P_VA_GDL + 0x404);
	__raw_writel(exynos4_qos_value[busfreq_qos][index][2], S5P_VA_GDR + 0x400);
	__raw_writel(exynos4_qos_value[busfreq_qos][index][3], S5P_VA_GDR + 0x404);
}

static void exynos4x12_suspend(void)
{
	/* Nothing to do */
}

static void exynos4x12_resume(void)
{
	__raw_writel(dmc_pause_ctrl, EXYNOS4_DMC_PAUSE_CTRL);
}

static unsigned int exynos4x12_get_int_volt(unsigned int index)
{
	return exynos4_int_volt[asv_group_index][index] + int_add;
}

#ifdef CONFIG_EXYNOS_TMU_TC
/**
 * exynos4x12_find_busfreq_by_volt - find busfreq by requested voltage.
 *
 * This function finds the busfreq to set for voltage above req_volt
 * and return its value.
 */
int exynos4x12_find_busfreq_by_volt(unsigned int req_volt, unsigned long *freq)
{
	unsigned int volt_cmp;
	int i;

	/* check if req_volt has value or not */
	if (!req_volt) {
		pr_err("%s: req_volt has no value.\n", __func__);
		return -EINVAL;
	}

	/* find busfreq level in busfreq_table */
	for (i = LV_END - 1; i >= 0; i--) {
		volt_cmp = min(exynos4_int_volt[asv_group_index][i],
				exynos4_mif_volt[asv_group_index][i]);

		if (volt_cmp >= req_volt) {
			*freq = exynos4_busfreq_table[i].mem_clk;
			return 0;
		}
	}
	pr_err("%s: %u volt can't support\n", __func__, req_volt);

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(exynos4x12_find_busfreq_by_volt);
#endif

static struct opp *exynos4x12_monitor(struct busfreq_data *data)
{
	struct opp *opp = data->curr_opp;
	int i;
	unsigned int cpu_load_average = 0;
	unsigned int dmc0_load_average = 0;
	unsigned int dmc1_load_average = 0;
	unsigned int dmc_load_average;
	unsigned long cpufreq = 0;
	unsigned long lockfreq;
	unsigned long dmcfreq;
	unsigned long newfreq;
	unsigned long currfreq = opp_get_freq(data->curr_opp) / 1000;
	unsigned long maxfreq = opp_get_freq(data->max_opp) / 1000;
	unsigned long cpu_load;
	unsigned long dmc0_load;
	unsigned long dmc1_load;
	unsigned long dmc_load;
	int cpu_load_slope;

#ifdef CONFIG_EXYNOS4_DEV_PPMU
	struct exynos4_ppmu_data ppmu_data;
	ppmu_data = exynos4_ppmu_update(PPMU_CPU);
	cpu_load = ppmu_data.load;
	ppmu_data = exynos4_ppmu_update(PPMU_DMC0);
	dmc0_load = div64_u64(ppmu_data.load * currfreq, maxfreq);
	ppmu_data = exynos4_ppmu_update(PPMU_DMC1);
	dmc1_load = div64_u64(ppmu_data.load * currfreq, maxfreq);
#endif

	cpu_load_slope = cpu_load -
		data->load_history[PPMU_CPU][data->index ? data->index - 1 : LOAD_HISTORY_SIZE - 1];

	data->load_history[PPMU_CPU][data->index] = cpu_load;
	data->load_history[PPMU_DMC0][data->index] = dmc0_load;
	data->load_history[PPMU_DMC1][data->index++] = dmc1_load;

	if (data->index >= LOAD_HISTORY_SIZE)
		data->index = 0;

	for (i = 0; i < LOAD_HISTORY_SIZE; i++) {
		cpu_load_average += data->load_history[PPMU_CPU][i];
		dmc0_load_average += data->load_history[PPMU_DMC0][i];
		dmc1_load_average += data->load_history[PPMU_DMC1][i];
	}

	/* Calculate average Load */
	cpu_load_average /= LOAD_HISTORY_SIZE;
	dmc0_load_average /= LOAD_HISTORY_SIZE;
	dmc1_load_average /= LOAD_HISTORY_SIZE;

	if (dmc0_load >= dmc1_load) {
		dmc_load = dmc0_load;
		dmc_load_average = dmc0_load_average;
	} else {
		dmc_load = dmc1_load;
		dmc_load_average = dmc1_load_average;
	}

	if (cpu_load >= UP_CPU_THRESHOLD) {
		cpufreq = opp_get_freq(data->max_opp);
		if (cpu_load < MAX_CPU_THRESHOLD) {
			opp = data->curr_opp;
			if (cpu_load_slope > CPU_SLOPE_SIZE) {
				cpufreq--;
				opp = opp_find_freq_floor(data->dev, &cpufreq);
			}
			cpufreq = opp_get_freq(opp);
		}
	}

	if (dmc_load >= dmc_max_threshold) {
		dmcfreq = opp_get_freq(data->max_opp);
	} else if (dmc_load < IDLE_THRESHOLD) {
		if (dmc_load_average < IDLE_THRESHOLD)
			opp = step_down(data, 1);
		else
			opp = data->curr_opp;
		dmcfreq = opp_get_freq(opp);
	} else {
		if (dmc_load < dmc_load_average) {
			dmc_load = dmc_load_average;
			if (dmc_load >= dmc_max_threshold)
				dmc_load = dmc_max_threshold;
		}
		dmcfreq = div64_u64(maxfreq * dmc_load * 1000, dmc_max_threshold);
	}

	lockfreq = dev_max_freq(data->dev);

	newfreq = max3(lockfreq, dmcfreq, cpufreq);

	if (samsung_rev() < EXYNOS4412_REV_1_0)
		newfreq = opp_get_freq(data->max_opp);

	pr_debug("curfreq %ld, newfreq %ld, dmc0_load %ld, dmc1_load %ld, cpu_load %ld\n",
		currfreq, newfreq, dmc0_load, dmc1_load, cpu_load);

	opp = opp_find_freq_ceil(data->dev, &newfreq);
	if (IS_ERR(opp))
		opp = data->max_opp;

	return opp;
}

static int exynos4x12_bus_qos_notifiy(struct notifier_block *nb,
		unsigned long l, void *v)
{
	struct busfreq_data *bus_data = container_of(nb, struct busfreq_data,
			exynos_busqos_notifier);

	busfreq_qos = (int)l;
	exynos4x12_set_qos(bus_data->get_table_index(bus_data->curr_opp));

	return NOTIFY_OK;
}

static inline void exynos4x12_bus_qos_notifier_init(struct notifier_block *n)
{
	pm_qos_add_notifier(PM_QOS_BUS_QOS, n);
}

#define ARM_INT_CORRECTION 160160
static int exynos4x12_busfreq_cpufreq_transition(struct notifier_block *nb,
					    unsigned long val, void *data)
{
	struct cpufreq_freqs *freqs = (struct cpufreq_freqs *)data;
	struct busfreq_data *bus_data = container_of(nb, struct busfreq_data,
			exynos_cpufreq_notifier);

	switch (val) {
	case CPUFREQ_PRECHANGE:
		if (freqs->new > 900000 && freqs->old < 1000000)
			dev_lock(bus_data->dev, bus_data->dev, ARM_INT_CORRECTION);
		break;
	case CPUFREQ_POSTCHANGE:
		if (freqs->old > 900000 && freqs->new < 1000000)
			dev_unlock(bus_data->dev, bus_data->dev);
		break;
	}
	return NOTIFY_DONE;
}

int exynos4x12_init(struct device *dev, struct busfreq_data *data)
{
	unsigned int i;
	unsigned int tmp;
	unsigned long maxfreq = 0;
	unsigned long minfreq = 0;
	unsigned long freq;
	struct clk *sclk_dmc;
	int ret;

	exynos4_busfreq_table = exynos4_busfreq_table_orig;

	if (soc_is_exynos4212()) {
		exynos4_mif_volt = exynos4212_mif_volt;
		exynos4_int_volt = exynos4212_int_volt;
		dmc_max_threshold = EXYNOS4212_DMC_MAX_THRESHOLD;
	} else if (soc_is_exynos4412()) {
		if (samsung_rev() >= EXYNOS4412_REV_2_0) {
#ifdef CONFIG_EXYNOS4412_MPLL_880MHZ
			exynos4_busfreq_table = exynos4_busfreq_table_rev2;
#endif
			exynos4_mif_volt = exynos4412_mif_volt_rev2;
			exynos4_int_volt = exynos4412_int_volt_rev2;
		} else {
			exynos4_mif_volt = exynos4412_mif_volt;
			exynos4_int_volt = exynos4412_int_volt;
		}
		dmc_max_threshold = EXYNOS4412_DMC_MAX_THRESHOLD;
	} else {
		pr_err("Unsupported model.\n");
		return -EINVAL;
	}

	/* Enable pause function for DREX2 DVFS */
	dmc_pause_ctrl = __raw_readl(EXYNOS4_DMC_PAUSE_CTRL);
	dmc_pause_ctrl |= DMC_PAUSE_ENABLE;
	__raw_writel(dmc_pause_ctrl, EXYNOS4_DMC_PAUSE_CTRL);

	tmp = __raw_readl(EXYNOS4_CLKDIV_DMC0);

	for (i = 0; i <  LV_END; i++) {
		tmp &= ~(EXYNOS4_CLKDIV_DMC0_ACP_MASK |
			EXYNOS4_CLKDIV_DMC0_ACPPCLK_MASK |
			EXYNOS4_CLKDIV_DMC0_DPHY_MASK |
			EXYNOS4_CLKDIV_DMC0_DMC_MASK |
			EXYNOS4_CLKDIV_DMC0_DMCD_MASK |
			EXYNOS4_CLKDIV_DMC0_DMCP_MASK);

		tmp |= ((clkdiv_dmc0[i][0] << EXYNOS4_CLKDIV_DMC0_ACP_SHIFT) |
			(clkdiv_dmc0[i][1] << EXYNOS4_CLKDIV_DMC0_ACPPCLK_SHIFT) |
			(clkdiv_dmc0[i][2] << EXYNOS4_CLKDIV_DMC0_DPHY_SHIFT) |
			(clkdiv_dmc0[i][3] << EXYNOS4_CLKDIV_DMC0_DMC_SHIFT) |
			(clkdiv_dmc0[i][4] << EXYNOS4_CLKDIV_DMC0_DMCD_SHIFT) |
			(clkdiv_dmc0[i][5] << EXYNOS4_CLKDIV_DMC0_DMCP_SHIFT));

		exynos4_busfreq_table[i].clk_dmc0div = tmp;
	}

	exynos4x12_set_bus_volt();

	for (i = 0; i < LV_END; i++) {
		ret = opp_add(dev, exynos4_busfreq_table[i].mem_clk,
				exynos4_busfreq_table[i].volt);
		if (ret) {
			dev_err(dev, "Fail to add opp entries.\n");
			return ret;
		}
	}

	/* Disable MIF 267 INT 200 Level */
	maxfreq = 400200;
#ifdef CONFIG_EXYNOS4412_MPLL_880MHZ
	if (samsung_rev() >= EXYNOS4412_REV_2_0)
		maxfreq = 440293;
#endif
	
	data->table = exynos4_busfreq_table;
	data->table_size = LV_END;

	/* Find max frequency */
	data->max_opp = opp_find_freq_floor(dev, &maxfreq);
	data->min_opp = opp_find_freq_ceil(dev, &minfreq);

	sclk_dmc = clk_get(NULL, "sclk_dmc");

	if (IS_ERR(sclk_dmc)) {
		pr_err("Failed to get sclk_dmc.!\n");
		data->curr_opp = data->max_opp;
	} else {
		freq = clk_get_rate(sclk_dmc) / 1000;
		clk_put(sclk_dmc);
		data->curr_opp = opp_find_freq_ceil(dev, &freq);
		if(IS_ERR(data->curr_opp))
			data->curr_opp = data->max_opp;		
	}

	data->vdd_int = regulator_get(NULL, "vdd_int");
	if (IS_ERR(data->vdd_int)) {
		pr_err("failed to get resource %s\n", "vdd_int");
		return -ENODEV;
	}

#if defined(CONFIG_REGULATOR_MAX77686)
	do {
		int volt_table[8] = {0};
		for (i = 0 ; i < LV_END ; i++)
			volt_table[i] = exynos4x12_get_int_volt(i);
		ret = max77686_update_dvs_voltage(regulator_get_dev(data->vdd_int),
							volt_table, LV_END);
	} while(0);
#endif

	data->vdd_mif = regulator_get(NULL, "vdd_mif");
	if (IS_ERR(data->vdd_mif)) {
		pr_err("failed to get resource %s\n", "vdd_mif");
		regulator_put(data->vdd_int);
		return -ENODEV;
	}

	data->exynos_cpufreq_notifier.notifier_call =
				exynos4x12_busfreq_cpufreq_transition;

	if (cpufreq_register_notifier(&data->exynos_cpufreq_notifier,
		   	CPUFREQ_TRANSITION_NOTIFIER))
		pr_err("Falied to register cpufreq notifier\n");

	data->exynos_busqos_notifier.notifier_call = exynos4x12_bus_qos_notifiy;
	exynos4x12_bus_qos_notifier_init(&data->exynos_busqos_notifier);

	data->target = exynos4x12_target;
	data->get_int_volt = exynos4x12_get_int_volt;
	data->get_table_index = exynos4x12_get_table_index;
	data->monitor = exynos4x12_monitor;
	data->busfreq_prepare = exynos4x12_prepare;
	data->busfreq_post = exynos4x12_post;
	data->set_qos = exynos4x12_set_qos;
	data->busfreq_suspend = exynos4x12_suspend;
	data->busfreq_resume = exynos4x12_resume;

	return 0;
}
