/******************************************************************************
*
 *
 * Filename:
 * ---------
 *    mt_soc_pcm_common
 *
 * Project:
 * --------
 *     mt_soc_pcm_common function
 *
 *
 * Description:
 * ------------
 *   common function
 *
 * Author:
 * -------
 *   Chipeng Chang (MTK02308)
 *
 *---------------------------------------------------------------------------
---
 *

*******************************************************************************/

#include "mt_soc_pcm_common.h"

unsigned long audio_frame_to_bytes(struct snd_pcm_substream *substream, unsigned long count)
{
	unsigned long bytes = count;
	struct snd_pcm_runtime *runtime = substream->runtime;
	if (runtime->format == SNDRV_PCM_FORMAT_S32_LE || runtime->format == SNDRV_PCM_FORMAT_U32_LE)
		bytes = bytes << 2;
	else
		bytes = bytes << 1;

	if (runtime->channels == 2)
		bytes = bytes << 1;
	/* pr_debug("%s bytes = %d count = %d\n",__func__,bytes,count); */
	return bytes;
}


unsigned long audio_bytes_to_frame(struct snd_pcm_substream *substream, unsigned long bytes)
{
	unsigned long count  = bytes;
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (runtime->format == SNDRV_PCM_FORMAT_S32_LE || runtime->format == SNDRV_PCM_FORMAT_U32_LE)
		count = count >> 2;
	else
		count = count >> 1;


	if (runtime->channels == 2)
		count = count >> 1;
	/* pr_debug("%s bytes = %d count = %d\n",__func__,bytes,count); */
	return count;
}

/* a59 port fix: see apply-patches2.py. copy_{from,to}_user() into or out of the
 * AFE's internal SRAM is unsafe -- that buffer is ioremap'd Device memory, and
 * those helpers assume normal memory, so an unaligned ring offset aborts and
 * panics the kernel. memcpy_toio/memcpy_fromio do handle unaligned head and
 * tail, so bounce through a kernel buffer and use them for the MMIO half.
 *
 * Uniform for both SRAM and DRAM destinations: the driver does not tell us which
 * it picked at this point, and memcpy_toio to normal memory is simply a slower
 * but correct write. One 4 KB buffer under a mutex is ample -- these run in
 * process context from the PCM copy op, never from the interrupt path, and a
 * period is a few KB.
 */
const char a59_audio_fix_marker[] = "a59-audio-fix-6755-v1";
EXPORT_SYMBOL(a59_audio_fix_marker);

#define A59_BOUNCE 4096
static DEFINE_MUTEX(a59_bounce_lock);
static char a59_bounce_buf[A59_BOUNCE];

unsigned long a59_copy_from_user_io(void *dst, const void __user *src, unsigned long n)
{
	char *d = (char *)dst;
	const char __user *s = (const char __user *)src;

	mutex_lock(&a59_bounce_lock);
	while (n) {
		unsigned long k = (n > A59_BOUNCE) ? A59_BOUNCE : n;

		if (copy_from_user(a59_bounce_buf, s, k)) {
			mutex_unlock(&a59_bounce_lock);
			return n;
		}
		memcpy_toio((void __iomem *)d, a59_bounce_buf, k);
		d += k;
		s += k;
		n -= k;
	}
	mutex_unlock(&a59_bounce_lock);
	return 0;
}
EXPORT_SYMBOL(a59_copy_from_user_io);

unsigned long a59_copy_to_user_io(void __user *dst, const void *src, unsigned long n)
{
	char __user *d = (char __user *)dst;
	const char *s = (const char *)src;

	mutex_lock(&a59_bounce_lock);
	while (n) {
		unsigned long k = (n > A59_BOUNCE) ? A59_BOUNCE : n;

		memcpy_fromio(a59_bounce_buf, (const void __iomem *)s, k);
		if (copy_to_user(d, a59_bounce_buf, k)) {
			mutex_unlock(&a59_bounce_lock);
			return n;
		}
		d += k;
		s += k;
		n -= k;
	}
	mutex_unlock(&a59_bounce_lock);
	return 0;
}
EXPORT_SYMBOL(a59_copy_to_user_io);
