/* ac3strip - strip dynamic range info from an A/52 stream */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include "ac3bits.h"
//#include "bitreader.h"

static int debug = 1;

struct bitreader {
	FILE *f;
	int nread; // number of bytes read
	int err;

	uint64_t res; // bit reservoir
	int n; // number of available bits
};

struct bitwriter {
	uint8_t *buf;
	int len;
	int cap;
	int err;

	uint64_t res;
	int n;
};

int      getnbitsread(struct bitreader*);
uint64_t readbits(struct bitreader*, int);
void     writebits(struct bitwriter*, uint64_t, int);
void     grow(struct bitwriter*, int);

// Get the total number of bits read.
int
getnbitsread(struct bitreader *br) {
	return br->nread * 8 - br->n;
}

// Read n bits from br.
uint64_t
readbits(struct bitreader *br, int n)
{
	uint64_t ret;
	int c;
	while (br->n < n) {
		c = getc(br->f);
		if (c < 0) {
			br->err = errno;
			return 0;
		}
		br->res <<= 8;
		br->res |= (uint64_t)c;
		br->n += 8;
		br->nread += 1;
	}
	ret = br->res >> (br->n - n);
	ret &= ((uint64_t)1 << n) - (uint64_t)1;
	br->n -= n;
	return ret;
}

// Write n bits to bw.
void
writebits(struct bitwriter *bw, uint64_t bits, int n)
{
	if (n <= 0) {
		return;
	}
	if (n < 64) {
		bits &= (1ULL<<n)-1ULL;
	}
	assert(n <= 56);
	bw->res <<= n;
	bw->res |= bits;
	bw->n += n;
	grow(bw, bw->len + bw->n/8);
	if (bw->err) {
		return;
	}
	while (bw->n >= 8 && bw->len < bw->cap) {
		bw->buf[bw->len] = (uint8_t)(bw->res >> (bw->n - 8));
		bw->n -= 8;
		bw->len += 1;
	}
}

void
grow(struct bitwriter *bw, int to)
{
	void *buf;
	if (to < bw->cap) {
		return;
	}
	if (to < bw->cap*2) {
		to = bw->cap*2;
	}
	if (to < 1024) {
		to = 1024;
	}
	buf = realloc(bw->buf, (size_t)to);
	if (buf == NULL) {
		// uh-oh
		bw->err = ENOMEM;
		return;
	}
	bw->buf = buf;
	bw->cap = to;
}

// Step 1: Round-trip

#define SYNCWORD 0x0B77

struct ac3 {
	struct bitwriter *bw;
	struct bitreader *br;

	int acmod; // channel mode
	int lfeon; // LFE channel present
	int fscod; // frequency code. 0=48kHz; 1=44.1kHz; 2=32kHz
	int frmsizecod; // frame size code

	// channel coupling
	int cplinu; // channel coupling in use flag
	int chincpl;
	int cplbegf, cplendf;
	int ncplbnd;
	int phsflginu;

	int bap[5][256]; // bit allocation
	int cplbap[256];
	int lfebap[256];

	int exp[5][256];
	int cplexp[256];
	int lfeexp[256];

	int b1, b2, b4; // mantissa blocks
};

int ac3(struct bitwriter*, struct bitreader*);
static int copy(struct ac3* a, int n, const char *var);
static int syncframe(struct ac3*);
static void audblk(struct ac3*);
static int copymant(struct ac3 *a, int bap);

static int nfchanstab[] = {2, 1, 2, 3, 3, 4, 4, 5};
static int expgrptab[] = {0, 3, 6, 12};
static int frmsizetab[3][38] = {{
	64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224,
	224, 256, 256, 320, 320, 384, 384, 448, 448, 512, 512, 640, 640, 768,
	768, 896, 896, 1024, 1024, 1152, 1152, 1280, 1280,
}, {
	69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243,
	244, 278, 279, 348, 349, 417, 418, 487, 488, 557, 558, 696, 697, 835,
	836, 975, 976, 1114, 1115, 1253, 1254, 1393, 1394,
}, {
	96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288,
	336, 336, 384, 384, 480, 480, 576, 576, 672, 672, 768, 768, 960, 960,
	1152, 1152, 1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920,
}};

static int sdecaytab[] = {0x0F, 0x11, 0x13, 0x15};
static int fdecaytab[] = {0x3F, 0x53, 0x67, 0x7B};
static int sgaintab[] = {0x540, 0x4D8, 0x478, 0x410};
static int dbkneetab[] = {0x000, 0x700, 0x900, 0xB00};

static int floortab[] = {
	0x2F0, 0x2B0, 0x270, 0x230, 0x1F0, 0x170, 0x0F0,
	0xF800, // -0x800?
};

 // fgaintab[i] = (i+1) * 0x80
static int fgaintab[] = {
	0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400
};

// 7.18
static int quantization_tab[16] = {
	0, 3, 5, 7, 11, 15, /* symmetric */
	5, 6, 7, 8, 9, 10, 11, 12, 14, 16, /* asymmetric */
};

static int grpsizetab[4] = {0, 1, 2, 4};

// Copy and return n bits from a->br to a->bw.
int
copy(struct ac3 *a, int n, const char* var)
{
	uint64_t bits = readbits(a->br, n);
	if (a->br->err) {
		return 0;
	}
	writebits(a->bw, bits, n);
	if (debug && var && var[0]) {
		fprintf(stderr, "%s: %lld\n", var, (long long int)bits);
	}
	return (int)bits;
}

int
ac3(struct bitwriter *bw, struct bitreader *br)
{
	struct ac3 a = {0};
	a.bw = bw;
	a.br = br;
	for (;;) {
		if (syncframe(&a) < 0) {
			return -1;
		}
	}
}

/* Appologies for the terrible variable names. They're straight from the spec. */

static int
syncframe(struct ac3 *a)
{
	int i, blk;
	int syncword, crcrsv, crc2;
	int addbsil;
	int nauxbits;

	syncword = (int)readbits(a->br, 16);
	if (syncword != SYNCWORD) {
		return -1;
	}
	writebits(a->bw, (uint64_t)syncword, 16);
	copy(a, 16, "crc1");
	a->fscod      = copy(a, 2, "fscod");
	a->frmsizecod = copy(a, 6, "frmsizecod");
	assert(a->fscod <= 2);
	assert(a->frmsizecod <= 36);

	// We'll come back later to fix the CRC.

	// Bit stream information
	//
	copy(a, 5, "bsid");
	copy(a, 3, "bsmod");
	a->acmod = copy(a, 3, "acmod");
	if ((a->acmod & 1) && a->acmod != 1) {
		copy(a, 2, "cmixlev");
	}
	if (a->acmod & 4) {
		copy(a, 2, "surmixlev");
	}
	if (a->acmod == 2) {
		copy(a, 2, "dsurmod");
	}
	a->lfeon = copy(a, 1, "lfeon");
	copy(a, 5, "dialnorm");
	if (copy(a, 1, "compre")) {
		copy(a, 8, "compr");
	}
	if (copy(a, 1, "langcode")) {
		copy(a, 8, "langcod");
	}
	if (copy(a, 1, "audprodie")) {
		copy(a, 5, "mixlevel");
		copy(a, 2, "roomtyp");
	}
	if (a->acmod == 0) {
		copy(a, 5, "dialnorm2");
		if (copy(a, 1, "compr2e")) {
			copy(a, 8, "compr2");
		}
		if (copy(a, 1, "langcod2e")) {
			copy(a, 8, "langcod2");
		}
		if (copy(a, 1, "audprodi2e")) {
			copy(a, 5, "mixlevel2");
			copy(a, 2, "roomtyp2");
		}
	}
	copy(a, 1, "copyrightb");
	copy(a, 1, "origbe");
	if (copy(a, 1, "timecod1e")) {
		copy(a, 14, "timecod1");
	}
	if (copy(a, 1, "timecod2e")) {
		copy(a, 14, "timecod2");
	}
	if (copy(a, 1, "addbsie")) {
		addbsil = copy(a, 6, "addbsil");
		for (i = 0; i < addbsil + 1; i++) {
			copy(a, 8, "addbsi");
		}
	}

	// Audio blocks
	//
	// reset coupling parameters
	a->cplinu = 0;
	a->chincpl = 0;
	a->cplbegf = 0;
	a->cplendf = 0;
	a->ncplbnd = 0;
	a->phsflginu = 0;
	for (blk = 0; blk < 6; blk++) {
		audblk(a);
	}

	// Auxilliary bits
	//
	nauxbits = frmsizetab[a->fscod][a->frmsizecod] * 16;
	nauxbits -= getnbitsread(a->br);
	// XXX reset nbitsread at start of frame
	while (nauxbits >= 16) {
		copy(a, 16, "");
		nauxbits -= 16;
	}
	copy(a, nauxbits, "");
	copy(a, 14, "auxdatal");
	copy(a, 1, "auxdatae");

	// Final CRC
	//
	crcrsv = copy(a, 1, "crcrsv");
	crc2 = copy(a, 16, "crc2");

	(void)crcrsv;
	(void)crc2;

	return 0;
}

static void
audblk(struct ac3 *a)
{
	int i, ch, freq, seg;
	int nfchans;

	nfchans = nfchanstab[a->acmod];

	copy(a, nfchans, "blksw[ch]");
	copy(a, nfchans, "dlithflag[ch]");

	if (copy(a, 1, "dynrange")) {
		copy(a, 8, "dynrang");
	}

	if (a->acmod == 0) {
		if (copy(a, 1, "dynrang2e")) {
			copy(a, 8, "dynrang2");
		}
	}

	// Coupling
	//
	int bnd;
	int cplcoe[5];
	int ncplsubnd;
	int cplstrtmant, cplendmant;
	if (copy(a, 1, "cplstre")) {
		a->cplinu = copy(a, 1, "cplinu");
		if (a->cplinu) {
			for (ch = 0; ch < nfchans; ch++) {
				a->chincpl |= copy(a, 1, "chincpl[ch]")<<ch;
			}
			if (a->acmod == 2) {
				a->phsflginu = copy(a, 1, "phsflginu");
			}
			a->cplbegf = copy(a, 4, "cplbegf");
			a->cplendf = copy(a, 4, "cplendf") + 3;
			ncplsubnd = a->cplendf - a->cplbegf;
			a->ncplbnd = ncplsubnd;
			for (bnd = 1; bnd < ncplsubnd; bnd++) {
				a->ncplbnd -= copy(a, 1, "cplbndstrc[bnd]");
			}
		}
	}
	cplstrtmant = a->cplbegf*12 + 37;
	cplendmant = a->cplendf*12 + 37;
	if (a->cplinu) {
		for (ch = 0; ch < nfchans; ch++) {
			if (a->chincpl & (1<<ch)) {
				cplcoe[ch] = copy(a, 1, "cplcoe[ch]");
				if (cplcoe[ch]) {
					copy(a, 2, "mstrcplco[ch]");
					for (bnd = 0; bnd < a->ncplbnd; bnd++) {
						copy(a, 4, "cplcoexp[ch][bnd]");
						copy(a, 4, "cplcomant[ch][bnd]");
					}
				}
			}
		}
		if (a->acmod == 2 && a->phsflginu && (cplcoe[0] || cplcoe[1])) {
			copy(a, a->ncplbnd, "phsflg");
		}
	}
	if (a->acmod == 2) {
		if (copy(a, 1, "rematstr")) {
			if (a->cplbegf == 0 && a->cplinu) {
				copy(a, 2, "rematflg");
			} else if (a->cplbegf <= 2 && a->cplinu) {
				copy(a, 3, "rematflg");
			} else {
				copy(a, 4, "rematflg");
			}
		}
	}

	// Exponents
	//
	int strtmant[5], endmant[5] = {0};
	int cplexpstr, chexpstr[5], lfeexpstr;
	int chbwcod[5];
	int grp, ncplgrps, nchgrps;
	int tmp;
	cplexpstr = 0;
	lfeexpstr = 0;
	if (a->cplinu) {
		cplexpstr = copy(a, 2, "cplexpstr");
	}
	for (ch = 0; ch < nfchans; ch++) {
		chexpstr[ch] = copy(a, 2, "chexpstr[ch]");
	}
	if (a->lfeon) {
		lfeexpstr = copy(a, 1, "lfeexpstr");
	}
	for (ch = 0; ch < nfchans; ch++) {
		strtmant[ch] = 0;

		if (chexpstr[ch] != 0) {
			if (a->chincpl & (1<<ch)) {
				endmant[ch] = cplstrtmant;
			} else {
				chbwcod[ch] = copy(a, 6, "chbwcod[ch]");
				endmant[ch] = 37 + 3*(chbwcod[ch] + 12);
			}
		}
	}
	int dexp[256];
	int grpsize;
	int absexp;
	if (a->cplinu) {
		if (cplexpstr != 0) {
			ncplgrps = (cplendmant - cplstrtmant) / expgrptab[cplexpstr];
			absexp = copy(a, 4, "cplabsexp") << 1;
			for (grp = 0; grp < ncplgrps; grp++) {
				dexp[grp] = copy(a, 7, "cplexps");
			}
			grpsize = grpsizetab[cplexpstr];
			decode_exponents(&a->cplexp[cplstrtmant-1], dexp, ncplgrps, absexp, grpsize);
			// XXX this sets exp[0] = absexp
		}
	}
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0) {
			absexp = copy(a, 4, "exps[ch][0]");
			tmp = expgrptab[chexpstr[ch]];
			nchgrps = (endmant[ch] - 1 + (tmp-3)) / tmp;
			for (grp = 0; grp < nchgrps; grp++) {
				dexp[grp] = copy(a, 7, "exps[ch][grp]");
				// XXX grp+1?
			}
			grpsize = grpsizetab[chexpstr[ch]];
			decode_exponents(a->exp[ch], dexp, nchgrps, absexp, grpsize);
			copy(a, 2, "gainrng[ch]");
		}
	}
	if (a->lfeon) {
		if (lfeexpstr != 0) {
			absexp = copy(a, 4, "lfeexps[0]");
			dexp[0] = copy(a, 7, "lfeexps[1]");
			dexp[1] = copy(a, 7, "lfeexps[2]");
			grpsize = grpsizetab[lfeexpstr];
			decode_exponents(a->lfeexp, dexp, 2, absexp, grpsize);
		}
	}

	// Bit allocation
	//
	struct balloc ba[5], cplba = {0}, lfeba = {0};
	int csnroffst = 0;
	int cpldeltbae;
	int sdecay, fdecay, sgain, dbknee, floor;
	cpldeltbae = 2;
	ba[0].deltbae = 2;
	ba[1].deltbae = 2;
	ba[2].deltbae = 2;
	ba[3].deltbae = 2;
	ba[4].deltbae = 2;
	cplba.deltnseg = 0; // XXX init at start of frame
	ba[0].deltnseg = 0;
	ba[1].deltnseg = 0;
	ba[2].deltnseg = 0;
	ba[3].deltnseg = 0;
	ba[4].deltnseg = 0;
	if (copy(a, 1, "baie")) {
		sdecay = sdecaytab[copy(a, 2, "sdcycod")];
		fdecay = fdecaytab[copy(a, 2, "fdcycod")];
		sgain = sgaintab[copy(a, 2, "sgaincod")];
		dbknee = dbkneetab[copy(a, 2, "dbpbcod")];
		floor = floortab[copy(a, 2, "floorcod")];
	} else {
		sdecay = sdecaytab[2];
		fdecay = fdecaytab[1];
		sgain = sgaintab[1];
		dbknee = dbkneetab[2];
		floor = floortab[7];
	}
	if (copy(a, 1, "snroffste")) {
		csnroffst = copy(a, 6, "csnroffst");
		if (a->cplinu) {
			cplba.fsnroffst = copy(a, 4, "cplfsnroffst");
			cplba.fgain = fgaintab[copy(a, 3, "cplfgaincod")];
		}
		for (ch = 0; ch < nfchans; ch++) {
			ba[ch].fsnroffst = copy(a, 4, "fsnroffst");
			ba[ch].fgain = fgaintab[copy(a, 3, "fgaincod")];
		}
		if (a->lfeon) {
			lfeba.fsnroffst = copy(a, 4, "lfefsnroffst");
			lfeba.fgain = fgaintab[copy(a, 3, "lfefgaincod")];
		}
	}
	if (a->cplinu) {
		if (copy(a, 1, "cplleake")) {
			cplba.fleak = copy(a, 3, "cplfleak");
			cplba.sleak = copy(a, 3, "cplsleak");
		}
	}
	if (copy(a, 1, "deltbaie")) {
		if (a->cplinu) {
			cpldeltbae = copy(a, 2, "cpldeltdae");
		}
		for (ch = 0; ch < nfchans; ch++) {
			ba[ch].deltbae = copy(a, 2, "deltdae[ch]");
		}
		if (a->cplinu) {
			if (cpldeltbae == 1) {
				cplba.deltnseg = copy(a, 3, "deltnseg") + 1;
				for (seg = 0; seg < cplba.deltnseg; seg++) {
					cplba.deltoffst[seg] = copy(a, 5, "deltoffst");
					cplba.deltlen[seg] = copy(a, 4, "deltlen");
					cplba.deltba[seg] = copy(a, 3, "deltba");
				}
			}
		}
		for (ch = 0; ch < nfchans; ch++) {
			if (ba[ch].deltbae == 1) {
				ba[ch].deltnseg = copy(a, 3, "deltnseg") + 1;
				for (seg = 0; seg < ba[ch].deltnseg; seg++) {
					ba[ch].deltoffst[seg] = copy(a, 5, "deltoffst");
					ba[ch].deltlen[seg] = copy(a, 4, "deltlen");
					ba[ch].deltba[seg] = copy(a, 3, "deltba");
				}
			}
		}
	}

	// 7.2.2.1
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0) {
			bit_allocation(a->bap[ch], &ba[ch], a->fscod, a->exp[ch], strtmant[ch], endmant[ch], csnroffst, sdecay, fdecay, sgain, dbknee, floor);
		}
	}

	if (cplexpstr != 0) {
		if (a->cplinu) {
			bit_allocation(a->cplbap, &cplba, a->fscod, a->cplexp, cplstrtmant, cplendmant, csnroffst, sdecay, fdecay, sgain, dbknee, floor);
		}
	}
	int lfestrtmant = 0;
	int lfeendmant = 7;
	if (lfeexpstr != 0) {
		bit_allocation(a->lfebap, &lfeba, a->fscod, a->lfeexp, lfestrtmant, lfeendmant, csnroffst, sdecay, fdecay, sgain, dbknee, floor);
	}

	// Skip bytes
	//
	int skipl;
	if (copy(a, 1, "skiple")) {
		skipl = copy(a, 9, "skipl");
		for (i = 0; i < skipl; i++) {
			copy(a, 8, "");
		}
	}

	// Mantissas
	//
	// nchmant[ch] from chbwcod[ch] or cplbegf
	//
	//int nchmant[5];
	//int ncplmant;
	int got_cplchan;
	got_cplchan = 0;
	a->b1 = 0;
	a->b2 = 0;
	a->b4 = 0;
	for (ch = 0; ch < nfchans; ch++) {
		for (freq = strtmant[ch]; freq < endmant[ch]; freq++) {
			copymant(a, a->bap[ch][freq]); // chmant[ch][bin]
		}
		if (a->cplinu && (a->chincpl & (1<<ch)) && !got_cplchan) {
			for (freq = cplstrtmant; freq < cplendmant; freq++) {
				copymant(a, a->cplbap[freq]); // cplmant[bin]
			}
			got_cplchan = 1;
		}
	}
	if (a->lfeon) {
		for (freq = lfestrtmant; freq < lfeendmant; freq++) {
			copymant(a, a->lfebap[freq]); // lfemant[bin]
		}
	}
}

static int copymant(struct ac3 *a, int bap)
{
	int bits = 0;
	switch (bap) {
	case 0:
		//fprintf(stderr, "mant,0\n");
		// random noise
		break;
	case 1:
		if (a->b1) {
			a->b1--;
		} else {
			bits = copy(a, 5, "mant,1.66");
			a->b1 = 2;
		}
		break;
	case 2:
		if (a->b2) {
			a->b2--;
		} else {
			bits = copy(a, 7, "mant,2.33");
			a->b2 = 2;
		}
		break;
	case 3:
		bits = copy(a, 3, "mant,3");
		break;
	case 4:
		if (a->b4) {
			a->b4 = 0;
		} else {
			bits = copy(a, 7, "mant,3.5");
			a->b4 = 1;
		}
		break;
	case 5:
		bits = copy(a, 4, "mant,5");
		break;
	default: // 6-15
		if (bap > 15) {
			// invalid
			bap = 15;
		}
		bits = copy(a, quantization_tab[bap], "mant,x");
		break;
	}
	return bits;
}

int main()
{
	struct bitwriter bw = {NULL};
	struct bitreader br = {stdin};
	ac3(&bw, &br);
	fprintf(stderr, "\n");
	fprintf(stderr, "len: %d\n", bw.len);
	fwrite(bw.buf, 1, (size_t)bw.len, stdout);
	return 0;
}
