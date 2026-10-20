// SPDX-License-Identifier: GPL-2.0
/*
 * pbwrap.c - Phoenix camera pre-processing wrapper (v4l2loopback-style)
 *
 * Wraps the OV13850 rear camera (msm8939 CAMSS, MIPI RAW10 packed "pBAA"
 * output) behind a standard V4L2 capture device that emits fully
 * pre-processed frames:
 *
 *   INPUT  (producer side, write()):
 *     4,139,520 bytes/frame = 2112 x 1568 pBAA (MIPI RAW10 packed, SBGGR):
 *       bytes 0-3 of each 5-byte group = pixel high 8 bits,
 *       byte 4 = low 2 bits, LSB first (px_k = (byte_k << 2) | frag_k).
 *     Producer = `v4l2-ctl --stream-to=/dev/pbwrapN` reading /dev/video0.
 *     One producer at a time; frames consumed from a partial-write
 *     accumulator, so any producer tool works.
 *
 *   OUTPUT (consumer side, standard V4L2 capture ioctls, mmap buffers):
 *     fmt_index 0: V4L2_PIX_FMT_RGB24 (2112x1568x3 = 9,933,312 B) — full
 *       pipeline: unpack -> black level -> WB -> phase-correct bilinear
 *       demosaic -> per-frame auto window + gamma tonemap.
 *     fmt_index 1: V4L2_PIX_FMT_SBGGR16 (2112x1568x2 = 6,615,296 B) —
 *       unpacked 16-bit bayer, LSB-aligned, black level + WB applied.
 *
 *   The whole chain is integer-only and is a literal translation of the
 *   host-verified decoder (camera-ref/tools/decode_ov13850.py, validated
 *   against the r66 gold-reference frame and the r72 white-wall round).
 *   The gamma curve (n^(1/2.2) over the 0.5%..97.5% luminance window) is
 *   burned in as a 1024-entry LUT (lut.h) identical to the host's powf.
 *
 * Buffer flow (v4l2loopback-ish, hand-rolled, vb2-free):
 *   consumer REQBUFS(n) allocates vmalloc buffers; QBUF pushes them onto
 *   free_list (sans fill) in QBUF order; the producer pops the front,
 *   converts the frame into it, then puts it on ready_list.  DQBUF pops
 *   ready_list FIFO.  No free buffer -> the incoming frame is dropped
 *   (never blocks the camera stream).  Consumer slower than camera ->
 *   rotation speed-limited to consumer (no drops, bounded lag by n).
 *
 * Producer claim: open() with write access claims the single producer
 * slot; concurrent producers get -EBUSY.  Readers/consumers may coexist.
 *
 * Build (out-of-tree, matches the running 6.12.1-msm8916 kernel):
 *   make -C $(kernel tree) M=$(this dir) ARCH=arm64 \
 *        CROSS_COMPILE=aarch64-linux-gnu- modules
 *
 * Usage (device):
 *   insmod pbwrap.ko wb_r_256=492 wb_b_256=343 bl=17
 *   # camera bound+exposed as usual (see CAMERA.md), then:
 *   v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=120 \
 *           --stream-to=/dev/pbwrapN &
 *   ffmpeg -f v4l2 -video_size 2112x1568 -framerate 12 -i /dev/pbwrapN \
 *          -frames:v 90 -c:v libx264 -preset veryfast -crf 24 wrap.mp4
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/time.h>
#include <linux/uaccess.h>

#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>

#include "lut.h"		/* pbw_gamma_lut[1024] */

#define PBW_NAME		"pbwrap"

/* Frame geometry: fixed to the verified OV13850 rear-camera capture mode */
#define PBW_W			2112
#define PBW_H			1568
#define PBW_NPIX		(PBW_W * PBW_H)		/* 3,311,616 */
#define PBW_FRAME_RAW		(PBW_NPIX * 5 / 4)	/* 4,139,520 */
#define PBW_FRAME_RGB		(PBW_NPIX * 3)		/* 9,933,312 */
#define PBW_FRAME_S16		(PBW_NPIX * 2)		/* 6,615,296 */

#define PBW_NFMT		2
#define PBW_NBUF_MAX		4

/* Output format table */
struct pbw_fmt {
	u32 fourcc;
	const char *desc;
	size_t bytesperline;
	size_t sizeimage;
};

static const struct pbw_fmt pbw_fmts[PBW_NFMT] = {
	{ V4L2_PIX_FMT_RGB24,	"8-bit RGB",		PBW_W * 3, PBW_FRAME_RGB },
	{ V4L2_PIX_FMT_SBGGR16,	"10-bit Bayer unpacked", PBW_W * 2, PBW_FRAME_S16 },
};

enum pbw_bufstate {
	PBW_BUF_IDLE = 0,	/* allocated, not queued */
	PBW_BUF_FREE,		/* on free_list, awaiting fill */
	PBW_BUF_READY,		/* on ready_list, awaiting DQBUF */
};

struct pbw_buffer {
	void *vaddr;
	unsigned int index;
	enum pbw_bufstate state;
	struct list_head list;
	atomic_t map_count;
	u32 sequence;
	u64 ts_ns;
};

struct pbw_device {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;

	struct mutex cfg_lock;		/* serialises ioctl state changes */
	spinlock_t list_lock;
	wait_queue_head_t ready_wq;

	struct list_head free_list;
	struct list_head ready_list;
	struct pbw_buffer bufs[PBW_NBUF_MAX];
	unsigned int nbufs;
	bool streaming;

	int fmt_index;			/* index into pbw_fmts */

	atomic_t producer_claim;
	atomic_t consumer_open;

	/* per-frame scratch (module-owned, vmalloc) */
	u16 *raw16;			/* unpacked RAW10 -> 16-bit bayer */
	u16 *pr, *pg, *pb;		/* WB'd/BL'd colour planes */

	/* producer frame accumulator (single producer claimed) */
	u8 *acc;
	size_t acc_have;

	/* stats */
	unsigned long frames_done;
	unsigned long frames_dropped;
};

struct pbw_fh {
	struct v4l2_fh fh;
	bool producer;
};

static int pbw_wb_r_256 = 492;		/* fixed point x256: 492/256 = 1.92 */
module_param_named(wb_r_256, pbw_wb_r_256, int, 0644);
MODULE_PARM_DESC(wb_r_256, "R white-balance gain x256 (default 492 = 1.92)");

static int pbw_wb_b_256 = 343;		/* 343/256 = 1.34 */
module_param_named(wb_b_256, pbw_wb_b_256, int, 0644);
MODULE_PARM_DESC(wb_b_256, "B white-balance gain x256 (default 343 = 1.34)");

static int pbw_black_level = 17;	/* measured 16.8 -> 17 */
module_param_named(bl, pbw_black_level, int, 0644);
MODULE_PARM_DESC(bl, "black level in 10-bit counts (default 17 = 16.8)");

static unsigned int pbw_dev_minor = -1U;
module_param_named(dev_minor, pbw_dev_minor, uint, 0444);
MODULE_PARM_DESC(dev_minor, "video device minor number (-1 = auto)");

static struct pbw_device *pbw_dev;

/* ------------------------------------------------------------------ */
/* conversion: one producer frame -> one consumer buffer              */
/* ------------------------------------------------------------------ */

static inline u16 pbw_at(const u16 *p, int y, int x)
{
	if (y < 0)
		y = 0;
	else if (y >= PBW_H)
		y = PBW_H - 1;
	if (x < 0)
		x = 0;
	else if (x >= PBW_W)
		x = PBW_W - 1;
	return p[(unsigned int)y * PBW_W + (unsigned int)x];
}

static inline u16 pbw_clamp1023(int v)
{
	return (u16)(v > 1023 ? 1023 : v);
}

static void pbw_unpack(const u8 *in, u16 *raw16)
{
	const u8 *p = in;
	u16 *d = raw16;
	size_t g;

	for (g = 0; g < PBW_NPIX / 4; g++, d += 4, p += 5) {
		u8 hib = p[4];
		d[0] = (u16)((p[0] << 2) | (hib & 0x03));
		d[1] = (u16)((p[1] << 2) | ((hib >> 2) & 0x03));
		d[2] = (u16)((p[2] << 2) | ((hib >> 4) & 0x03));
		d[3] = (u16)((p[3] << 2) | ((hib >> 6) & 0x03));
	}
}

/* split into colour planes, subtract black level, apply WB gains */
static void pbw_planes(const u16 *raw16, u16 *pr, u16 *pg, u16 *pb)
{
	int y, x;

	for (y = 0; y < PBW_H; y++) {
		const u16 *sp = raw16 + (unsigned int)y * PBW_W;
		u16 *rp = pr + (unsigned int)y * PBW_W;
		u16 *gp = pg + (unsigned int)y * PBW_W;
		u16 *bp = pb + (unsigned int)y * PBW_W;

		for (x = 0; x < PBW_W; x++) {
			int v = (int)sp[x] - pbw_black_level;
			if (v < 0)
				v = 0;
			switch (((y & 1) << 1) | (x & 1)) {
			case 0: /* (e,e) B */
				bp[x] = pbw_clamp1023(v * pbw_wb_b_256 / 256);
				break;
			case 1: /* (e,o) G1 */
				gp[x] = (u16)v;
				break;
			case 2: /* (o,e) G2 */
				gp[x] = (u16)v;
				break;
			case 3: /* (o,o) R */
				rp[x] = pbw_clamp1023(v * pbw_wb_r_256 / 256);
				break;
			}
		}
	}
}

/* CFA: B(e,e) G(e,o) G(o,e) R(o,o); G1 == G2merited after the r72 proof. */
static void pbw_demosaic(u16 *pr, u16 *pg, u16 *pb)
{
	int y, x;

	/* R plane: real at (o,o); (e,e) 4-diag, (e,o) vertical, (o,e) horizontal */
	for (y = 0; y < PBW_H; y++) {
		u16 *rp = pr + (unsigned int)y * PBW_W;
		for (x = 0; x < PBW_W; x++) {
			switch (((y & 1) << 1) | (x & 1)) {
			case 0: /* (e,e) B-site */
				rp[x] = pbw_clamp1023(
					((int)pbw_at(pr, y - 1, x - 1) +
					 pbw_at(pr, y - 1, x + 1) +
					 pbw_at(pr, y + 1, x - 1) +
					 pbw_at(pr, y + 1, x + 1)) / 4);
				break;
			case 1: /* (e,o) G1-site */
				rp[x] = pbw_clamp1023(
					((int)pbw_at(pr, y - 1, x) +
					 pbw_at(pr, y + 1, x)) / 2);
				break;
			case 2: /* (o,e) G2-site */
				rp[x] = pbw_clamp1023(
					((int)pbw_at(pr, y, x - 1) +
					 pbw_at(pr, y, x + 1)) / 2);
				break;
			}
		}
	}

	/* B plane: real at (e,e); (o,o) 4-diag, (e,o) horizontal, (o,e) vertical */
	for (y = 0; y < PBW_H; y++) {
		u16 *bp = pb + (unsigned int)y * PBW_W;
		for (x = 0; x < PBW_W; x++) {
			switch (((y & 1) << 1) | (x & 1)) {
			case 3: /* (o,o) R-site */
				bp[x] = pbw_clamp1023(
					((int)pbw_at(pb, y - 1, x - 1) +
					 pbw_at(pb, y - 1, x + 1) +
					 pbw_at(pb, y + 1, x - 1) +
					 pbw_at(pb, y + 1, x + 1)) / 4);
				break;
			case 1: /* (e,o) G1-site */
				bp[x] = pbw_clamp1023(
					((int)pbw_at(pb, y, x - 1) +
					 pbw_at(pb, y, x + 1)) / 2);
				break;
			case 2: /* (o,e) G2-site */
				bp[x] = pbw_clamp1023(
					((int)pbw_at(pb, y - 1, x) +
					 pbw_at(pb, y + 1, x)) / 2);
				break;
			}
		}
	}

	/* G plane: real at (e,o)+(o,e); (e,e)|(o,o) holes <- 4-axial average */
	for (y = 0; y < PBW_H; y++) {
		u16 *gp = pg + (unsigned int)y * PBW_W;
		for (x = 0; x < PBW_W; x++) {
			if (((y & 1) ^ (x & 1)) == 0)
				gp[x] = pbw_clamp1023(
					((int)pbw_at(pg, y - 1, x) +
					 pbw_at(pg, y + 1, x) +
					 pbw_at(pg, y, x - 1) +
					 pbw_at(pg, y, x + 1)) / 4);
		}
	}
}

/* per-frame auto window (0.5%..97.5% of luminance histogram) -> LUT output */
static void pbw_tonemap(const u16 *pr, const u16 *pg, const u16 *pb,
			u8 *out)
{
	static unsigned long hist[1024];
	size_t i, half, c;
	int lo = 0, hi = 1023, v, span;

	memset(hist, 0, sizeof(hist));
	for (i = 0; i < PBW_NPIX; i++) {
		unsigned int l = ((unsigned int)pr[i] + pg[i] + pb[i]) / 3;
		if (l > 1023)
			l = 1023;
		hist[l]++;
	}
	half = PBW_NPIX / 200;		/* 0.5% */
	c = 0;
	for (v = 0; v < 1024; v++) {
		c += hist[v];
		if (c >= half) {
			lo = v;
			break;
		}
	}
	c = 0;
	for (v = 0; v < 1024; v++) {
		c += hist[v];
		if (c >= PBW_NPIX - half) {
			hi = v;
			break;
		}
	}
	span = hi - lo;
	if (span < 1)
		span = 1;

	for (i = 0; i < PBW_NPIX; i++) {
		int rv = (int)pr[i] - lo;
		int gv = (int)pg[i] - lo;
		int bv = (int)pb[i] - lo;

		if (rv < 0)
			rv = 0;
		else if (rv > span)
			rv = span;
		if (gv < 0)
			gv = 0;
		else if (gv > span)
			gv = span;
		if (bv < 0)
			bv = 0;
		else if (bv > span)
			bv = span;

		/* normalized index into the shared-window LUT */
		out[0] = pbw_gamma_lut[rv * 1023 / span];
		out[1] = pbw_gamma_lut[gv * 1023 / span];
		out[2] = pbw_gamma_lut[bv * 1023 / span];
		out += 3;
	}
}

/* S16 output: processed bayer planes re-interleaved in the CFA layout */
static void pbw_planar_to_s16(const u16 *pr, const u16 *pg, const u16 *pb,
			      u16 *out)
{
	int y, x;

	for (y = 0; y < PBW_H; y++) {
		const u16 *rp = pr + (unsigned int)y * PBW_W;
		const u16 *gp = pg + (unsigned int)y * PBW_W;
		const u16 *bp = pb + (unsigned int)y * PBW_W;
		u16 *o = out + (unsigned int)y * PBW_W;

		for (x = 0; x < PBW_W; x++) {
			switch (((y & 1) << 1) | (x & 1)) {
			case 0:
				o[x] = bp[x];
				break;
			case 1:
			case 2:
				o[x] = gp[x];
				break;
			case 3:
				o[x] = rp[x];
				break;
			}
		}
	}
}

static void pbw_convert_frame(const u8 *raw, void *out, int fmt_index)
{
	struct pbw_device *d = pbw_dev;

	pbw_unpack(raw, d->raw16);
	pbw_planes(d->raw16, d->pr, d->pg, d->pb);

	if (pbw_fmts[fmt_index].fourcc == V4L2_PIX_FMT_RGB24) {
		pbw_demosaic(d->pr, d->pg, d->pb);
		pbw_tonemap(d->pr, d->pg, d->pb, out);
	} else {
		pbw_planar_to_s16(d->pr, d->pg, d->pb, out);
	}
}

/* ------------------------------------------------------------------ */
/* producer push                                                      */
/* ------------------------------------------------------------------ */

static void pbw_publish(void)
{
	struct pbw_device *d = pbw_dev;
	struct pbw_buffer *buf;
	unsigned long flags;

	/* need a queued (free) consumer buffer; otherwise drop the frame */
	spin_lock_irqsave(&d->list_lock, flags);
	if (list_empty(&d->free_list)) {
		spin_unlock_irqrestore(&d->list_lock, flags);
		d->frames_dropped++;
		return;
	}
	buf = list_first_entry(&d->free_list, struct pbw_buffer, list);
	list_del(&buf->list);
	buf->state = PBW_BUF_IDLE;
	spin_unlock_irqrestore(&d->list_lock, flags);

	pbw_convert_frame(d->acc, buf->vaddr, d->fmt_index);

	buf->sequence = (u32)++d->frames_done;
	buf->ts_ns = ktime_get_ns();

	spin_lock_irqsave(&d->list_lock, flags);
	buf->state = PBW_BUF_READY;
	list_add_tail(&buf->list, &d->ready_list);
	spin_unlock_irqrestore(&d->list_lock, flags);

	wake_up_interruptible(&d->ready_wq);
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static struct pbw_device *pbw_video_dev(struct file *file)
{
	struct video_device *vdev = video_devdata(file);

	return container_of(vdev, struct pbw_device, vdev);
}

static struct pbw_fh *pbw_fh(struct file *file)
{
	struct v4l2_fh *fh = file->private_data;

	return container_of(fh, struct pbw_fh, fh);
}

static inline const struct pbw_fmt *pbw_fmt_of(struct pbw_device *d)
{
	return &pbw_fmts[d->fmt_index];
}

/* reload buffers (must hold cfg_lock); returns number allocated */
static void pbw_free_buffers(struct pbw_device *d)
{
	unsigned int i;

	INIT_LIST_HEAD(&d->free_list);
	INIT_LIST_HEAD(&d->ready_list);
	d->nbufs = 0;
	d->streaming = false;

	for (i = 0; i < PBW_NBUF_MAX; i++) {
		struct pbw_buffer *b = &d->bufs[i];

		INIT_LIST_HEAD(&b->list);
		if (b->vaddr && atomic_read(&b->map_count) > 0)
			pr_warn_ratelimited(PBW_NAME ": freeing mapped buffer %u!\n", i);
		vfree(b->vaddr);
		b->vaddr = NULL;
		b->state = PBW_BUF_IDLE;
		atomic_set(&b->map_count, 0);
	}
	wake_up_interruptible_all(&d->ready_wq);
}

/* ------------------------------------------------------------------ */
/* file operations                                                    */
/* ------------------------------------------------------------------ */

static int pbw_open(struct file *file)
{
	struct pbw_device *d = pbw_video_dev(file);
	struct pbw_fh *pfh;

	pfh = kzalloc(sizeof(*pfh), GFP_KERNEL);
	if (!pfh)
		return -ENOMEM;

	if (file->f_mode & FMODE_WRITE) {
		if (atomic_xchg(&d->producer_claim, 1)) {
			kfree(pfh);
			return -EBUSY;
		}
		pfh->producer = true;
		d->acc_have = 0;
	} else {
		atomic_inc(&d->consumer_open);
	}

	v4l2_fh_init(&pfh->fh, &d->vdev);
	file->private_data = &pfh->fh;
	v4l2_fh_add(&pfh->fh);
	return 0;
}

static void pbw_flush_ready(struct pbw_device *d)
{
	unsigned long flags;

	spin_lock_irqsave(&d->list_lock, flags);
	while (!list_empty(&d->ready_list)) {
		struct pbw_buffer *b =
			list_first_entry(&d->ready_list, struct pbw_buffer, list);
		list_del(&b->list);
		b->state = PBW_BUF_FREE;
		list_add_tail(&b->list, &d->free_list);
	}
	d->streaming = false;
	spin_unlock_irqrestore(&d->list_lock, flags);
	wake_up_interruptible_all(&d->ready_wq);
}

static int pbw_release(struct file *file)
{
	struct pbw_device *d = pbw_video_dev(file);
	struct pbw_fh *pfh = pbw_fh(file);

	v4l2_fh_del(&pfh->fh);
	v4l2_fh_exit(&pfh->fh);

	if (pfh->producer) {
		atomic_xchg(&d->producer_claim, 0);
	} else if (atomic_dec_and_test(&d->consumer_open)) {
		/* last consumer gone: stop, hand buffers back */
		pbw_flush_ready(d);
	}

	kfree(pfh);
	return 0;
}

static ssize_t pbw_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct pbw_device *d = pbw_video_dev(file);
	struct pbw_fh *pfh = pbw_fh(file);
	size_t total = 0;

	if (!pfh->producer)
		return -EPERM;

	while (count > 0) {
		size_t space = PBW_FRAME_RAW - d->acc_have;
		size_t chunk = min_t(size_t, count, space);
		unsigned long not = copy_from_user(d->acc + d->acc_have,
						   ubuf, chunk);

		if (not) {
			/* partial copy: count the part that landed */
			size_t done = chunk - not;

			d->acc_have += done;
			total += done;
			return total ? (ssize_t)total : -EFAULT;
		}
		d->acc_have += chunk;
		total += chunk;
		count -= chunk;
		ubuf += chunk;

		if (d->acc_have == PBW_FRAME_RAW) {
			pbw_publish();
			d->acc_have = 0;
		}
	}
	return (ssize_t)total;
}

static __poll_t pbw_poll(struct file *file, poll_table *wait)
{
	struct pbw_device *d = pbw_video_dev(file);
	struct pbw_fh *pfh = pbw_fh(file);
	__poll_t mask = 0;
	unsigned long flags;

	if (pfh->producer)
		return EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &d->ready_wq, wait);

	spin_lock_irqsave(&d->list_lock, flags);
	if (!list_empty(&d->ready_list))
		mask = EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&d->list_lock, flags);
	return mask;
}

/* --- consumer mmap --- */

static void pbw_vm_open(struct vm_area_struct *vma)
{
	struct pbw_buffer *buf = vma->vm_private_data;

	atomic_inc(&buf->map_count);
}

static void pbw_vm_close(struct vm_area_struct *vma)
{
	struct pbw_buffer *buf = vma->vm_private_data;

	atomic_dec(&buf->map_count);
}

static const struct vm_operations_struct pbw_vm_ops = {
	.open = pbw_vm_open,
	.close = pbw_vm_close,
};

static int pbw_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct pbw_device *d = pbw_video_dev(file);
	struct pbw_fh *pfh = pbw_fh(file);
	struct pbw_buffer *buf;
	unsigned long index = vma->vm_pgoff;
	size_t size = PBW_FRAME_RGB;
	int ret;

	if (pfh->producer)
		return -EPERM;

	mutex_lock(&d->cfg_lock);
	if (index >= d->nbufs) {
		ret = -EINVAL;
		goto out;
	}
	size = pbw_fmt_of(d)->sizeimage;
	buf = &d->bufs[index];

	ret = remap_vmalloc_range(vma, buf->vaddr, 0);
	if (ret)
		goto out;

	vma->vm_private_data = buf;
	vma->vm_ops = &pbw_vm_ops;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	pbw_vm_open(vma);		/* account for this initial mapping */
out:
	mutex_unlock(&d->cfg_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* ioctls                                                             */
/* ------------------------------------------------------------------ */

static int pbw_vidioc_querycap(struct file *file, void *fh,
			       struct v4l2_capability *cap)
{
	strscpy(cap->driver, PBW_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Phoenix OV13850 preproc", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", PBW_NAME);
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int pbw_vidioc_enum_fmt(struct file *file, void *fh,
			       struct v4l2_fmtdesc *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (f->index >= PBW_NFMT)
		return -EINVAL;

	memset(f->reserved, 0, sizeof(f->reserved));
	f->flags = 0;
	f->mbus_code = 0;
	f->pixelformat = pbw_fmts[f->index].fourcc;
	strscpy(f->description, pbw_fmts[f->index].desc,
		sizeof(f->description));
	return 0;
}

static int pbw_vidioc_fmt_common(struct pbw_device *d,
				 struct v4l2_format *f)
{
	const struct pbw_fmt *fmt;
	int idx;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	for (idx = 0; idx < PBW_NFMT; idx++) {
		if (f->fmt.pix.pixelformat == pbw_fmts[idx].fourcc)
			break;
	}
	if (idx >= PBW_NFMT)
		return -EINVAL;

	d->fmt_index = idx;
	fmt = &pbw_fmts[idx];

	/* geometry is fixed; silently normalise whatever the app asked for */
	f->fmt.pix.width = PBW_W;
	f->fmt.pix.height = PBW_H;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = fmt->bytesperline;
	f->fmt.pix.sizeimage = fmt->sizeimage;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	f->fmt.pix.xfer_func = V4L2_XFER_FUNC_SRGB;
	f->fmt.pix.priv = 0;
	return 0;
}

static int pbw_vidioc_g_fmt(struct file *file, void *fh,
			    struct v4l2_format *f)
{
	struct pbw_device *d = pbw_video_dev(file);
	const struct pbw_fmt *fmt;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	fmt = pbw_fmt_of(d);
	memset(&f->fmt.pix, 0, sizeof(f->fmt.pix));
	f->fmt.pix.width = PBW_W;
	f->fmt.pix.height = PBW_H;
	f->fmt.pix.pixelformat = fmt->fourcc;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = fmt->bytesperline;
	f->fmt.pix.sizeimage = fmt->sizeimage;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	f->fmt.pix.xfer_func = V4L2_XFER_FUNC_SRGB;
	return 0;
}

static int pbw_vidioc_try_fmt(struct file *file, void *fh,
			      struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* try: don't apply, just validate+fill without touching state */
	{
		struct v4l2_format tmp = *f;
		int idx;

		for (idx = 0; idx < PBW_NFMT; idx++) {
			if (tmp.fmt.pix.pixelformat == pbw_fmts[idx].fourcc)
				break;
		}
		if (idx >= PBW_NFMT)
			return -EINVAL;

		tmp.fmt.pix.width = PBW_W;
		tmp.fmt.pix.height = PBW_H;
		tmp.fmt.pix.field = V4L2_FIELD_NONE;
		tmp.fmt.pix.bytesperline = pbw_fmts[idx].bytesperline;
		tmp.fmt.pix.sizeimage = pbw_fmts[idx].sizeimage;
		tmp.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
		*f = tmp;
	}
	return 0;
}

static int pbw_vidioc_s_fmt(struct file *file, void *fh,
			    struct v4l2_format *f)
{
	struct pbw_device *d = pbw_video_dev(file);
	int ret;

	mutex_lock(&d->cfg_lock);
	/* if buffers are allocated, format is locked (V4L2 semantics) */
	if (d->nbufs && d->bufs[0].vaddr &&
	    pbw_fmts[d->fmt_index].fourcc != f->fmt.pix.pixelformat) {
		mutex_unlock(&d->cfg_lock);
		return -EBUSY;
	}
	ret = pbw_vidioc_fmt_common(d, f);
	mutex_unlock(&d->cfg_lock);
	return ret;
}

static int pbw_vidioc_g_parm(struct file *file, void *fh,
			     struct v4l2_streamparm *a)
{
	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(&a->parm, 0, sizeof(a->parm));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe.numerator = 1;
	a->parm.capture.timeperframe.denominator = 12; /* measured ~12 fps */
	return 0;
}

static int pbw_vidioc_s_parm(struct file *file, void *fh,
			     struct v4l2_streamparm *a)
{
	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* pacing is driven by the producer; accept and echo */
	if (a->parm.capture.timeperframe.denominator < 1)
		a->parm.capture.timeperframe.denominator = 12;
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	return 0;
}

static int pbw_vidioc_reqbufs(struct file *file, void *fh,
			      struct v4l2_requestbuffers *b)
{
	struct pbw_device *d = pbw_video_dev(file);
	unsigned int i, count;
	int ret = 0;
	bool mapped = false;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (b->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	mutex_lock(&d->cfg_lock);
	count = b->count;

	if (count == 0) {
		if (d->nbufs)
			pbw_free_buffers(d);
		b->count = 0;
		goto out;
	}
	if (count > PBW_NBUF_MAX)
		count = PBW_NBUF_MAX;

	if (d->nbufs == count && d->bufs[0].vaddr) {
		b->count = count;	/* idempotent */
		goto out;
	}

	for (i = 0; i < PBW_NBUF_MAX; i++) {
		if (atomic_read(&d->bufs[i].map_count) > 0)
			mapped = true;
	}
	if (mapped && d->nbufs) {
		ret = -EBUSY;
		goto out;
	}

	pbw_free_buffers(d);
	for (i = 0; i < count; i++) {
		struct pbw_buffer *buf = &d->bufs[i];

		buf->vaddr = vmalloc(pbw_fmt_of(d)->sizeimage);
		if (!buf->vaddr) {
			pbw_free_buffers(d);
			ret = -ENOMEM;
			goto out;
		}
		buf->index = i;
		buf->state = PBW_BUF_IDLE;
		INIT_LIST_HEAD(&buf->list);
		atomic_set(&buf->map_count, 0);
		buf->sequence = 0;
		buf->ts_ns = 0;
	}
	d->nbufs = count;
	b->count = count;
out:
	mutex_unlock(&d->cfg_lock);
	return ret;
}

static int pbw_vidioc_querybuf(struct file *file, void *fh,
			       struct v4l2_buffer *b)
{
	struct pbw_device *d = pbw_video_dev(file);
	unsigned int index;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    b->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;
	if (b->index >= d->nbufs)
		return -EINVAL;

	index = b->index;
	memset(b, 0, sizeof(*b));
	b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b->memory = V4L2_MEMORY_MMAP;
	b->index = index;
	b->length = pbw_fmt_of(d)->sizeimage;
	/* userspace mmap()s with this offset; kernel sees vm_pgoff = index */
	b->m.offset = index * PAGE_SIZE;
	b->flags = 0;
	return 0;
}

static int pbw_vidioc_qbuf(struct file *file, void *fh,
			   struct v4l2_buffer *b)
{
	struct pbw_device *d = pbw_video_dev(file);
	struct pbw_buffer *buf;
	unsigned long flags;
	int ret = 0;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    b->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	mutex_lock(&d->cfg_lock);
	if (b->index >= d->nbufs) {
		ret = -EINVAL;
		goto out;
	}
	buf = &d->bufs[b->index];

	spin_lock_irqsave(&d->list_lock, flags);
	if (buf->state == PBW_BUF_IDLE) {
		buf->state = PBW_BUF_FREE;
		list_add_tail(&buf->list, &d->free_list);
	}
	/* already queued buffers stay where they are (idempotent) */
	spin_unlock_irqrestore(&d->list_lock, flags);

	memset(b, 0, sizeof(*b));
	b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b->memory = V4L2_MEMORY_MMAP;
	b->index = buf->index;
out:
	mutex_unlock(&d->cfg_lock);
	return ret;
}

static int pbw_vidioc_dqbuf(struct file *file, void *fh,
			    struct v4l2_buffer *b)
{
	struct pbw_device *d = pbw_video_dev(file);
	struct pbw_buffer *buf;
	unsigned long flags;
	int ret = 0;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    b->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	if (!(file->f_flags & O_NONBLOCK)) {
		for (;;) {
			int r;
			r = wait_event_interruptible(d->ready_wq,
				!READ_ONCE(d->streaming) ||
				!list_empty_careful(&d->ready_list));
			if (r)
				return r;
			if (!d->streaming)
				return -EINVAL;
			spin_lock_irqsave(&d->list_lock, flags);
			if (!list_empty(&d->ready_list))
				break;
			spin_unlock_irqrestore(&d->list_lock, flags);
		}
	} else {
		spin_lock_irqsave(&d->list_lock, flags);
		if (list_empty(&d->ready_list)) {
			spin_unlock_irqrestore(&d->list_lock, flags);
			return -EAGAIN;
		}
	}
	/* holding list_lock; a buffer is guaranteed on the front */
	buf = list_first_entry(&d->ready_list, struct pbw_buffer, list);
	list_del(&buf->list);
	buf->state = PBW_BUF_IDLE;
	spin_unlock_irqrestore(&d->list_lock, flags);

	memset(b, 0, sizeof(*b));
	b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b->memory = V4L2_MEMORY_MMAP;
	b->index = buf->index;
	b->bytesused = pbw_fmt_of(d)->sizeimage;
	b->sequence = buf->sequence;
	b->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	b->field = V4L2_FIELD_NONE;
	{
		struct timespec64 ts = ns_to_timespec64(buf->ts_ns);

		b->timestamp.tv_sec = (long)ts.tv_sec;
		b->timestamp.tv_usec = ts.tv_nsec / 1000;
	}
	return ret;
}

static int pbw_vidioc_streamon(struct file *file, void *fh,
			       enum v4l2_buf_type type)
{
	struct pbw_device *d = pbw_video_dev(file);
	unsigned long flags;
	int ret = 0;

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	mutex_lock(&d->cfg_lock);
	if (!d->nbufs) {
		ret = -EINVAL;
		goto out;
	}
	spin_lock_irqsave(&d->list_lock, flags);
	d->streaming = true;
	spin_unlock_irqrestore(&d->list_lock, flags);
	wake_up_interruptible(&d->ready_wq);
out:
	mutex_unlock(&d->cfg_lock);
	return ret;
}

static int pbw_vidioc_streamoff(struct file *file, void *fh,
				enum v4l2_buf_type type)
{
	struct pbw_device *d = pbw_video_dev(file);

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	mutex_lock(&d->cfg_lock);
	pbw_flush_ready(d);
	mutex_unlock(&d->cfg_lock);
	wake_up_interruptible_all(&d->ready_wq);
	return 0;
}

static const struct v4l2_ioctl_ops pbw_ioctl_ops = {
	.vidioc_querycap	= pbw_vidioc_querycap,
	.vidioc_enum_fmt_vid_cap = pbw_vidioc_enum_fmt,
	.vidioc_g_fmt_vid_cap	= pbw_vidioc_g_fmt,
	.vidioc_try_fmt_vid_cap	= pbw_vidioc_try_fmt,
	.vidioc_s_fmt_vid_cap	= pbw_vidioc_s_fmt,
	.vidioc_g_parm		= pbw_vidioc_g_parm,
	.vidioc_s_parm		= pbw_vidioc_s_parm,
	.vidioc_reqbufs		= pbw_vidioc_reqbufs,
	.vidioc_querybuf	= pbw_vidioc_querybuf,
	.vidioc_qbuf		= pbw_vidioc_qbuf,
	.vidioc_dqbuf		= pbw_vidioc_dqbuf,
	.vidioc_streamon	= pbw_vidioc_streamon,
	.vidioc_streamoff	= pbw_vidioc_streamoff,
};

static const struct v4l2_file_operations pbw_fops = {
	.owner		= THIS_MODULE,
	.open		= pbw_open,
	.release	= pbw_release,
	.write		= pbw_write,
	.poll		= pbw_poll,
	.mmap		= pbw_mmap,
	.unlocked_ioctl	= video_ioctl2,
	.compat_ioctl32	= video_ioctl2,
};

/* ------------------------------------------------------------------ */
/* init/exit                                                          */
/* ------------------------------------------------------------------ */

static int pbw_register(void)
{
	struct pbw_device *d;
	int ret;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	pbw_dev = d;

	mutex_init(&d->cfg_lock);
	spin_lock_init(&d->list_lock);
	init_waitqueue_head(&d->ready_wq);
	INIT_LIST_HEAD(&d->free_list);
	INIT_LIST_HEAD(&d->ready_list);
	d->fmt_index = 0;

	/* per-frame scratch space */
	d->raw16 = vmalloc(PBW_FRAME_S16);
	d->pr = vmalloc(PBW_FRAME_S16);
	d->pg = vmalloc(PBW_FRAME_S16);
	d->pb = vmalloc(PBW_FRAME_S16);
	d->acc = vmalloc(PBW_FRAME_RAW);
	if (!d->raw16 || !d->pr || !d->pg || !d->pb || !d->acc) {
		ret = -ENOMEM;
		goto err_mem;
	}

	ret = v4l2_device_register(NULL, &d->v4l2_dev);
	if (ret) {
		pr_err(PBW_NAME ": v4l2_device_register failed %d\n", ret);
		goto err_mem;
	}
	strscpy(d->vdev.name, PBW_NAME, sizeof(d->vdev.name));
	d->vdev.v4l2_dev = &d->v4l2_dev;
	d->vdev.fops = &pbw_fops;
	d->vdev.ioctl_ops = &pbw_ioctl_ops;
	d->vdev.release = video_device_release_empty;
	d->vdev.lock = &d->cfg_lock;
	d->vdev.vfl_dir = VFL_DIR_RX;
	d->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(&d->vdev, d);

	ret = video_register_device(&d->vdev, VFL_TYPE_VIDEO,
				    pbw_dev_minor == -1U ? -1 : (int)pbw_dev_minor);
	if (ret) {
		pr_err(PBW_NAME ": video_register_device failed %d\n", ret);
		goto err_vdev;
	}

	pr_info(PBW_NAME ": registered /dev/video%d (%s -> %s, %zu B/frame)\n",
		d->vdev.minor, "pBAA RAW10 packed", pbw_fmts[d->fmt_index].desc,
		pbw_fmts[d->fmt_index].sizeimage);
	pr_info(PBW_NAME ": wb_r=%d.%03d wb_b=%d.%03d bl=%d\n",
		pbw_wb_r_256 / 256, (pbw_wb_r_256 % 256) * 250 / 256,
		pbw_wb_b_256 / 256, (pbw_wb_b_256 % 256) * 250 / 256,
		pbw_black_level);
	return 0;

err_vdev:
	v4l2_device_unregister(&d->v4l2_dev);
err_mem:
	vfree(d->raw16); vfree(d->pr); vfree(d->pg); vfree(d->pb); vfree(d->acc);
	kfree(d);
	pbw_dev = NULL;
	return ret;
}

static void pbw_unregister(void)
{
	struct pbw_device *d = pbw_dev;
	unsigned long done, dropped;

	if (!d)
		return;

	video_unregister_device(&d->vdev);
	v4l2_device_unregister(&d->v4l2_dev);
	pbw_free_buffers(d);
	vfree(d->raw16); vfree(d->pr); vfree(d->pg); vfree(d->pb); vfree(d->acc);
	done = d->frames_done;
	dropped = d->frames_dropped;
	kfree(d);
	pbw_dev = NULL;
	pr_info(PBW_NAME ": unregistered (%lu frames done, %lu dropped)\n",
		done, dropped);
}

module_init(pbw_register);
module_exit(pbw_unregister);

MODULE_AUTHOR("phoenix-pmos-port");
MODULE_DESCRIPTION("OV13850 pBAA (MIPI RAW10) pre-processing V4L2 wrapper");
MODULE_LICENSE("GPL v2");
