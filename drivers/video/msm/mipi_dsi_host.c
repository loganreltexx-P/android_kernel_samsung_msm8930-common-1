
/* Copyright (c) 2008-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/iopoll.h>

#include <asm/system.h>
#include <asm/mach-types.h>

#include <mach/hardware.h>
#include <mach/gpio.h>
#include <mach/clk.h>
#include <mach/dma.h>

#include "msm_fb.h"
#include "mipi_dsi.h"
#include "mdp.h"
#include "mdp4.h"

static struct completion dsi_dma_comp;
static struct completion dsi_mdp_comp;
static struct completion dsi_video_comp;
static struct dsi_buf dsi_tx_buf;
static struct dsi_buf dsi_rx_buf;
static spinlock_t dsi_irq_lock;
static spinlock_t dsi_mdp_lock;
spinlock_t dsi_clk_lock;
static int dsi_ctrl_lock;
static int dsi_mdp_busy;
static struct mutex cmd_mutex;
static struct mutex clk_mutex;

static struct list_head pre_kickoff_list;
static struct list_head post_kickoff_list;
struct work_struct mdp_reset_work;

#if defined(CONFIG_RUNTIME_MIPI_CLK_CHANGE) || defined(CONFIG_FB_MSM_CAMERA_CSC)
static struct mutex dsi_vsync_mutex;
static struct completion dsi_vsync_comp;
#endif

enum {
	STAT_DSI_START,
	STAT_DSI_ERROR,
	STAT_DSI_CMD,
	STAT_DSI_MDP
};

struct dcs_cmd_list	cmdlist;

#ifdef CONFIG_FB_MSM_MDP40
void mipi_dsi_mdp_stat_inc(int which)
{
	switch (which) {
	case STAT_DSI_START:
		mdp4_stat.dsi_mdp_start++;
		break;
	case STAT_DSI_ERROR:
		mdp4_stat.intr_dsi_err++;
		break;
	case STAT_DSI_CMD:
		mdp4_stat.intr_dsi_cmd++;
		break;
	case STAT_DSI_MDP:
		mdp4_stat.intr_dsi_mdp++;
		break;
	default:
		break;
	}
}
#else
void mipi_dsi_mdp_stat_inc(int which)
{
}
#endif

static void mdp_reset_wq_handler(struct work_struct *work)
{
	mdp4_mixer_reset(0);
}

void mipi_dsi_init(void)
{
	init_completion(&dsi_dma_comp);
	init_completion(&dsi_mdp_comp);
	init_completion(&dsi_video_comp);
	mipi_dsi_buf_alloc(&dsi_tx_buf, DSI_BUF_SIZE);
	mipi_dsi_buf_alloc(&dsi_rx_buf, DSI_BUF_SIZE);
	spin_lock_init(&dsi_irq_lock);
	spin_lock_init(&dsi_mdp_lock);
	spin_lock_init(&dsi_clk_lock);
	mutex_init(&cmd_mutex);
	mutex_init(&clk_mutex);
	INIT_WORK(&mdp_reset_work, mdp_reset_wq_handler);

	INIT_LIST_HEAD(&pre_kickoff_list);
	INIT_LIST_HEAD(&post_kickoff_list);

#if defined(CONFIG_RUNTIME_MIPI_CLK_CHANGE) || defined(CONFIG_FB_MSM_CAMERA_CSC)
	mutex_init(&dsi_vsync_mutex);
	init_completion(&dsi_vsync_comp);
#endif

}


static u32 dsi_irq_mask;

void mipi_dsi_enable_irq(u32 term)
{
	unsigned long flags;

	spin_lock_irqsave(&dsi_irq_lock, flags);
	if (dsi_irq_mask & term) {
		spin_unlock_irqrestore(&dsi_irq_lock, flags);
		return;
	}
	if (dsi_irq_mask == 0) {
		enable_irq(dsi_irq);
		pr_debug("%s: IRQ Enable, mask=%x term=%x\n",
				__func__, (int)dsi_irq_mask, (int)term);
	}
	dsi_irq_mask |= term;
	spin_unlock_irqrestore(&dsi_irq_lock, flags);
}

void mipi_dsi_disable_irq(u32 term)
{
	unsigned long flags;

	spin_lock_irqsave(&dsi_irq_lock, flags);
	if (!(dsi_irq_mask & term)) {
		spin_unlock_irqrestore(&dsi_irq_lock, flags);
		return;
	}
	dsi_irq_mask &= ~term;
	if (dsi_irq_mask == 0) {
		disable_irq(dsi_irq);
		pr_debug("%s: IRQ Disable, mask=%x term=%x\n",
				__func__, (int)dsi_irq_mask, (int)term);
	}
	spin_unlock_irqrestore(&dsi_irq_lock, flags);
}

/*
 * mipi_dsi_disale_irq_nosync() should be called
 * from interrupt context
 */
void mipi_dsi_disable_irq_nosync(u32 term)
{
	spin_lock(&dsi_irq_lock);
	if (!(dsi_irq_mask & term)) {
		spin_unlock(&dsi_irq_lock);
		return;
	}
	dsi_irq_mask &= ~term;
	if (dsi_irq_mask == 0) {
		disable_irq_nosync(dsi_irq);
		pr_debug("%s: IRQ Disable, mask=%x term=%x\n",
				__func__, (int)dsi_irq_mask, (int)term);
	}
	spin_unlock(&dsi_irq_lock);
}

void mipi_dsi_clk_cfg(int on)
{
	static int dsi_clk_cnt;

	mutex_lock(&clk_mutex);
	if (on) {
		if (dsi_clk_cnt == 0) {
			mipi_dsi_prepare_ahb_clocks();
			mipi_dsi_ahb_ctrl(1);
			mipi_dsi_clk_enable();
		}
		dsi_clk_cnt++;
	} else {
		if (dsi_clk_cnt) {
			dsi_clk_cnt--;
			if (dsi_clk_cnt == 0) {
				mipi_dsi_clk_disable();
				mipi_dsi_unprepare_clocks();
				mipi_dsi_ahb_ctrl(0);
				mipi_dsi_unprepare_ahb_clocks();
			}
		}
	}
	pr_debug("%s: on=%d clk_cnt=%d pid=%d\n", __func__,
				on, dsi_clk_cnt, current->pid);
	mutex_unlock(&clk_mutex);
}

void mipi_dsi_turn_on_clks(void)
{
	mipi_dsi_prepare_ahb_clocks();
	mipi_dsi_ahb_ctrl(1);
	mipi_dsi_clk_enable();
}

void mipi_dsi_turn_off_clks(void)
{
	mipi_dsi_clk_disable();
	mipi_dsi_unprepare_clocks();
	mipi_dsi_ahb_ctrl(0);
	mipi_dsi_unprepare_ahb_clocks();
}

static void mipi_dsi_action(struct list_head *act_list)
{
	struct list_head *lp;
	struct dsi_kickoff_action *act;

	list_for_each(lp, act_list) {
		act = list_entry(lp, struct dsi_kickoff_action, act_entry);
		if (act && act->action)
			act->action(act->data);
	}
}

void mipi_dsi_pre_kickoff_action(void)
{
	mipi_dsi_action(&pre_kickoff_list);
}

void mipi_dsi_post_kickoff_action(void)
{
	mipi_dsi_action(&post_kickoff_list);
}

/*
 * mipi_dsi_pre_kickoff_add:
 * ov_mutex need to be acquired before call this function.
 */
void mipi_dsi_pre_kickoff_add(struct dsi_kickoff_action *act)
{
	if (act)
		list_add_tail(&act->act_entry, &pre_kickoff_list);
}

/*
 * mipi_dsi_pre_kickoff_add:
 * ov_mutex need to be acquired before call this function.
 */
void mipi_dsi_post_kickoff_add(struct dsi_kickoff_action *act)
{
	if (act)
		list_add_tail(&act->act_entry, &post_kickoff_list);
}

/*
 * mipi_dsi_pre_kickoff_add:
 * ov_mutex need to be acquired before call this function.
 */
void mipi_dsi_pre_kickoff_del(struct dsi_kickoff_action *act)
{
	if (!list_empty(&pre_kickoff_list) && act)
		list_del(&act->act_entry);
}

/*
 * mipi_dsi_pre_kickoff_add:
 * ov_mutex need to be acquired before call this function.
 */
void mipi_dsi_post_kickoff_del(struct dsi_kickoff_action *act)
{
	if (!list_empty(&post_kickoff_list) && act)
		list_del(&act->act_entry);
}

/*
 * mipi dsi buf mechanism
 */
char *mipi_dsi_buf_reserve(struct dsi_buf *dp, int len)
{
	dp->data += len;
	return dp->data;
}

char *mipi_dsi_buf_unreserve(struct dsi_buf *dp, int len)
{
	dp->data -= len;
	return dp->data;
}

char *mipi_dsi_buf_push(struct dsi_buf *dp, int len)
{
	dp->data -= len;
	dp->len += len;
	return dp->data;
}

char *mipi_dsi_buf_reserve_hdr(struct dsi_buf *dp, int hlen)
{
	dp->hdr = (uint32 *)dp->data;
	return mipi_dsi_buf_reserve(dp, hlen);
}

char *mipi_dsi_buf_init(struct dsi_buf *dp)
{
	int off;

	dp->data = dp->start;
	off = (int)dp->data;
	/* 8 byte align */
	off &= 0x07;
	if (off)
		off = 8 - off;
	dp->data += off;
	dp->len = 0;
	return dp->data;
}

int mipi_dsi_buf_alloc(struct dsi_buf *dp, int size)
{

	dp->start = kmalloc(size, GFP_KERNEL);
	if (dp->start == NULL) {
		pr_err("%s:%u\n", __func__, __LINE__);
		return -ENOMEM;
	}

	dp->end = dp->start + size;
	dp->size = size;

	if ((int)dp->start & 0x07)
		pr_err("%s: buf NOT 8 bytes aligned\n", __func__);

	dp->data = dp->start;
	dp->len = 0;
	return size;
}

/*
 * mipi dsi gerneric long write
 */
static int mipi_dsi_generic_lwrite(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	char *bp;
	uint32 *hp;
	int i, len;

	bp = mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);

	/* fill up payload */
	if (cm->payload) {
		len = cm->dlen;
		len += 3;
		len &= ~0x03;	/* multipled by 4 */
		for (i = 0; i < cm->dlen; i++)
			*bp++ = cm->payload[i];

		/* append 0xff to the end */
		for (; i < len; i++)
			*bp++ = 0xff;

		dp->len += len;
	}

	/* fill up header */
	hp = dp->hdr;
	*hp = 0;
	*hp = DSI_HDR_WC(cm->dlen);
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_LONG_PKT;
	*hp |= DSI_HDR_DTYPE(DTYPE_GEN_LWRITE);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;
}

/*
 * mipi dsi gerneric short write with 0, 1 2 parameters
 */
static int mipi_dsi_generic_swrite(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;
	int len;

	if (cm->dlen && cm->payload == 0) {
		pr_err("%s: NO payload error\n", __func__);
		return 0;
	}

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	if (cm->last)
		*hp |= DSI_HDR_LAST;


	len = (cm->dlen > 2) ? 2 : cm->dlen;

	if (len == 1) {
		*hp |= DSI_HDR_DTYPE(DTYPE_GEN_WRITE1);
		*hp |= DSI_HDR_DATA1(cm->payload[0]);
		*hp |= DSI_HDR_DATA2(0);
	} else if (len == 2) {
		*hp |= DSI_HDR_DTYPE(DTYPE_GEN_WRITE2);
		*hp |= DSI_HDR_DATA1(cm->payload[0]);
		*hp |= DSI_HDR_DATA2(cm->payload[1]);
	} else {
		*hp |= DSI_HDR_DTYPE(DTYPE_GEN_WRITE);
		*hp |= DSI_HDR_DATA1(0);
		*hp |= DSI_HDR_DATA2(0);
	}

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

/*
 * mipi dsi gerneric read with 0, 1 2 parameters
 */
static int mipi_dsi_generic_read(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;
	int len;

	if (cm->dlen && cm->payload == 0) {
		pr_err("%s: NO payload error\n", __func__);
		return 0;
	}

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_BTA;
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	len = (cm->dlen > 2) ? 2 : cm->dlen;

	if (len == 1) {
		*hp |= DSI_HDR_DTYPE(DTYPE_GEN_READ1);
		*hp |= DSI_HDR_DATA1(cm->payload[0]);
		*hp |= DSI_HDR_DATA2(0);
	} else if (len == 2) {
		*hp |= DSI_HDR_DTYPE(DTYPE_GEN_READ2);
		*hp |= DSI_HDR_DATA1(cm->payload[0]);
		*hp |= DSI_HDR_DATA2(cm->payload[1]);
	} else {
		*hp |= DSI_HDR_DTYPE(DTYPE_GEN_READ);
		*hp |= DSI_HDR_DATA1(0);
		*hp |= DSI_HDR_DATA2(0);
	}

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);
	return dp->len;	/* 4 bytes */
}

/*
 * mipi dsi dcs long write
 */
static int mipi_dsi_dcs_lwrite(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	char *bp;
	uint32 *hp;
	int i, len;

	bp = mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);

	/*
	 * fill up payload
	 * dcs command byte (first byte) followed by payload
	 */
	if (cm->payload) {
		len = cm->dlen;
		len += 3;
		len &= ~0x03;	/* multipled by 4 */
		for (i = 0; i < cm->dlen; i++)
			*bp++ = cm->payload[i];

		/* append 0xff to the end */
		for (; i < len; i++)
			*bp++ = 0xff;

		dp->len += len;
	}

	/* fill up header */
	hp = dp->hdr;
	*hp = 0;
	*hp = DSI_HDR_WC(cm->dlen);
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_LONG_PKT;
	*hp |= DSI_HDR_DTYPE(DTYPE_DCS_LWRITE);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;
}

/*
 * mipi dsi dcs short write with 0 parameters
 */
static int mipi_dsi_dcs_swrite(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;
	int len;

	if (cm->payload == 0) {
		pr_err("%s: NO payload error\n", __func__);
		return -EINVAL;
	}

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	if (cm->ack)		/* ask ACK trigger msg from peripeheral */
		*hp |= DSI_HDR_BTA;
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	len = (cm->dlen > 1) ? 1 : cm->dlen;

	*hp |= DSI_HDR_DTYPE(DTYPE_DCS_WRITE);
	*hp |= DSI_HDR_DATA1(cm->payload[0]);	/* dcs command byte */
	*hp |= DSI_HDR_DATA2(0);

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);
	return dp->len;
}

/*
 * mipi dsi dcs short write with 1 parameters
 */
static int mipi_dsi_dcs_swrite1(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	if (cm->dlen < 2 || cm->payload == 0) {
		pr_err("%s: NO payload error\n", __func__);
		return -EINVAL;
	}

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	if (cm->ack)		/* ask ACK trigger msg from peripeheral */
		*hp |= DSI_HDR_BTA;
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	*hp |= DSI_HDR_DTYPE(DTYPE_DCS_WRITE1);
	*hp |= DSI_HDR_DATA1(cm->payload[0]);	/* dcs comamnd byte */
	*hp |= DSI_HDR_DATA2(cm->payload[1]);	/* parameter */

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;
}
/*
 * mipi dsi dcs read with 0 parameters
 */

static int mipi_dsi_dcs_read(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	if (cm->payload == 0) {
		pr_err("%s: NO payload error\n", __func__);
		return -EINVAL;
	}

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_BTA;
	*hp |= DSI_HDR_DTYPE(DTYPE_DCS_READ);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	*hp |= DSI_HDR_DATA1(cm->payload[0]);	/* dcs command byte */
	*hp |= DSI_HDR_DATA2(0);

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

static int mipi_dsi_cm_on(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_DTYPE(DTYPE_CM_ON);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

static int mipi_dsi_cm_off(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_DTYPE(DTYPE_CM_OFF);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

static int mipi_dsi_peripheral_on(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_DTYPE(DTYPE_PERIPHERAL_ON);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

static int mipi_dsi_peripheral_off(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_DTYPE(DTYPE_PERIPHERAL_OFF);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

static int mipi_dsi_set_max_pktsize(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	if (cm->payload == 0) {
		pr_err("%s: NO payload error\n", __func__);
		return 0;
	}

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_DTYPE(DTYPE_MAX_PKTSIZE);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	*hp |= DSI_HDR_DATA1(cm->payload[0]);
	*hp |= DSI_HDR_DATA2(cm->payload[1]);

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

static int mipi_dsi_null_pkt(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp = DSI_HDR_WC(cm->dlen);
	*hp |= DSI_HDR_LONG_PKT;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_DTYPE(DTYPE_NULL_PKT);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

static int mipi_dsi_blank_pkt(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	uint32 *hp;

	mipi_dsi_buf_reserve_hdr(dp, DSI_HOST_HDR_SIZE);
	hp = dp->hdr;
	*hp = 0;
	*hp = DSI_HDR_WC(cm->dlen);
	*hp |= DSI_HDR_LONG_PKT;
	*hp |= DSI_HDR_VC(cm->vc);
	*hp |= DSI_HDR_DTYPE(DTYPE_BLANK_PKT);
	if (cm->last)
		*hp |= DSI_HDR_LAST;

	mipi_dsi_buf_push(dp, DSI_HOST_HDR_SIZE);

	return dp->len;	/* 4 bytes */
}

/*
 * prepare cmd buffer to be txed
 */
int mipi_dsi_cmd_dma_add(struct dsi_buf *dp, struct dsi_cmd_desc *cm)
{
	int len = 0;

	switch (cm->dtype) {
	case DTYPE_GEN_WRITE:
	case DTYPE_GEN_WRITE1:
	case DTYPE_GEN_WRITE2:
		len = mipi_dsi_generic_swrite(dp, cm);
		break;
	case DTYPE_GEN_LWRITE:
		len = mipi_dsi_generic_lwrite(dp, cm);
		break;
	case DTYPE_GEN_READ:
	case DTYPE_GEN_READ1:
	case DTYPE_GEN_READ2:
		len = mipi_dsi_generic_read(dp, cm);
		break;
	case DTYPE_DCS_LWRITE:
		len = mipi_dsi_dcs_lwrite(dp, cm);
		break;
	case DTYPE_DCS_WRITE:
		len = mipi_dsi_dcs_swrite(dp, cm);
		break;
	case DTYPE_DCS_WRITE1:
		len = mipi_dsi_dcs_swrite1(dp, cm);
		break;
	case DTYPE_DCS_READ:
		len = mipi_dsi_dcs_read(dp, cm);
		break;
	case DTYPE_MAX_PKTSIZE:
		len = mipi_dsi_set_max_pktsize(dp, cm);
		break;
	case DTYPE_NULL_PKT:
		len = mipi_dsi_null_pkt(dp, cm);
		break;
	case DTYPE_BLANK_PKT:
		len = mipi_dsi_blank_pkt(dp, cm);
		break;
	case DTYPE_CM_ON:
		len = mipi_dsi_cm_on(dp, cm);
		break;
	case DTYPE_CM_OFF:
		len = mipi_dsi_cm_off(dp, cm);
		break;
	case DTYPE_PERIPHERAL_ON:
		len = mipi_dsi_peripheral_on(dp, cm);
		break;
	case DTYPE_PERIPHERAL_OFF:
		len = mipi_dsi_peripheral_off(dp, cm);
		break;
	default:
		pr_debug("%s: dtype=%x NOT supported\n",
					__func__, cm->dtype);
		break;

	}

	return len;
}

/*
 * mipi_dsi_short_read1_resp: 1 parameter
 */
static int mipi_dsi_short_read1_resp(struct dsi_buf *rp)
{
	/* strip out dcs type */
	rp->data++;
	rp->len = 1;
	return rp->len;
}

/*
 * mipi_dsi_short_read2_resp: 2 parameter
 */
static int mipi_dsi_short_read2_resp(struct dsi_buf *rp)
{
	/* strip out dcs type */
	rp->data++;
	rp->len = 2;
	return rp->len;
}

static int mipi_dsi_long_read_resp(struct dsi_buf *rp)
{
	short len;

	len = rp->data[2];
	len <<= 8;
	len |= rp->data[1];
	/* strip out dcs header */
	rp->data += 4;
	rp->len -= 4;
	/* strip out 2 bytes of checksum */
	rp->len -= 2;
	return len;
}

void mipi_dsi_host_init(struct mipi_panel_info *pinfo)
{
	uint32 dsi_ctrl, intr_ctrl;
	uint32 data;

	if (mdp_rev > MDP_REV_41 || mdp_rev == MDP_REV_303)
		pinfo->rgb_swap = DSI_RGB_SWAP_RGB;
	else
		pinfo->rgb_swap = DSI_RGB_SWAP_BGR;

	if (pinfo->mode == DSI_VIDEO_MODE) {
		data = 0;
		if (pinfo->pulse_mode_hsa_he)
			data |= BIT(28);
		if (pinfo->hfp_power_stop)
			data |= BIT(24);
		if (pinfo->hbp_power_stop)
			data |= BIT(20);
		if (pinfo->hsa_power_stop)
			data |= BIT(16);
		if (pinfo->eof_bllp_power_stop)
			data |= BIT(15);
		if (pinfo->bllp_power_stop)
			data |= BIT(12);
		data |= ((pinfo->traffic_mode & 0x03) << 8);
		data |= ((pinfo->dst_format & 0x03) << 4); /* 2 bits */
		data |= (pinfo->vc & 0x03);
		MIPI_OUTP(MIPI_DSI_BASE + 0x000c, data);

		data = 0;
		data |= ((pinfo->rgb_swap & 0x07) << 12);
		if (pinfo->b_sel)
			data |= BIT(8);
		if (pinfo->g_sel)
			data |= BIT(4);
		if (pinfo->r_sel)
			data |= BIT(0);
		MIPI_OUTP(MIPI_DSI_BASE + 0x001c, data);
	} else if (pinfo->mode == DSI_CMD_MODE) {
		data = 0;
		data |= ((pinfo->interleave_max & 0x0f) << 20);
		data |= ((pinfo->rgb_swap & 0x07) << 16);
		if (pinfo->b_sel)
			data |= BIT(12);
		if (pinfo->g_sel)
			data |= BIT(8);
		if (pinfo->r_sel)
			data |= BIT(4);
		data |= (pinfo->dst_format & 0x0f);	/* 4 bits */
		MIPI_OUTP(MIPI_DSI_BASE + 0x003c, data);

		/* DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL */
		data = pinfo->wr_mem_continue & 0x0ff;
		data <<= 8;
		data |= (pinfo->wr_mem_start & 0x0ff);
		if (pinfo->insert_dcs_cmd)
			data |= BIT(16);
		MIPI_OUTP(MIPI_DSI_BASE + 0x0040, data);
	} else
		pr_err("%s: Unknown DSI mode=%d\n", __func__, pinfo->mode);

	dsi_ctrl = BIT(8) | BIT(2);	/* clock enable & cmd mode */
	intr_ctrl = 0;
	intr_ctrl = (DSI_INTR_CMD_DMA_DONE_MASK | DSI_INTR_CMD_MDP_DONE_MASK);

	if (pinfo->crc_check)
		dsi_ctrl |= BIT(24);
	if (pinfo->ecc_check)
		dsi_ctrl |= BIT(20);
	if (pinfo->data_lane3)
		dsi_ctrl |= BIT(7);
	if (pinfo->data_lane2)
		dsi_ctrl |= BIT(6);
	if (pinfo->data_lane1)
		dsi_ctrl |= BIT(5);
	if (pinfo->data_lane0)
		dsi_ctrl |= BIT(4);

#if defined(CONFIG_MIPI_DSI_LP_CMD_SEND)
		/* from frame buffer, low power mode */
		/* DSI_COMMAND_MODE_DMA_CTRL */
		MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x14000000);
#else
		/* send commands in High Speed Mode */
		MIPI_OUTP(MIPI_DSI_BASE + 0x38, 0x10000000);
#endif

	data = 0;
	if (pinfo->te_sel)
		data |= BIT(31);
	data |= pinfo->mdp_trigger << 4;/* cmd mdp trigger */
	data |= pinfo->dma_trigger;	/* cmd dma trigger */
	data |= (pinfo->stream & 0x01) << 8;
	MIPI_OUTP(MIPI_DSI_BASE + 0x0080, data); /* DSI_TRIG_CTRL */

	/* DSI_LAN_SWAP_CTRL */
	MIPI_OUTP(MIPI_DSI_BASE + 0x00ac, pinfo->dlane_swap);

	/* clock out ctrl */
	data = pinfo->t_clk_post & 0x3f;	/* 6 bits */
	data <<= 8;
	data |= pinfo->t_clk_pre & 0x3f;	/*  6 bits */
	MIPI_OUTP(MIPI_DSI_BASE + 0xc0, data);	/* DSI_CLKOUT_TIMING_CTRL */

	data = 0;
	if (pinfo->rx_eot_ignore)
		data |= BIT(4);
	if (pinfo->tx_eot_append)
		data |= BIT(0);
	MIPI_OUTP(MIPI_DSI_BASE + 0x00c8, data); /* DSI_EOT_PACKET_CTRL */


	/* allow only ack-err-status + fifo underrun to generate interrupt */
	MIPI_OUTP(MIPI_DSI_BASE + 0x0108, 0x13ff37e0); /* DSI_ERR_INT_MASK0 */

	intr_ctrl |= DSI_INTR_ERROR_MASK;
	MIPI_OUTP(MIPI_DSI_BASE + 0x010c, intr_ctrl); /* DSI_INTL_CTRL */

	/* turn esc, byte, dsi, pclk, sclk, hclk on */
	if (mdp_rev >= MDP_REV_41)
		MIPI_OUTP(MIPI_DSI_BASE + 0x118, 0x23f); /* DSI_CLK_CTRL */
	else
		MIPI_OUTP(MIPI_DSI_BASE + 0x118, 0x33f); /* DSI_CLK_CTRL */

	dsi_ctrl |= BIT(0);	/* enable dsi */
	MIPI_OUTP(MIPI_DSI_BASE + 0x0000, dsi_ctrl);

	wmb();
}

void mipi_set_tx_power_mode(int mode)
{
	uint32 data = MIPI_INP(MIPI_DSI_BASE + 0x38);

	if (mode == 0)
		data &= ~BIT(26);
	else
		data |= BIT(26);

	MIPI_OUTP(MIPI_DSI_BASE + 0x38, data);
}

void mipi_dsi_sw_reset(void)
{
	MIPI_OUTP(MIPI_DSI_BASE + 0x114, 0x01);
	wmb();
	MIPI_OUTP(MIPI_DSI_BASE + 0x114, 0x00);
	wmb();
}

void mipi_dsi_controller_cfg(int enable)
{

	uint32 dsi_ctrl;
	uint32 status;
	u32 sleep_us = 1000;
	u32 timeout_us = 16000;

	/* Check for CMD_MODE_DMA_BUSY */
	if (readl_poll_timeout((MIPI_DSI_BASE + 0x0004),
			   status,
			   ((status & 0x02) == 0),
			       sleep_us, timeout_us))
		pr_info("%s: DSI status=%x failed\n", __func__, status);

	/* Check for x_HS_FIFO_EMPTY */
	if (readl_poll_timeout((MIPI_DSI_BASE + 0x0008),
			   status,
			   ((status & 0x11111000) == 0x11111000),
			       sleep_us, timeout_us))
		pr_info("%s: FIFO status=%x failed\n", __func__, status);

	/* Check for VIDEO_MODE_ENGINE_BUSY */
	if (readl_poll_timeout((MIPI_DSI_BASE + 0x0004),
			   status,
			   ((status & 0x08) == 0),
			       sleep_us, timeout_us))
		pr_info("%s: DSI status=%x failed\n", __func__, status);

	dsi_ctrl = MIPI_INP(MIPI_DSI_BASE + 0x0000);
	if (enable)
		dsi_ctrl |= 0x01;
	else
		dsi_ctrl &= ~0x01;

	MIPI_OUTP(MIPI_DSI_BASE + 0x0000, dsi_ctrl);
	wmb();
}

void mipi_dsi_op_mode_config(int mode)
{

	uint32 dsi_ctrl, intr_ctrl;

	dsi_ctrl = MIPI_INP(MIPI_DSI_BASE + 0x0000);
	dsi_ctrl &= ~0x07;
	if (mode == DSI_VIDEO_MODE) {
		dsi_ctrl |= 0x03;
		intr_ctrl = DSI_INTR_CMD_DMA_DONE_MASK;
	} else {		/* command mode */
		dsi_ctrl |= 0x05;
		intr_ctrl = DSI_INTR_CMD_DMA_DONE_MASK | DSI_INTR_ERROR_MASK |
				DSI_INTR_CMD_MDP_DONE_MASK;
	}

	pr_debug("%s: dsi_ctrl=%x intr=%x\n", __func__, dsi_ctrl, intr_ctrl);

	MIPI_OUTP(MIPI_DSI_BASE + 0x010c, intr_ctrl); /* DSI_INTL_CTRL */
	MIPI_OUTP(MIPI_DSI_BASE + 0x0000, dsi_ctrl);
	wmb();
}


void mipi_dsi_wait4video_done(void)
{
	unsigned long flag;

	spin_lock_irqsave(&dsi_mdp_lock, flag);
	INIT_COMPLETION(dsi_video_comp);
	mipi_dsi_enable_irq(DSI_VIDEO_TERM);
	mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, DSI_INTR_VIDEO_DONE_MASK);
	spin_unlock_irqrestore(&dsi_mdp_lock, flag);

	wait_for_completion_timeout(&dsi_video_comp,
					msecs_to_jiffies(VSYNC_PERIOD * 4));
}

void mipi_dsi_mdp_busy_wait(void)
{
	mutex_lock(&cmd_mutex);
	mipi_dsi_cmd_mdp_busy();
	mutex_unlock(&cmd_mutex);
}

void mipi_dsi_cmd_mdp_start(void)
{
	unsigned long flag;

	mipi_dsi_mdp_stat_inc(STAT_DSI_START);

	spin_lock_irqsave(&dsi_mdp_lock, flag);
	mipi_dsi_enable_irq(DSI_MDP_TERM);
	dsi_mdp_busy = TRUE;
	INIT_COMPLETION(dsi_mdp_comp);
	spin_unlock_irqrestore(&dsi_mdp_lock, flag);
}

void mipi_dsi_cmd_bta_sw_trigger(void)
{
	uint32 data;
	int cnt = 0;

	MIPI_OUTP(MIPI_DSI_BASE + 0x094, 0x01);	/* trigger */
	wmb();

	while (cnt < 10000) {
		data = MIPI_INP(MIPI_DSI_BASE + 0x0004);/* DSI_STATUS */
		if ((data & 0x0010) == 0)
			break;
		cnt++;
	}

	mipi_dsi_ack_err_status();

	pr_debug("%s: BTA done, cnt=%d\n", __func__, cnt);
}

static char set_tear_on[2] = {0x35, 0x00};
static struct dsi_cmd_desc dsi_tear_on_cmd = {
	DTYPE_DCS_WRITE1, 1, 0, 0, 0, sizeof(set_tear_on), set_tear_on};

static char set_tear_off[2] = {0x34, 0x00};
static struct dsi_cmd_desc dsi_tear_off_cmd = {
	DTYPE_DCS_WRITE, 1, 0, 0, 0, sizeof(set_tear_off), set_tear_off};

void mipi_dsi_set_tear_on(struct msm_fb_data_type *mfd)
{
	struct dcs_cmd_req cmdreq;

	cmdreq.cmds = &dsi_tear_on_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_COMMIT;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mipi_dsi_cmdlist_put(&cmdreq);
}

void mipi_dsi_set_tear_off(struct msm_fb_data_type *mfd)
{
	struct dcs_cmd_req cmdreq;

	cmdreq.cmds = &dsi_tear_off_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_COMMIT;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mipi_dsi_cmdlist_put(&cmdreq);
}

int mipi_dsi_cmd_reg_tx(uint32 data)
{
#ifdef DSI_HOST_DEBUG
	int i, len;
	char *bp;
	char buffer[256];

	bp = (char *)&data;
	buffer[0] = 0; // make length 0
	snprintf( buffer +strlen(buffer), sizeof(buffer) -strlen(buffer) -1, "%s: ", __func__ );
	switch( bp[2])
	{
		case 0x05: strcat( buffer, "D_WR "); len = 4; break;
		case 0x15: strcat( buffer, "D_W1 "); len = 4; break;
		case 0x06: strcat( buffer, "D_RD "); len = 4; break;
		case 0x39: strcat( buffer, "D_LW "); len = 4 +(*bp); break;
		case 0x03: strcat( buffer, "G_WR "); len = 4; break;
		case 0x13: strcat( buffer, "G_W1 "); len = 4; break;
		case 0x23: strcat( buffer, "G_W2 "); len = 4; break;
		case 0x29: strcat( buffer, "G_LW "); len = 4 +(*bp); break;
		case 0x04: strcat( buffer, "G_RD "); len = 4; break;
		case 0x14: strcat( buffer, "G_R1 "); len = 4; break;
		case 0x24: strcat( buffer, "G_R2 "); len = 4; break;
		case 0x37: strcat( buffer, "PKTL "); len = 4; break;
		default: strcat( buffer, "???? "); len = 4; break;

	}
	for (i=0; i < len; i++) { snprintf( buffer +strlen(buffer), sizeof(buffer) -strlen(buffer) -1, "%2x ", *bp++ ); }
	pr_info( "%s", buffer);
#endif

	MIPI_OUTP(MIPI_DSI_BASE + 0x0080, 0x04);/* sw trigger */
	MIPI_OUTP(MIPI_DSI_BASE + 0x0, 0x135);

	wmb();

	MIPI_OUTP(MIPI_DSI_BASE + 0x038, data);
	wmb();
	MIPI_OUTP(MIPI_DSI_BASE + 0x08c, 0x01);	/* trigger */
	wmb();

	udelay(300);

	return 4;
}


#ifdef CONFIG_MIPI_DSI_LP_RX
/*embedding max pkt size cmd and NULL pkt in one burst*/
static char null_pkt1[8] = {0x00, 0x00, 0x09, 0x40, 0x01, 0x00, 0x37, 0x80};

int mipi_dsi_cmds_rx_lp(struct msm_fb_data_type *mfd,
			struct dsi_buf *tp, struct dsi_buf *rp,
			char *cmds, int rlen)
{
	int cnt, len, diff, video_mode;
	unsigned long flag;
	char cmd;

	uint32 dsi_ctrl, ctrl;

	if (mfd->panel_info.mipi.no_max_pkt_size) {
		/* Only support rlen = 4*n */
		rlen += 3;
		rlen &= ~0x03;
	}

	len = rlen;
	diff = 0;

	if (len <= 2)
		cnt = 4;	/* short read */
	else {
		if (len > MIPI_DSI_LEN)
			len = MIPI_DSI_LEN;	/* 8 bytes at most */

		len = (len + 3) & ~0x03; /* len 4 bytes align */
		diff = len - rlen;
		/*
		 * add extra 2 bytes to len to have overall
		 * packet size is multipe by 4. This also make
		 * sure 4 bytes dcs headerlocates within a
		 * 32 bits register after shift in.
		 * after all, len should be either 6 or 10.
		 */
		len += 2;
		cnt = len + 6; /* 4 bytes header + 2 bytes crc */
	}


	/* turn on cmd mode
	* for video mode, do not send cmds more than
	* one pixel line, since it only transmit it
	* during BLLP.
	*/
	dsi_ctrl = MIPI_INP(MIPI_DSI_BASE + 0x0000);
	video_mode = dsi_ctrl & 0x02; /* VIDEO_MODE_EN */
	if (video_mode) {
		ctrl = dsi_ctrl | 0x04; /* CMD_MODE_EN */
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, ctrl);
	}

	spin_lock_irqsave(&dsi_mdp_lock, flag);
	mipi_dsi_enable_irq(DSI_CMD_TERM);
	dsi_mdp_busy = TRUE;
	spin_unlock_irqrestore(&dsi_mdp_lock, flag);

	if (!mfd->panel_info.mipi.no_max_pkt_size) {
		mipi_dsi_buf_init(tp);
		tp->len = 8;
		memcpy(tp->start, null_pkt1, tp->len);
		tp->data = tp->start;
		/* transmit read comamnd to client */
		if (video_mode)
			mdp4_dsi_video_wait4dmap_for_dsi(0);

		mipi_dsi_cmd_dma_tx(tp);
	}

	/*
	 * once cmd_dma_done interrupt received,
	 * return data from client is ready and stored
	 * at RDBK_DATA register already
	 */
	mipi_dsi_enable_irq(DSI_CMD_TERM);
	mipi_dsi_buf_init(tp);
	tp->len = 8;
	memcpy(tp->start, cmds, tp->len);
	tp->data = tp->start;
	/* transmit read comamnd to client */
	if (video_mode)
		mdp4_dsi_video_wait4dmap_for_dsi(0);

	mipi_dsi_cmd_dma_tx(tp);
	mipi_dsi_disable_irq(DSI_CMD_TERM);

	/*
	 * expect rlen = n * 4
	 * short alignement for start addr
	 */
	if (mfd->panel_info.mipi.no_max_pkt_size)
		rp->data += 2;

	mipi_dsi_cmd_dma_rx(rp, cnt);


	/*restore*/
	if (video_mode)
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, dsi_ctrl);

	spin_lock_irqsave(&dsi_mdp_lock, flag);
	dsi_mdp_busy = FALSE;
	mipi_dsi_disable_irq(DSI_CMD_TERM);
	complete(&dsi_mdp_comp);
	spin_unlock_irqrestore(&dsi_mdp_lock, flag);

	/*
	 * remove extra 2 bytes from previous
	 * rx transaction at shift register
	 * which was inserted during copy
	 * shift registers to rx buffer
	 * rx payload start from long alignment addr
	 */
	if (mfd->panel_info.mipi.no_max_pkt_size)
		rp->data += 2;

	cmd = rp->data[0];
	switch (cmd) {
	case DTYPE_ACK_ERR_RESP:
		pr_debug("%s: rx ACK_ERR_PACLAGE\n", __func__);
		break;
	case DTYPE_GEN_READ1_RESP:
	case DTYPE_DCS_READ1_RESP:
		pr_debug("%s: rx DTYPE_DCS_READ1_RESP\n", __func__);
		mipi_dsi_short_read1_resp(rp);
		break;
	case DTYPE_GEN_READ2_RESP:
	case DTYPE_DCS_READ2_RESP:
		pr_debug("%s: rx DTYPE_DCS_READ2_RESP\n", __func__);

		mipi_dsi_short_read2_resp(rp);
		break;
	case DTYPE_GEN_LREAD_RESP:
	case DTYPE_DCS_LREAD_RESP:
		pr_debug("%s: rx DTYPE_DCS_LREAD_RESP\n", __func__);

		mipi_dsi_long_read_resp(rp);
		rp->len -= 2; /* extra 2 bytes added */
		rp->len -= diff; /* align bytes */
		break;
	default:
		pr_err("%s: rx default !!! : %x\n", __func__, cmd);
		break;
	}

	return rp->len;
}
#endif

/*
 * mipi_dsi_cmds_tx:
 * thread context only
 */
int mipi_dsi_cmds_tx(struct dsi_buf *tp, struct dsi_cmd_desc *cmds, int cnt)
{
	struct dsi_cmd_desc *cm;
	uint32 dsi_ctrl, ctrl;
	int i, video_mode;

	/* turn on cmd mode
	* for video mode, do not send cmds more than
	* one pixel line, since it only transmit it
	* during BLLP.
	*/
	dsi_ctrl = MIPI_INP(MIPI_DSI_BASE + 0x0000);
	video_mode = dsi_ctrl & 0x02; /* VIDEO_MODE_EN */
	if (video_mode) {
		ctrl = dsi_ctrl | 0x04; /* CMD_MODE_EN */
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, ctrl);
#if defined(CONFIG_MIPI_DSI_LP_CMD_SEND)
		mdp4_dsi_video_wait4dmap_for_dsi(0);
#endif
	}

	cm = cmds;
	mipi_dsi_buf_init(tp);
	for (i = 0; i < cnt; i++) {
		mipi_dsi_enable_irq(DSI_CMD_TERM);
		mipi_dsi_buf_init(tp);
		mipi_dsi_cmd_dma_add(tp, cm);
		mipi_dsi_cmd_dma_tx(tp);
		if (cm->wait)
			msleep(cm->wait);
		cm++;
	}

	if (video_mode)
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, dsi_ctrl); /* restore */

	return cnt;
}

/*
 * mipi_dsi_cmds_single_tx:
 * thread context only
 */
int mipi_dsi_cmds_single_tx(struct dsi_buf *tp, struct dsi_cmd_desc *cmds,
								int cnt)
{
	struct dsi_cmd_desc *cm;
	uint32 dsi_ctrl, ctrl;
	int i, j = 0, k = 0, cmd_len = 0, video_mode;
	char *cmds_tx;
	char *bp;

	if (tp == NULL || cmds == NULL) {
		pr_err("%s: Null commands", __func__);
		return -EINVAL;
	}

	/* turn on cmd mode
	* for video mode, do not send cmds more than
	* one pixel line, since it only transmit it
	* during BLLP.
	*/
	dsi_ctrl = MIPI_INP(MIPI_DSI_BASE + 0x0000);
	video_mode = dsi_ctrl & 0x02; /* VIDEO_MODE_EN */
	if (video_mode) {
		ctrl = dsi_ctrl | 0x04; /* CMD_MODE_EN */
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, ctrl);
	}

	cm = cmds;
	cmds_tx = kmalloc((DSI_BUF_SIZE + DSI_HOST_HDR_SIZE) * cnt, GFP_KERNEL);
	mipi_dsi_buf_init(tp);
	mipi_dsi_enable_irq(DSI_CMD_TERM);
	for (i = 0; i < cnt; i++) {
		mipi_dsi_buf_init(tp);
		mipi_dsi_cmd_dma_add(tp, cm);
		bp = tp->data;
		for (j = 0; j < tp->len; j++) {
			*(cmds_tx + k) = *bp++;
			k++;
		}
		cmd_len = cmd_len + tp->len;
		cm++;
	}
	tp->data = cmds_tx;
	tp->len = cmd_len;
	mipi_dsi_cmd_dma_tx(tp);
	kfree(cmds_tx);

	if (video_mode)
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, dsi_ctrl); /* restore */

	return cnt;
}

/* MIPI_DSI_MRPS, Maximum Return Packet Size */
static char max_pktsize[2] = {0x00, 0x00}; /* LSB tx first, 10 bytes */

static struct dsi_cmd_desc pkt_size_cmd[] = {
	{DTYPE_MAX_PKTSIZE, 1, 0, 0, 0,
		sizeof(max_pktsize), max_pktsize}
};

/*
 * DSI panel reply with  MAX_RETURN_PACKET_SIZE bytes of data
 * plus DCS header, ECC and CRC for DCS long read response
 * mipi_dsi_controller only have 4x32 bits register ( 16 bytes) to
 * hold data per transaction.
 * MIPI_DSI_LEN equal to 8
 * len should be either 4 or 8
 * any return data more than MIPI_DSI_LEN need to be break down
 * to multiple transactions.
 *
 * ov_mutex need to be acquired before call this function.
 */
int mipi_dsi_cmds_rx(struct msm_fb_data_type *mfd,
			struct dsi_buf *tp, struct dsi_buf *rp,
			struct dsi_cmd_desc *cmds, int rlen)
{
	int cnt, len, diff, pkt_size;
	char cmd;

	if (mfd->panel_info.mipi.no_max_pkt_size) {
		/* Only support rlen = 4*n */
		rlen += 3;
		rlen &= ~0x03;
	}

	len = rlen;
	diff = 0;

	if (len <= 2)
		cnt = 4;	/* short read */
	else {
		if (len > MIPI_DSI_LEN)
			len = MIPI_DSI_LEN;	/* 8 bytes at most */

		len = (len + 3) & ~0x03; /* len 4 bytes align */
		diff = len - rlen;
		/*
		 * add extra 2 bytes to len to have overall
		 * packet size is multipe by 4. This also make
		 * sure 4 bytes dcs headerlocates within a
		 * 32 bits register after shift in.
		 * after all, len should be either 6 or 10.
		 */
		len += 2;
		cnt = len + 6; /* 4 bytes header + 2 bytes crc */
	}

	if (mfd->panel_info.type == MIPI_CMD_PANEL) {
		/* make sure mdp dma is not txing pixel data */
#ifdef CONFIG_FB_MSM_MDP303
			mdp3_dsi_cmd_dma_busy_wait(mfd);
#endif
	}

	if (!mfd->panel_info.mipi.no_max_pkt_size) {
		/* packet size need to be set at every read */
		pkt_size = len;
		max_pktsize[0] = pkt_size;
		mipi_dsi_enable_irq(DSI_CMD_TERM);
		mipi_dsi_buf_init(tp);
		mipi_dsi_cmd_dma_add(tp, pkt_size_cmd);
		mipi_dsi_cmd_dma_tx(tp);
	}

	mipi_dsi_enable_irq(DSI_CMD_TERM);
	mipi_dsi_buf_init(tp);
	mipi_dsi_cmd_dma_add(tp, cmds);

	/* transmit read comamnd to client */
	mipi_dsi_cmd_dma_tx(tp);

	/*
	 * once cmd_dma_done interrupt received,
	 * return data from client is ready and stored
	 * at RDBK_DATA register already
	 */
	mipi_dsi_buf_init(rp);
	if (mfd->panel_info.mipi.no_max_pkt_size) {
		/*
		 * expect rlen = n * 4
		 * short alignement for start addr
		 */
		rp->data += 2;
	}

	mipi_dsi_cmd_dma_rx(rp, cnt);

	if (mfd->panel_info.mipi.no_max_pkt_size) {
		/*
		 * remove extra 2 bytes from previous
		 * rx transaction at shift register
		 * which was inserted during copy
		 * shift registers to rx buffer
		 * rx payload start from long alignment addr
		 */
		rp->data += 2;
	}

	cmd = rp->data[0];
	switch (cmd) {
	case DTYPE_ACK_ERR_RESP:
		pr_debug("%s: rx ACK_ERR_PACLAGE\n", __func__);
		break;
	case DTYPE_GEN_READ1_RESP:
	case DTYPE_DCS_READ1_RESP:
		mipi_dsi_short_read1_resp(rp);
		break;
	case DTYPE_GEN_READ2_RESP:
	case DTYPE_DCS_READ2_RESP:
		mipi_dsi_short_read2_resp(rp);
		break;
	case DTYPE_GEN_LREAD_RESP:
	case DTYPE_DCS_LREAD_RESP:
		mipi_dsi_long_read_resp(rp);
		rp->len -= 2; /* extra 2 bytes added */
		rp->len -= diff; /* align bytes */
		break;
	default:
		break;
	}

	return rp->len;
}

int mipi_dsi_cmds_rx_new(struct dsi_buf *tp, struct dsi_buf *rp,
			struct dcs_cmd_req *req, int rlen)
{
	struct dsi_cmd_desc *cmds;
	int cnt, len, diff, pkt_size;
	char cmd;

	if (req->flags & CMD_REQ_NO_MAX_PKT_SIZE) {
		/* Only support rlen = 4*n */
		rlen += 3;
		rlen &= ~0x03;
	}

	cmds = req->cmds;

	len = rlen;
	diff = 0;

	if (len <= 2)
		cnt = 4;	/* short read */
	else {
		if (len > MIPI_DSI_LEN)
			len = MIPI_DSI_LEN;	/* 8 bytes at most */

		len = (len + 3) & ~0x03; /* len 4 bytes align */
		diff = len - rlen;
		/*
		 * add extra 2 bytes to len to have overall
		 * packet size is multipe by 4. This also make
		 * sure 4 bytes dcs headerlocates within a
		 * 32 bits register after shift in.
		 * after all, len should be either 6 or 10.
		 */
		len += 2;
		cnt = len + 6; /* 4 bytes header + 2 bytes crc */
	}

	if (!(req->flags & CMD_REQ_NO_MAX_PKT_SIZE)) {


		/* packet size need to be set at every read */
		pkt_size = len;
		max_pktsize[0] = pkt_size;
		mipi_dsi_enable_irq(DSI_CMD_TERM);
		mipi_dsi_buf_init(tp);
		mipi_dsi_cmd_dma_add(tp, pkt_size_cmd);
		mipi_dsi_cmd_dma_tx(tp);
	}

	mipi_dsi_enable_irq(DSI_CMD_TERM);
	mipi_dsi_buf_init(tp);
	mipi_dsi_cmd_dma_add(tp, cmds);

	/* transmit read comamnd to client */
	mipi_dsi_cmd_dma_tx(tp);

	/*
	 * once cmd_dma_done interrupt received,
	 * return data from client is ready and stored
	 * at RDBK_DATA register already
	 */
	mipi_dsi_buf_init(rp);
	if (req->flags & CMD_REQ_NO_MAX_PKT_SIZE) {
		/*
		 * expect rlen = n * 4
		 * short alignement for start addr
		 */
		rp->data += 2;
	}

	mipi_dsi_cmd_dma_rx(rp, cnt);

	if (req->flags & CMD_REQ_NO_MAX_PKT_SIZE) {
		/*
		 * remove extra 2 bytes from previous
		 * rx transaction at shift register
		 * which was inserted during copy
		 * shift registers to rx buffer
		 * rx payload start from long alignment addr
		 */
		rp->data += 2;
	}

	cmd = rp->data[0];
	switch (cmd) {
	case DTYPE_ACK_ERR_RESP:
		pr_debug("%s: rx ACK_ERR_PACLAGE\n", __func__);
		break;
	case DTYPE_GEN_READ1_RESP:
	case DTYPE_DCS_READ1_RESP:
		mipi_dsi_short_read1_resp(rp);
		break;
	case DTYPE_GEN_READ2_RESP:
	case DTYPE_DCS_READ2_RESP:
		mipi_dsi_short_read2_resp(rp);
		break;
	case DTYPE_GEN_LREAD_RESP:
	case DTYPE_DCS_LREAD_RESP:
		mipi_dsi_long_read_resp(rp);
		rp->len -= 2; /* extra 2 bytes added */
		rp->len -= diff; /* align bytes */
		break;
	default:
		break;
	}

	return rp->len;
}
#ifdef CONFIG_MIPI_DSI_LP_RX 
/*embedding max pkt size cmd and NULL pkt in one burst*/
/*static char null_pkt1[8] = {0x00, 0x00, 0x09, 0x40, 0x01, 0x00, 0x37, 0x80};*/

int mipi_dsi_cmds_rx_lp_new(struct dsi_buf *tp, struct dsi_buf *rp,
			struct dcs_cmd_req *req, int rlen)

{
	struct dsi_cmd_desc *cmds;

	int cnt, len, diff, video_mode;
	unsigned long flag;
	char cmd;

	uint32 dsi_ctrl, ctrl;

	if (req->flags & CMD_REQ_NO_MAX_PKT_SIZE) {
		/* Only support rlen = 4*n */
		rlen += 3;
		rlen &= ~0x03;
	}
	cmds = req->cmds;

	len = rlen;
	diff = 0;

	if (len <= 2)
		cnt = 4;	/* short read */
	else {
		if (len > MIPI_DSI_LEN)
			len = MIPI_DSI_LEN;	/* 8 bytes at most */

		len = (len + 3) & ~0x03; /* len 4 bytes align */
		diff = len - rlen;
		/*
		 * add extra 2 bytes to len to have overall
		 * packet size is multipe by 4. This also make
		 * sure 4 bytes dcs headerlocates within a
		 * 32 bits register after shift in.
		 * after all, len should be either 6 or 10.
		 */
		len += 2;
		cnt = len + 6; /* 4 bytes header + 2 bytes crc */
	}


	/* turn on cmd mode
	* for video mode, do not send cmds more than
	* one pixel line, since it only transmit it
	* during BLLP.
	*/
	dsi_ctrl = MIPI_INP(MIPI_DSI_BASE + 0x0000);
	video_mode = dsi_ctrl & 0x02; /* VIDEO_MODE_EN */
	if (video_mode) {
		ctrl = dsi_ctrl | 0x04; /* CMD_MODE_EN */
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, ctrl);
	}

	spin_lock_irqsave(&dsi_mdp_lock, flag);
	mipi_dsi_enable_irq(DSI_CMD_TERM);
	dsi_mdp_busy = TRUE;
	spin_unlock_irqrestore(&dsi_mdp_lock, flag);

	if (!(req->flags & CMD_REQ_NO_MAX_PKT_SIZE)) {
		mipi_dsi_buf_init(tp);
		tp->len = 8;
		memcpy(tp->start, null_pkt1, tp->len);
		tp->data = tp->start;
		/* transmit read comamnd to client */
		if (video_mode)
			mdp4_dsi_video_wait4dmap_for_dsi(0);

		mipi_dsi_cmd_dma_tx(tp);
	}

	/*
	 * once cmd_dma_done interrupt received,
	 * return data from client is ready and stored
	 * at RDBK_DATA register already
	 */
	mipi_dsi_enable_irq(DSI_CMD_TERM);
	mipi_dsi_buf_init(tp);
	tp->len = 8;
	memcpy(tp->start, cmds, tp->len);
	tp->data = tp->start;
	/* transmit read comamnd to client */
	if (video_mode)
		mdp4_dsi_video_wait4dmap_for_dsi(0);

	mipi_dsi_cmd_dma_tx(tp);
	mipi_dsi_disable_irq(DSI_CMD_TERM);

	/*
	 * expect rlen = n * 4
	 * short alignement for start addr
	 */
	if (req->flags & CMD_REQ_NO_MAX_PKT_SIZE) {
		rp->data += 2;

	mipi_dsi_cmd_dma_rx(rp, cnt);


	/*restore*/
	if (video_mode)
		MIPI_OUTP(MIPI_DSI_BASE + 0x0000, dsi_ctrl);

	spin_lock_irqsave(&dsi_mdp_lock, flag);
	dsi_mdp_busy = FALSE;
	mipi_dsi_disable_irq(DSI_CMD_TERM);
	complete(&dsi_mdp_comp);
	spin_unlock_irqrestore(&dsi_mdp_lock, flag);

	/*
	 * remove extra 2 bytes from previous
	 * rx transaction at shift register
	 * which was inserted during copy
	 * shift registers to rx buffer
	 * rx payload start from long alignment addr
	 */
	if (req->flags & CMD_REQ_NO_MAX_PKT_SIZE)
		rp->data += 2;

	cmd = rp->data[0];
	switch (cmd) {
	case DTYPE_ACK_ERR_RESP:
		pr_debug("%s: rx ACK_ERR_PACLAGE\n", __func__);
		break;
	case DTYPE_GEN_READ1_RESP:
	case DTYPE_DCS_READ1_RESP:
		pr_debug("%s: rx DTYPE_DCS_READ1_RESP\n", __func__);
		mipi_dsi_short_read1_resp(rp);
		break;
	case DTYPE_GEN_READ2_RESP:
	case DTYPE_DCS_READ2_RESP:
		pr_debug("%s: rx DTYPE_DCS_READ2_RESP\n", __func__);

		mipi_dsi_short_read2_resp(rp);
		break;
	case DTYPE_GEN_LREAD_RESP:
	case DTYPE_DCS_LREAD_RESP:
		pr_debug("%s: rx DTYPE_DCS_LREAD_RESP\n", __func__);

		mipi_dsi_long_read_resp(rp);
		rp->len -= 2; /* extra 2 bytes added */
		rp->len -= diff; /* align bytes */
		break;
	default:
		pr_err("%s: rx default !!! : %x\n", __func__, cmd);
		break;
	}

	return rp->len;
   }
}
#endif
#if defined(CONFIG_MIPI_MDP_REGISTER_DUMP)
extern boolean mdp_current_clk_on;
extern struct mdp4_statistic mdp4_stat;
int normalcase = 1;
void dumpreg(int error)
{

	uint32 tmp0x0,tmp0x4,tmp0x8,tmp0xc;
	int i;

	if (error)
		pr_err("%s: =============IT's normal case ==============\n", __func__);
	else
		pr_err("%s: =============IT's Fail case ==============\n", __func__);

	//for(int i=0; i<0x558;){
	for(i=0; i< 86; i++){ //0x558/16 = 85
		tmp0x0 = MIPI_INP(MIPI_DSI_BASE+(i*16)+0x0);
		tmp0x4 = MIPI_INP(MIPI_DSI_BASE+(i*16)+0x4);
		tmp0x8 = MIPI_INP(MIPI_DSI_BASE+(i*16)+0x8);

		if (i != 85)
			tmp0xc = MIPI_INP(MIPI_DSI_BASE+(i*16)+0xc);
		else
			tmp0xc = 0 ;

		pr_err("[%04x] : %08x %08x %08x %08x\n",i*16, tmp0x0,tmp0x4,tmp0x8,tmp0xc);
	}
	pr_err("%s: ============= END ==============\n", __func__);

}

void dumstate(int error)
{
	if (error)
		pr_err("%s: =============IT's normal case ==============\n", __func__);
	else
		pr_err("%s: =============IT's Fail case ==============\n", __func__);

	pr_err("intr_tot :%08lu\n", mdp4_stat.intr_tot);
	pr_err("intr_underrun_p :%08lu\n", mdp4_stat.intr_underrun_p);
	pr_err("intr_dma_p :%08lu\n", mdp4_stat.intr_dma_p);
	pr_err("dsi_clk_on :%08lu\n", mdp4_stat.dsi_clk_on);
	pr_err("dsi_clk_off :%08lu\n", mdp4_stat.dsi_clk_off);
	pr_err("intr_dsi :%08lu\n", mdp4_stat.intr_dsi);
	pr_err("intr_dsi_mdp :%08lu\n", mdp4_stat.intr_dsi_mdp);
	pr_err("intr_dsi_cmd :%08lu\n", mdp4_stat.intr_dsi_cmd);
	pr_err("intr_dsi_err :%08lu\n", mdp4_stat.intr_dsi_err);
	pr_err("mdp_current_clk_on : %08d\n", mdp_current_clk_on);
	pr_err("%s: ============= END ==============\n", __func__);
}
void mdp4_dump_regs(void)
{
	int i, z, start, len;
	int offsets[] = {0x0, 0x200, 0x10000, 0x18000,  0x20000,  0x30000,  0x40000,  0x50000, 0x88000, 0x90000, 0xB0000, 0xD0000, 0xE0000}; 
	int length[]  = {24,     64,     101,     101,       32,       32,       32,       32,     101,      64,      64,      27,      64};

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	for (i = 0; i < sizeof(offsets) / sizeof(int); i++) {
		start = offsets[i];
		len   = length[i];
		printk("-------- Address %05x: -------\n", start);
		for (z = 0; z < len; z++) {
			if ((z & 3) == 0)
				printk("%05x:", start + (z * 4));
			printk(" %08x", inpdw(MDP_BASE + start + (z * 4)));
			if ((z & 3) == 3)
				printk("\n");
		}
		if ((z & 3) != 0)
			printk("\n");
	}
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void dsi_clk_dump(void)
{
	printk(" ======== dsi clock dump ++++  =====================\n");
	printk(" 0x0130 : %08x\n",MIPI_INP(mmss_cc_base + 0x0130));
	printk(" 0x0134 : %08x\n",MIPI_INP(mmss_cc_base + 0x0134));
	printk(" 0x0138 : %08x\n",MIPI_INP(mmss_cc_base + 0x0138));
	printk(" 0x004c : %08x\n",MIPI_INP(mmss_cc_base + 0x004c));
	printk(" 0x0050 : %08x\n",MIPI_INP(mmss_cc_base + 0x0050));
	printk(" 0x0054 : %08x\n",MIPI_INP(mmss_cc_base + 0x0054));
	printk(" ======== dsi clock dump ---- =====================\n");
}
#endif
int mipi_dsi_cmd_dma_tx(struct dsi_buf *tp)
{
	unsigned long flags;

#ifdef DSI_HOST_DEBUG
	int i;
	char *bp;
	char buffer[256];

	bp = tp->data;
	buffer[0] = 0; // make length 0
	snprintf( buffer +strlen(buffer), sizeof(buffer) -strlen(buffer) -1, "%s:%2d:", __func__, tp->len );
	switch( tp->data[2] )
	{
		case 0x05: strcat( buffer, "D_WR "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		case 0x15: strcat( buffer, "D_W1 "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		case 0x06: strcat( buffer, "D_RD "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		case 0x39: strcat( buffer, "D_LW "); if( tp->len < 5 ) strcat( buffer, "(!) "); break;
		case 0x03: strcat( buffer, "G_WR "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		case 0x13: strcat( buffer, "G_W1 "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		case 0x23: strcat( buffer, "G_W2 "); break;
		case 0x29: strcat( buffer, "G_LW "); if( tp->len < 5 ) strcat( buffer, "(!) "); break;
		case 0x04: strcat( buffer, "G_RD "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		case 0x14: strcat( buffer, "G_R1 "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		case 0x24: strcat( buffer, "G_R2 "); break;
		case 0x37: strcat( buffer, "PKTL "); if( tp->len != 4 ) strcat( buffer, "(!) "); break;
		default: strcat( buffer, "???? "); break;

	}

	for (i = 0; i <tp->len; i++) { snprintf( buffer +strlen(buffer), sizeof(buffer) -strlen(buffer) -1, "%2x ", *bp++ ); }
	pr_info( "%s", buffer);
#endif

	if (tp->len == 0) {
		pr_err("%s: Error, len=0\n", __func__);
		return 0;
	}

	spin_lock_irqsave(&dsi_mdp_lock, flags);
	tp->len += 3;
	tp->len &= ~0x03;	/* multipled by 4 */

	tp->dmap = dma_map_single(&dsi_dev, tp->data, tp->len, DMA_TO_DEVICE);
	if (dma_mapping_error(&dsi_dev, tp->dmap))
		pr_err("%s: dmap mapp failed\n", __func__);

	INIT_COMPLETION(dsi_dma_comp);

	MIPI_OUTP(MIPI_DSI_BASE + 0x044, tp->dmap);
	MIPI_OUTP(MIPI_DSI_BASE + 0x048, tp->len);
	wmb();
	MIPI_OUTP(MIPI_DSI_BASE + 0x08c, 0x01);	/* trigger */
	wmb();
	spin_unlock_irqrestore(&dsi_mdp_lock, flags);

	if (!wait_for_completion_timeout(&dsi_dma_comp,
					msecs_to_jiffies(200))) {
		pr_err("%s: dma timeout error\n", __func__);
#if defined(CONFIG_MIPI_MDP_REGISTER_DUMP)
		dumpreg(0);
		dumstate(0);
		mdp4_dump_regs();
		dsi_clk_dump();
		console_verbose();
		dump_stack();
#endif
	}

	dma_unmap_single(&dsi_dev, tp->dmap, tp->len, DMA_TO_DEVICE);
	tp->dmap = 0;
	return tp->len;
}

int mipi_dsi_cmd_dma_rx(struct dsi_buf *rp, int rlen)
{
	uint32 *lp, data;
	int i, off, cnt;

	lp = (uint32 *)rp->data;
	cnt = rlen;
	cnt += 3;
	cnt >>= 2;

	if (cnt > 4)
		cnt = 4; /* 4 x 32 bits registers only */

	off = 0x068;	/* DSI_RDBK_DATA0 */
	off += ((cnt - 1) * 4);


	for (i = 0; i < cnt; i++) {
		data = (uint32)MIPI_INP(MIPI_DSI_BASE + off);
		*lp++ = ntohl(data);	/* to network byte order */
		off -= 4;
		rp->len += sizeof(*lp);
	}

	return rlen;
}

static void mipi_dsi_wait4video_eng_busy(void)
{
	mipi_dsi_wait4video_done();
	/* delay 4 ms to skip BLLP */
	usleep(4000);
}

void mipi_dsi_cmd_mdp_busy(void)
{
	unsigned long flags;
	int need_wait = 0;

	pr_debug("%s: start pid=%d\n",
				__func__, current->pid);
	spin_lock_irqsave(&dsi_mdp_lock, flags);
	if (dsi_mdp_busy == TRUE)
		need_wait++;
	spin_unlock_irqrestore(&dsi_mdp_lock, flags);

	if (need_wait) {
		/* wait until DMA finishes the current job */
		pr_debug("%s: pending pid=%d\n",
				__func__, current->pid);
		if (!wait_for_completion_timeout(&dsi_mdp_comp,
						msecs_to_jiffies(100))) {
			pr_err("%s: mdp timeout error\n", __func__);
		}
	}
	pr_debug("%s: done pid=%d\n",
				__func__, current->pid);
}

/*
 * mipi_dsi_cmd_get: cmd_mutex acquired by caller
 */
struct dcs_cmd_req *mipi_dsi_cmdlist_get(void)
{
	struct dcs_cmd_req *req = NULL;

	if (cmdlist.get != cmdlist.put) {
		req = &cmdlist.list[cmdlist.get];
		cmdlist.get++;
		cmdlist.get %= CMD_REQ_MAX;
		cmdlist.tot--;
		pr_debug("%s: tot=%d put=%d get=%d\n", __func__,
		cmdlist.tot, cmdlist.put, cmdlist.get);
	}
	return req;
}
void mipi_dsi_cmdlist_tx(struct dcs_cmd_req *req)
{
	struct dsi_buf *tp;
	int ret;

	mipi_dsi_buf_init(&dsi_tx_buf);
	tp = &dsi_tx_buf;
	if (req->flags & CMD_REQ_SINGLE_TX)
		ret = mipi_dsi_cmds_single_tx(tp, req->cmds, req->cmds_cnt);
	else
		ret = mipi_dsi_cmds_tx(tp, req->cmds, req->cmds_cnt);

	if (req->cb)
		req->cb(ret);

}

void mipi_dsi_cmdlist_rx(struct dcs_cmd_req *req)
{
	int len;
	u32 *dp;
	struct dsi_buf *tp;
	struct dsi_buf *rp;

	mipi_dsi_buf_init(&dsi_tx_buf);
	mipi_dsi_buf_init(&dsi_rx_buf);

	tp = &dsi_tx_buf;
	rp = &dsi_rx_buf;

#ifdef CONFIG_MIPI_DSI_LP_RX
	len = mipi_dsi_cmds_rx_lp_new(tp, rp, req, req->rlen);
#else
	len = mipi_dsi_cmds_rx_new(tp, rp, req, req->rlen);
#endif
	dp = (u32 *)rp->data;

	if (req->cb)
		req->cb(*dp);
}

void mipi_dsi_cmdlist_commit(int from_mdp)
{
	struct dcs_cmd_req *req;
	u32 dsi_ctrl;

	mutex_lock(&cmd_mutex);
	req = mipi_dsi_cmdlist_get();

	/* make sure dsi_cmd_mdp is idle */
	mipi_dsi_cmd_mdp_busy();

	if (req == NULL)
		goto need_lock;

	pr_debug("%s:  from_mdp=%d pid=%d\n", __func__, from_mdp, current->pid);

	dsi_ctrl = MIPI_INP(MIPI_DSI_BASE + 0x0000);
	if (dsi_ctrl & 0x02) {
		/* video mode, make sure video engine is busy
		 * so dcs command will be sent at start of BLLP
		 */
		mipi_dsi_wait4video_eng_busy();
	} else {
		/* command mode */
		if (!from_mdp) { /* cmdlist_put */
			/* make sure dsi_cmd_mdp is idle */
			mipi_dsi_cmd_mdp_busy();
		}
	}

	if (req->flags & CMD_REQ_RX)
		mipi_dsi_cmdlist_rx(req);
	else
		mipi_dsi_cmdlist_tx(req);

need_lock:

	if (from_mdp) /* from pipe_commit */
		mipi_dsi_cmd_mdp_start();

	mutex_unlock(&cmd_mutex);
}

int mipi_dsi_cmdlist_put(struct dcs_cmd_req *cmdreq)
{
	struct dcs_cmd_req *req;
	int ret = 0;

	mutex_lock(&cmd_mutex);
	req = &cmdlist.list[cmdlist.put];
	*req = *cmdreq;
	cmdlist.put++;
	cmdlist.put %= CMD_REQ_MAX;
	cmdlist.tot++;
	if (cmdlist.put == cmdlist.get) {
		/* drop the oldest one */
		pr_debug("%s: DROP, tot=%d put=%d get=%d\n", __func__,
			cmdlist.tot, cmdlist.put, cmdlist.get);
		cmdlist.get++;
		cmdlist.get %= CMD_REQ_MAX;
		cmdlist.tot--;
	}
	mutex_unlock(&cmd_mutex);

	ret++;
	pr_debug("%s: tot=%d put=%d get=%d\n", __func__,
		cmdlist.tot, cmdlist.put, cmdlist.get);

	if (req->flags & CMD_CLK_CTRL)
		mipi_dsi_clk_cfg(1);

	if (req->flags & CMD_REQ_COMMIT)
		mipi_dsi_cmdlist_commit(0);

	if (req->flags & CMD_CLK_CTRL)
		mipi_dsi_clk_cfg(0);

	return ret;
}

void mipi_dsi_irq_set(uint32 mask, uint32 irq)
{
	uint32 data;

	data = MIPI_INP(MIPI_DSI_BASE + 0x010c);/* DSI_INTR_CTRL */
	data &= ~mask;
	data |= irq;
	MIPI_OUTP(MIPI_DSI_BASE + 0x010c, data);
}


void mipi_dsi_ack_err_status(void)
{
	uint32 status;

	status = MIPI_INP(MIPI_DSI_BASE + 0x0064);/* DSI_ACK_ERR_STATUS */

	if (status) {
		MIPI_OUTP(MIPI_DSI_BASE + 0x0064, status);
		/*
		 * base on hw enginner, write an extra 0 needed
		 * to clear error bits
		 */
		MIPI_OUTP(MIPI_DSI_BASE + 0x0064, ~status);
		pr_debug("%s: status=%x\n", __func__, status);
	}
}

void mipi_dsi_timeout_status(void)
{
	uint32 status;

	status = MIPI_INP(MIPI_DSI_BASE + 0x00bc);/* DSI_TIMEOUT_STATUS */
	if (status & 0x0111) {
		MIPI_OUTP(MIPI_DSI_BASE + 0x00bc, status);
		pr_debug("%s: status=%x\n", __func__, status);
	}
}

void mipi_dsi_dln0_phy_err(void)
{
	uint32 status;

	status = MIPI_INP(MIPI_DSI_BASE + 0x00b0);/* DSI_DLN0_PHY_ERR */

	if (status & 0x011111) {
		MIPI_OUTP(MIPI_DSI_BASE + 0x00b0, status);
		pr_debug("%s: status=%x\n", __func__, status);
	}
}

void mipi_dsi_fifo_status(void)
{
	uint32 status;

	status = MIPI_INP(MIPI_DSI_BASE + 0x0008);/* DSI_FIFO_STATUS */

	if (status & 0x44444489) {
		MIPI_OUTP(MIPI_DSI_BASE + 0x0008, status);
		pr_err("%s: Error: status=%x\n", __func__, status);
		mipi_dsi_sw_reset();
		schedule_work(&mdp_reset_work);
	}
}

void mipi_dsi_status(void)
{
	uint32 status;

	status = MIPI_INP(MIPI_DSI_BASE + 0x0004);/* DSI_STATUS */

	if (status & 0x80000000) {
		MIPI_OUTP(MIPI_DSI_BASE + 0x0004, status);
		pr_debug("%s: status=%x\n", __func__, status);
	}
}

void mipi_dsi_error(void)
{
	/* DSI_ERR_INT_MASK0 */
	mipi_dsi_ack_err_status();	/* mask0, 0x01f */
	mipi_dsi_timeout_status();	/* mask0, 0x0e0 */
	mipi_dsi_fifo_status();		/* mask0, 0x133d00 */
	mipi_dsi_status();		/* mask0, 0xc0100 */
	mipi_dsi_dln0_phy_err();	/* mask0, 0x3e00000 */
}

#if defined(CONFIG_RUNTIME_MIPI_CLK_CHANGE)
static int runtime_clk_chagne;
static int goal_fps;
int mipi_runtime_clk_change(int fps)
{
	int rc;

	mutex_lock(&dsi_vsync_mutex);
		
	runtime_clk_chagne = 1;
	goal_fps = fps;
		
	INIT_COMPLETION(dsi_vsync_comp);
#ifdef CONFIG_FB_MSM_MIPI_NT35510_CMD_WVGA_PT_PANEL
	mipi_dsi_enable_irq(DSI_MDP_TERM);
	mipi_dsi_irq_set(DSI_INTR_CMD_MDP_DONE_MASK,
					DSI_INTR_CMD_MDP_DONE_MASK);
#else
	mipi_dsi_enable_irq(DSI_VIDEO_TERM);
	mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK,
					DSI_INTR_VIDEO_DONE_MASK);
#endif

	rc = wait_for_completion_timeout(&dsi_vsync_comp, 5 * HZ);

	if (!rc) {
		pr_err("%s: dma timeout error\n", __func__);
	}
		
	mutex_unlock(&dsi_vsync_mutex);

	return rc;
}
#endif

#if defined(CONFIG_FB_MSM_CAMERA_CSC)
extern int csc_updated;
static uint32 csc_reg[CSC_UPDATA_SIZE][2];
static int csc_length;

int mipi_runtime_csc_update(uint32_t reg[][2], int length)
{
	int rc;
	int loop;

	mutex_lock(&dsi_vsync_mutex);

	csc_length = length;

	for(loop = 0; loop < csc_length; loop++) {
		csc_reg[loop][0] = reg[loop][0];
		csc_reg[loop][1] = reg[loop][1];
	}

	INIT_COMPLETION(dsi_vsync_comp);
	mipi_dsi_enable_irq(DSI_VIDEO_TERM);
	mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK,
					DSI_INTR_VIDEO_DONE_MASK);

	rc = wait_for_completion_timeout(&dsi_vsync_comp, 2 * HZ);

	if (!rc) {
		pr_err("%s: dma timeout error\n", __func__);
	}
		
	mutex_unlock(&dsi_vsync_mutex);

	return rc;
}
#endif

irqreturn_t mipi_dsi_isr(int irq, void *ptr)
{
	uint32 isr;

	isr = MIPI_INP(MIPI_DSI_BASE + 0x010c);/* DSI_INTR_CTRL */
	MIPI_OUTP(MIPI_DSI_BASE + 0x010c, isr);

//	pr_debug("%s: isr=%x\n", __func__, (int)isr);

#ifdef CONFIG_FB_MSM_MDP40
	mdp4_stat.intr_dsi++;
#endif
	if (isr & DSI_INTR_ERROR) {
		mipi_dsi_mdp_stat_inc(STAT_DSI_ERROR);
		spin_lock(&dsi_mdp_lock);
		dsi_ctrl_lock = FALSE;
		dsi_mdp_busy = FALSE;
		mipi_dsi_disable_irq_nosync(DSI_MDP_TERM);
		mipi_dsi_error();
		complete(&dsi_mdp_comp);
		spin_unlock(&dsi_mdp_lock);
	}

	if (isr & DSI_INTR_VIDEO_DONE) {
#if defined(CONFIG_RUNTIME_MIPI_CLK_CHANGE)
#if defined(CONFIG_FB_MSM_CAMERA_CSC)
		if (runtime_clk_chagne) {
			if (inpdw(MDP_BASE+0x90014)) {
				pr_debug("%s VIDEO_ENGINE BUSY", __func__);
			} else {
				mipi_dsi_configure_dividers(goal_fps);
				runtime_clk_chagne = 0;
				mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
				mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
				complete(&dsi_vsync_comp);
				pr_debug("%s VIDEO_ENGINE NOT BUSY", __func__);
			}

			spin_lock(&dsi_mdp_lock);
			complete(&dsi_video_comp);
			spin_unlock(&dsi_mdp_lock);
		} else if (csc_updated) {
			int loop = 0;
			for (loop = 0; loop < csc_length; loop++) {
				outpdw(csc_reg[loop][0], csc_reg[loop][1]);
//				pr_debug("%s 0x%x 0x%x ", __func__, csc_reg[loop][0], csc_reg[loop][1]);
			}
			csc_updated = 0;
			mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
			mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
			complete(&dsi_vsync_comp);
//			pr_info("%s CSC update done \n", __func__);
//			pr_debug("%s VIDEO_ENGINE NOT BUSY", __func__);

			spin_lock(&dsi_mdp_lock);
			complete(&dsi_video_comp);
			spin_unlock(&dsi_mdp_lock);
		} else {
			spin_lock(&dsi_mdp_lock);
			mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
			mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
			complete(&dsi_video_comp);
			spin_unlock(&dsi_mdp_lock);
		}
#else
		if (runtime_clk_chagne) {
			if (inpdw(MDP_BASE+0x90014)) {
				pr_debug("%s VIDEO_ENGINE BUSY", __func__);
				
			} else {
				mipi_dsi_configure_dividers(goal_fps);
				runtime_clk_chagne = 0;
				mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
				mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
				complete(&dsi_vsync_comp);
				pr_debug("%s VIDEO_ENGINE NOT BUSY", __func__);
			}

			spin_lock(&dsi_mdp_lock);
			complete(&dsi_video_comp);
			spin_unlock(&dsi_mdp_lock);
		} else {
			spin_lock(&dsi_mdp_lock);
			mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
			mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
			complete(&dsi_video_comp);
			spin_unlock(&dsi_mdp_lock);
		}
#endif
#else
#if defined(CONFIG_FB_MSM_CAMERA_CSC)
		if (csc_updated) {
			int loop = 0;
			for (loop = 0; loop < csc_length; loop++) {
				outpdw(csc_reg[loop][0], csc_reg[loop][1]);
				pr_debug("%s 0x%x 0x%x ", __func__, csc_reg[loop][0], csc_reg[loop][1]);
			}
			csc_updated = 0;
			mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
			mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
			complete(&dsi_vsync_comp);
			pr_info("%s CSC update done \n", __func__);
			pr_debug("%s VIDEO_ENGINE NOT BUSY", __func__);

			spin_lock(&dsi_mdp_lock);
			complete(&dsi_video_comp);
			spin_unlock(&dsi_mdp_lock);
		} else {
			spin_lock(&dsi_mdp_lock);
			mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
			mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
			complete(&dsi_video_comp);
			spin_unlock(&dsi_mdp_lock);
		}
#else
		spin_lock(&dsi_mdp_lock);
		mipi_dsi_irq_set(DSI_INTR_VIDEO_DONE_MASK, 0);
		mipi_dsi_disable_irq_nosync(DSI_VIDEO_TERM);
		complete(&dsi_video_comp);
		spin_unlock(&dsi_mdp_lock);
#endif
#endif
	}

	if (isr & DSI_INTR_CMD_DMA_DONE) {
		mipi_dsi_mdp_stat_inc(STAT_DSI_CMD);
		spin_lock(&dsi_mdp_lock);
		complete(&dsi_dma_comp);
		dsi_ctrl_lock = FALSE;
		mipi_dsi_disable_irq_nosync(DSI_CMD_TERM);
		spin_unlock(&dsi_mdp_lock);
	}

	if (isr & DSI_INTR_CMD_MDP_DONE) {
#if defined(CONFIG_FB_MSM_CAMERA_CSC)
		if (csc_updated) {
			int loop = 0;
			for (loop = 0; loop < csc_length; loop++) {
				outpdw(csc_reg[loop][0], csc_reg[loop][1]);
				pr_debug("%s 0x%x 0x%x ", __func__, csc_reg[loop][0], csc_reg[loop][1]);
			}
			csc_updated = 0;
			mipi_dsi_mdp_stat_inc(STAT_DSI_MDP);
			mipi_dsi_disable_irq_nosync(DSI_MDP_TERM);
			complete(&dsi_vsync_comp);
			pr_info("%s CSC update done \n", __func__);
			pr_debug("%s VIDEO_ENGINE NOT BUSY", __func__);

			spin_lock(&dsi_mdp_lock);
			dsi_ctrl_lock = FALSE;
			dsi_mdp_busy = FALSE;
			complete(&dsi_mdp_comp);
			spin_unlock(&dsi_mdp_lock);
		}
		else
#endif
		{
			mipi_dsi_mdp_stat_inc(STAT_DSI_MDP);
			spin_lock(&dsi_mdp_lock);
			dsi_ctrl_lock = FALSE;
			dsi_mdp_busy = FALSE;
			mipi_dsi_disable_irq_nosync(DSI_MDP_TERM);
			complete(&dsi_mdp_comp);
			spin_unlock(&dsi_mdp_lock);
		}
	}


	return IRQ_HANDLED;
}
