/*
 * Copyright (c) 2015, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "gk104.h"

static int
gm20b_fifo_init(struct nvkm_object *object)
{
	struct gk104_fifo_priv *priv = (void *)object;
	int i, ret;

	ret = gk104_fifo_init(object);
	if (ret)
		return ret;

	/* set fb timeout period to max */
	nv_mask(priv, 0x002a04, 0x3fffffff, 0x3fffffff);

	/* set pbdma timeout period */
	for (i = 0; i < priv->spoon_nr; i++)
		nv_wr32(priv, 0x04012c + i * 0x2000, 0xffffffff);

	return 0;
}

struct nvkm_oclass *
gm20b_fifo_oclass = &(struct gk104_fifo_impl) {
	.base.handle = NV_ENGINE(FIFO, 0x2b),
	.base.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = gm204_fifo_ctor,
		.dtor = gk104_fifo_dtor,
		.init = gm20b_fifo_init,
		.fini = gk104_fifo_fini,
	},
	.channels = 512,
	.engine = gk20a_fifo_engines,
	.num_engine = ARRAY_SIZE(gk20a_fifo_engines),
}.base;
