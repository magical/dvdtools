#include "ac3tab.c"

int abs(int);

static int
min(int a, int b) {
	if (a < b) {
		return a;
	}
	return b;
}

static int
max(int a, int b) {
	if (a > b) {
		return a;
	}
	return b;
}

static int
calc_lowcomp(int a, int b0, int b1, int bin) {
	if (bin < 7) {
		if (b0 + 256 == b1) {
			a = 0x180;
		} else if (b0 > b1) {
			a = max(0, a - 64);
		}
	} else if (bin < 20) {
		if (b0 + 256 == b1) {
			a = 0x180 - 64;
		} else if (b0 > b1) {
			a = max(0, a - 64);
		}
	} else {
		a = max(0, a - 128);
	}
	return a;
}

static int
logadd(int a, int b) {
	int c, i;
	c = a - b;
	i = min(abs(c) / 2, 255);
	if (c >= 0) {
		return a + latab[i];
	} else {
		return b + latab[i];
	}
}

int
bit_allocation(int *bapp, int start, int end, int fgain, int snroffset) {
	int exp[256];
	int deltbae, deltnseg, deltoffst[8], deltlen[8], deltba[8];
	int sdecay, fdecay, sgain, dbknee, floor;
	int fscod;

	int psd[256], bndpsd[50], mask[50], bap[256];

	int fastleak, slowleak;
	int lowcomp;
	int delta;

	int i, band, bin, seg;
	int lastbin;

	// Exponent mapping into power-spectral density. 7.2.2.2
	for (bin = start; bin < end; bin++) {
		psd[bin] = (24 - exp[bin]) << 7;
	}

	// PSD integration. 7.2.2.3
	bin = start;
	band = masktab[start];
	do {
		lastbin = min(bndtab[band] + bndsz[band], end);
		bndpsd[band] = psd[bin];
		for (bin++; bin < lastbin; bin++) {
			bndpsd[band] = logadd(bndpsd[band], psd[bin]);
		}
		band++;
	} while (lastbin < end);

	// Excitation function. 7.2.2.4
	int bndstrt, bndend, begin;
	int excite[256];
	lowcomp = 0;
	bndstrt = masktab[start];
	bndend = masktab[end - 1] + 1;
	if (bndstrt == 0) { // full bandwidth and lfe
		lowcomp = calc_lowcomp(lowcomp, bndpsd[0], bndpsd[1], 0);
		excite[0] = bndpsd[0] - fgain - lowcomp;
		lowcomp = calc_lowcomp(lowcomp, bndpsd[1], bndpsd[2], 1);
		excite[1] = bndpsd[1] - fgain - lowcomp;
		begin = 7;
		for (band = 2; band < 7; band++) {
			if (!(bndend == 7 && band == 6)) {
				lowcomp = calc_lowcomp(lowcomp, bndpsd[band], bndpsd[band+1], band);
			}
			fastleak = bndpsd[band] - fgain;
			slowleak = bndpsd[band] - sgain;
			excite[band] = fastleak - lowcomp;
			if (!(bndend == 7 && band == 6)) {
				if (bndpsd[band] <= bndpsd[band+1]) {
					begin = band + 1;
					break;
				}
			}
		}
		for (band = begin; band < min(bndend, 22); band++) {
			if (bndend == 7 && band == 6) {
				lowcomp = calc_lowcomp(lowcomp, bndpsd[band], bndpsd[band+1], band);
			}
			fastleak = max(fastleak - fdecay, bndpsd[band] - fgain);
			slowleak = max(slowleak - sdecay, bndpsd[band] - sgain);
			excite[band] = max(fastleak - lowcomp, slowleak);
		}
		begin = 22;
	} else { // coupled
		begin = bndstrt;
		//fastleak = (cplfleak + 3) << 8;
		//slowleak = (cplsleak + 3) << 8;
	}
	for (band = begin; band < bndend; band++) {
		fastleak = max(fastleak - fdecay, bndpsd[band] - fgain);
		slowleak = max(slowleak - sdecay, bndpsd[band] - sgain);
		excite[band] = max(fastleak, slowleak);
	}

	// Masking curve. 7.2.2.5
	for (band = bndstrt; band < bndend; band++) {
		if (bndpsd[band] < dbknee) {
			excite[band] += (dbknee - bndpsd[band]);
		}
		mask[band] = max(excite[band], hth[fscod][band]);
	}

	// Delta bit allocation. 7.2.2.6
	if (deltbae == 0 || deltbae == 1) {
		band = 0;
		for (seg = 0; seg < deltnseg+1; seg++) {
			band += deltoffst[seg];
			delta = (deltba[seg] + (deltba[seg] >= 4) - 4) << 7;
			for (i = 0; i < deltlen[seg]; i++) {
				mask[band] += delta;
				band++;
			}
		}
	}

	// Bit allocation. 7.2.2.7
	bin = start;
	band = masktab[start];
	do {
		lastbin = min(bndtab[band] + bndsz[band], end);
		mask[band] -= snroffset;
		mask[band] -= floor;
		if (mask[band] < 0) {
			mask[band] = 0;
		}
		mask[band] &= 0x1fe0;
		mask[band] += floor;
		for (; bin < lastbin; bin++) {
			i = (psd[bin] - mask[band]) >> 5;
			i = min(63, max(0, i));
			bap[bin] = baptab[i];
		}
		band++;
	} while (lastbin < end);

	// Output
	for (bin = 0; bin < 256; bin++) {
		bapp[bin] = bap[bin];
	}

	return 0;
}