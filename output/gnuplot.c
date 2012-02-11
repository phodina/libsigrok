/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "config.h"
#include "sigrok.h"
#include "sigrok-internal.h"

struct context {
	unsigned int num_enabled_probes;
	unsigned int unitsize;
	char *probelist[SR_MAX_NUM_PROBES + 1];
	char *header;
};

#define MAX_HEADER_LEN \
	(1024 + (SR_MAX_NUM_PROBES * (SR_MAX_PROBENAME_LEN + 10)))

static const char *gnuplot_header = "\
# Sample data in space-separated columns format usable by gnuplot\n\
#\n\
# Generated by: %s on %s%s\
# Period: %s\n\
#\n\
# Column\tProbe\n\
# -------------------------------------\
----------------------------------------\n\
# 0\t\tSample counter (for internal gnuplot purposes)\n%s\n";

static const char *gnuplot_header_comment = "\
# Comment: Acquisition with %d/%d probes at %s\n";

static int init(struct sr_output *o)
{
	struct context *ctx;
	struct sr_probe *probe;
	GSList *l;
	uint64_t samplerate;
	unsigned int i;
	int b, num_probes;
	char *c, *frequency_s;
	char wbuf[1000], comment[128];
	time_t t;

	if (!o) {
		sr_err("gnuplot out: %s: o was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!o->device) {
		sr_err("gnuplot out: %s: o->device was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!o->device->plugin) {
		sr_err("gnuplot out: %s: o->device->plugin was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!(ctx = g_try_malloc0(sizeof(struct context)))) {
		sr_err("gnuplot out: %s: ctx malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	if (!(ctx->header = g_try_malloc0(MAX_HEADER_LEN + 1))) {
		sr_err("gnuplot out: %s: ctx->header malloc failed", __func__);
		g_free(ctx);
		return SR_ERR_MALLOC;
	}

	o->internal = ctx;
	ctx->num_enabled_probes = 0;
	for (l = o->device->probes; l; l = l->next) {
		probe = l->data; /* TODO: Error checks. */
		if (!probe->enabled)
			continue;
		ctx->probelist[ctx->num_enabled_probes++] = probe->name;
	}
	ctx->probelist[ctx->num_enabled_probes] = 0;
	ctx->unitsize = (ctx->num_enabled_probes + 7) / 8;

	num_probes = g_slist_length(o->device->probes);
	comment[0] = '\0';
	if (sr_device_has_hwcap(o->device, SR_HWCAP_SAMPLERATE)) {
		samplerate = *((uint64_t *) o->device->plugin->get_device_info(
				o->device->plugin_index, SR_DI_CUR_SAMPLERATE));
		if (!(frequency_s = sr_samplerate_string(samplerate))) {
			sr_err("gnuplot out: %s: sr_samplerate_string failed",
			       __func__);
			g_free(ctx->header);
			g_free(ctx);
			return SR_ERR;
		}
		snprintf(comment, 127, gnuplot_header_comment,
			ctx->num_enabled_probes, num_probes, frequency_s);
		g_free(frequency_s);
	}

	/* Columns / channels */
	wbuf[0] = '\0';
	for (i = 0; i < ctx->num_enabled_probes; i++) {
		c = (char *)&wbuf + strlen((char *)&wbuf);
		sprintf(c, "# %d\t\t%s\n", i + 1, ctx->probelist[i]);
	}

	if (!(frequency_s = sr_period_string(samplerate))) {
		sr_err("gnuplot out: %s: sr_period_string failed", __func__);
		g_free(ctx->header);
		g_free(ctx);
		return SR_ERR;
	}

	t = time(NULL);
	b = snprintf(ctx->header, MAX_HEADER_LEN, gnuplot_header,
		     PACKAGE_STRING, ctime(&t), comment, frequency_s,
		     (char *)&wbuf);
	g_free(frequency_s);

	if (b < 0) {
		sr_err("gnuplot out: %s: sprintf failed", __func__);
		g_free(ctx->header);
		g_free(ctx);
		return SR_ERR;
	}

	return 0;
}

static int event(struct sr_output *o, int event_type, char **data_out,
		 uint64_t *length_out)
{
	struct context *ctx;

	if (!o) {
		sr_err("gnuplot out: %s: o was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!data_out) {
		sr_err("gnuplot out: %s: data_out was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!length_out) {
		sr_err("gnuplot out: %s: length_out was NULL", __func__);
		return SR_ERR_ARG;
	}

	ctx = o->internal;

	switch (event_type) {
	case SR_DF_TRIGGER:
		/* TODO: Can a trigger mark be in a gnuplot data file? */
		break;
	case SR_DF_END:
		g_free(o->internal);
		o->internal = NULL;
		break;
	default:
		sr_err("gnuplot out: %s: unsupported event type: %d",
		       __func__, event_type);
		break;
	}

	*data_out = NULL;
	*length_out = 0;

	return SR_OK;
}

static int data(struct sr_output *o, const char *data_in, uint64_t length_in,
		char **data_out, uint64_t *length_out)
{
	struct context *ctx;
	unsigned int max_linelen, outsize, p, curbit, i;
	uint64_t sample;
	static uint64_t samplecount = 0, old_sample = 0;
	char *outbuf, *c;

	if (!o) {
		sr_err("gnuplot out: %s: o was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!o->internal) {
		sr_err("gnuplot out: %s: o->internal was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!data_in) {
		sr_err("gnuplot out: %s: data_in was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!data_out) {
		sr_err("gnuplot out: %s: data_out was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!length_out) {
		sr_err("gnuplot out: %s: length_out was NULL", __func__);
		return SR_ERR_ARG;
	}

	ctx = o->internal;
	max_linelen = 16 + ctx->num_enabled_probes * 2;
	outsize = length_in / ctx->unitsize * max_linelen;
	if (ctx->header)
		outsize += strlen(ctx->header);

	if (!(outbuf = g_try_malloc0(outsize))) {
		sr_err("gnuplot out: %s: outbuf malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	outbuf[0] = '\0';
	if (ctx->header) {
		/* The header is still here, this must be the first packet. */
		strncpy(outbuf, ctx->header, outsize);
		g_free(ctx->header);
		ctx->header = NULL;
	}

	for (i = 0; i <= length_in - ctx->unitsize; i += ctx->unitsize) {

		memcpy(&sample, data_in + i, ctx->unitsize);

		/*
		 * Don't output the same samples multiple times. However, make
		 * sure to output at least the first and last sample.
		 */
		if (samplecount++ != 0 && sample == old_sample) {
			if (i != (length_in - ctx->unitsize))
				continue;
		}
		old_sample = sample;

		/* The first column is a counter (needed for gnuplot). */
		c = outbuf + strlen(outbuf);
		sprintf(c, "%" PRIu64 "\t", samplecount++);

		/* The next columns are the values of all channels. */
		for (p = 0; p < ctx->num_enabled_probes; p++) {
			curbit = (sample & ((uint64_t) (1 << p))) >> p;
			c = outbuf + strlen(outbuf);
			sprintf(c, "%d ", curbit);
		}

		c = outbuf + strlen(outbuf);
		sprintf(c, "\n");
	}

	*data_out = outbuf;
	*length_out = strlen(outbuf);

	return SR_OK;
}

SR_PRIV struct sr_output_format output_gnuplot = {
	.id = "gnuplot",
	.description = "Gnuplot",
	.df_type = SR_DF_LOGIC,
	.init = init,
	.data = data,
	.event = event,
};

/* Temporarily disabled. */
#if 0
static int analog_init(struct sr_output *o)
{
	struct context *ctx;
	struct sr_probe *probe;
	GSList *l;
	uint64_t samplerate;
	unsigned int i;
	int b, num_probes;
	char *c, *frequency_s;
	char wbuf[1000], comment[128];
	time_t t;

	if (!(ctx = g_try_malloc0(sizeof(struct context)))) {
		sr_err("gnuplot out: %s: ctx malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	if (!(ctx->header = g_try_malloc0(MAX_HEADER_LEN + 1))) {
		g_free(ctx);
		sr_err("gnuplot out: %s: ctx->header malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	o->internal = ctx;
	ctx->num_enabled_probes = 0;
	for (l = o->device->probes; l; l = l->next) {
		probe = l->data;
		if (!probe->enabled)
			continue;
		ctx->probelist[ctx->num_enabled_probes++] = probe->name;
	}
	ctx->probelist[ctx->num_enabled_probes] = 0;
//	ctx->unitsize = (ctx->num_enabled_probes + 7) / 8;
	ctx->unitsize = sizeof(struct sr_analog_sample) +
			(ctx->num_enabled_probes * sizeof(struct sr_analog_probe));

	num_probes = g_slist_length(o->device->probes);
	comment[0] = '\0';
	if (o->device->plugin && sr_device_has_hwcap(o->device, SR_HWCAP_SAMPLERATE)) {
		samplerate = *((uint64_t *) o->device->plugin->get_device_info(
				o->device->plugin_index, SR_DI_CUR_SAMPLERATE));
		if (!(frequency_s = sr_samplerate_string(samplerate))) {
			g_free(ctx->header);
			g_free(ctx);
			return SR_ERR;
		}
		snprintf(comment, 127, gnuplot_header_comment,
			ctx->num_enabled_probes, num_probes, frequency_s);
		g_free(frequency_s);
	}

	/* Columns / channels */
	wbuf[0] = '\0';
	for (i = 0; i < ctx->num_enabled_probes; i++) {
		c = (char *)&wbuf + strlen((char *)&wbuf);
		sprintf(c, "# %d\t\t%s\n", i + 1, ctx->probelist[i]);
	}

	if (!(frequency_s = sr_period_string(samplerate))) {
		g_free(ctx->header);
		g_free(ctx);
		return SR_ERR;
	}
	t = time(NULL);
	b = snprintf(ctx->header, MAX_HEADER_LEN, gnuplot_header,
		     PACKAGE_STRING, ctime(&t), comment, frequency_s,
		     (char *)&wbuf);
	g_free(frequency_s);

	if (b < 0) {
		g_free(ctx->header);
		g_free(ctx);
		return SR_ERR;
	}

	return 0;
}

static int analog_data(struct sr_output *o, char *data_in, uint64_t length_in,
		char **data_out, uint64_t *length_out)
{
	struct context *ctx;
	unsigned int max_linelen, outsize, p, /* curbit, */ i;
//	uint64_t sample;
	static uint64_t samplecount = 0;
	char *outbuf, *c;
	struct sr_analog_sample *sample;

	ctx = o->internal;
//	max_linelen = 16 + ctx->num_enabled_probes * 2;
	max_linelen = 16 + ctx->num_enabled_probes * 30;
	outsize = length_in / ctx->unitsize * max_linelen;
	if (ctx->header)
		outsize += strlen(ctx->header);

	if (!(outbuf = g_try_malloc0(outsize))) {
		sr_err("gnuplot out: %s: outbuf malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	outbuf[0] = '\0';
	if (ctx->header) {
		/* The header is still here, this must be the first packet. */
		strncpy(outbuf, ctx->header, outsize);
		g_free(ctx->header);
		ctx->header = NULL;
	}

	for (i = 0; i <= length_in - ctx->unitsize; i += ctx->unitsize) {
//		memcpy(&sample, data_in + i, ctx->unitsize);
		sample = (struct sr_analog_sample *) (data_in + i);

		/* The first column is a counter (needed for gnuplot). */
		c = outbuf + strlen(outbuf);
		sprintf(c, "%" PRIu64 "\t", samplecount++);

		/* The next columns are the values of all channels. */
		for (p = 0; p < ctx->num_enabled_probes; p++) {
//			curbit = (sample & ((uint64_t) (1 << p))) >> p;
			c = outbuf + strlen(outbuf);
//			sprintf(c, "%d ", curbit);
			/*
			 * FIXME: Should be doing proper raw->voltage conversion
			 * here, casting to int16_t isn't it. Remember that if
			 * res = 1 conversion isn't necessary.
			 */
			sprintf(c, "%f ", (double) ((int16_t) (sample->probes[p].val &
					((1 << sample->probes[p].res) - 1))));
		}

		c = outbuf + strlen(outbuf);
		sprintf(c, "\n");
	}

	*data_out = outbuf;
	*length_out = strlen(outbuf);

	return SR_OK;
}

struct sr_output_format output_analog_gnuplot = {
	.id = "analog_gnuplot",
	.description = "Gnuplot analog",
	.df_type = SR_DF_ANALOG,
	.init = analog_init,
	.data = analog_data,
	.event = event,
};
#endif
