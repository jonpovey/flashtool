/*
 * Generate ECC in software for legacy NAND layout
 *
 * Copyright (C) 2011 Racelogic Limited
 * Written by Jon Povey <jon.povey@racelogic.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <string.h>

#include "debug.h"
#include "genecc.h"

const int subsz_raw = 512 + 16;
const int subsz_data = 512;
const int pagesz_data = 2048;

// hardcode for the only sizing we care about, for now
u8 mtd_raw_buf[2048 + 64];

/*
 * Reed-Solomon ECC code reverse-engineered from TI PSP flash_utils genecc
 */

#define MAX_CORR_ERR	4
#define S				MAX_CORR_ERR
#define K				512
#define N				(K + 2 * MAX_CORR_ERR)
#define LENGTH			(1 << 10)

typedef signed int	bgfe;	// BinaryGaloisFieldElement

const bgfe poly = 0x409;
const bgfe primelement = 2;

bgfe gp[2 * MAX_CORR_ERR + 1];	// generator poly
bgfe alpha[LENGTH];				// 4KB
s32  indx[LENGTH];				// 4KB

bgfe alphafromindex(int i)
{
	return alpha[i % (LENGTH - 1)];
}

s32 indexfromalpha(bgfe alpha)
{
	if (alpha == 0 || alpha >= LENGTH)
		ERR("BUG\n");
	return indx[alpha];
}

/* fudged up version of linux asm-generic/bitops/fls.h */
static inline s32 order(bgfe x)
{
	int r = 31;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		x <<= 1;
		r -= 1;
	}

	return r > 0 ? r : 0;
}

bgfe modulo(bgfe x, bgfe y)
{
	s32 ordx, ordy;

	ordx = order(x);
	ordy = order(y);

	while (ordx >= ordy) {
		if (x & (1 << ordx))
			x ^= y << (ordx - ordy);	// bgfe -= is a ^=
		ordx--;
	}
	return x;
}

bgfe multiply(bgfe x, bgfe y)
{
	int i;
	bgfe temp = 0;
	u32 mask = 1;

	for (i = 0; i < 16; i++) {
		if (x & mask)
			temp ^= y << i;				// bgfe += is a ^=
		mask <<= 1;
	}
	return modulo(temp, poly);
}

void genecc_init(void)
{
	int i, j;

	alpha[0] = 1;
	indx[0] = 1;

	for (i = 1; i < LENGTH; i++) {
		alpha[i] = multiply(alpha[i - 1], primelement);
		indx[alpha[i]] = i;
	}

	// create generator poly
	gp[0] = 1;
	for (i = 1; i <= (2 * MAX_CORR_ERR); i++) {
		gp[i] = 1;
		for (j = i - 1; j > 0; j--) {
			if (gp[j])
				gp[j] = gp[j - 1] ^ multiply(alphafromindex(i), gp[j]);
			else
				gp[j] = gp[j - 1];
		}
		gp[0] = alphafromindex((i * (i + 1)) / 2);
	}
}

void gen_subpage_ecc(const u8 *buf, u8 *ecc)
{
	bgfe data[N], *p;
	int i, j;
	u8 *e;

	// things break if data is not cleared. Loop sets the rest.
	memset(data, 0, 2 * S * sizeof(bgfe));

	for (i = 0; i < K; i++)
		data[i + (2 * S)] = buf[(K - 1) - i];

	// long division!
	for (i = N - 1; i >= (2 * S); i--) {
		if (data[i]) {
			for (j = 1; j <= (2 * S); j++) {
				data[i - j] = data[i - j] ^ multiply(data[i], gp[2 * S - j]);
			}
			data[i] = 0;
		}
	}

	// first 2*s (8) elements of data[] contain our parity
	// pack as 2 sets of 5*8 bits (NAND stored format)
	for (e = ecc, p = data; e < ecc + 10; p += 4) {
		*e++ =   p[0]       & 0xff;
		*e++ = ((p[0] >> 8) & 0x03) | ((p[1] << 2) & 0xfc);
		*e++ = ((p[1] >> 6) & 0x0f) | ((p[2] << 4) & 0xf0);
		*e++ = ((p[2] >> 4) & 0x3f) | ((p[3] << 6) & 0xc0);
		*e++ =  (p[3] >> 2) & 0xff;
	}
}

unsigned char *do_genecc(const u8 *src, int layout)
{
	int n;
	unsigned char *raw_subpage, *oob;

	switch (layout) {
	case GENECC_LAYOUT_LEGACY:
		for (n = 0; n < 4; n++) {
			raw_subpage = &mtd_raw_buf[subsz_raw * n];
			oob = raw_subpage + subsz_data;

			// copy data to correct locations in raw page
			memcpy(raw_subpage, &src[subsz_data * n], subsz_data);

			/*
			 * RBL ignores ECC so we shouldn't HAVE to generate any - but let's.
			 * legacy OOB format is 6 bytes spare, 10 bytes ECC
			 */
			memset(oob, 0xff, 6);
			gen_subpage_ecc(raw_subpage, oob + 6);
		}
		break;
	case GENECC_LAYOUT_DM365_RBL:
		/*
		 * This layout is all data where it should be, in the first 2KB of the
		 * page, but ECC is laid out in the OOB in units of 6 FF, 10 ECC per
		 * subpage, instead of all 40 bytes of ECC being at the end of OOB.
		 */
		memcpy(mtd_raw_buf, src, pagesz_data);
		
		oob = mtd_raw_buf + pagesz_data;
		memset(oob, 0xff, 16 * 4);
	
		for (n = 0; n < 4; n++) {
			raw_subpage = &mtd_raw_buf[subsz_data * n];
			gen_subpage_ecc(raw_subpage, oob + (n * 16) + 6);
		}
		break;
	default:
		ERR("BUG: bad layout value %d\n", layout);
	}
	return mtd_raw_buf;
}
