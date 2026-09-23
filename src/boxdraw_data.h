#define BDL (1<<8)
#define BDA (1<<9)

#define BBD (1<<10)
#define BBL (2<<10)
#define BBU (3<<10)
#define BBR (4<<10)
#define BBQ (5<<10)
#define BRL (6<<10)

#define BBS (1<<14)
#define BDB (1<<15)

#define LL (1<<0)
#define LU (1<<1)
#define LR (1<<2)
#define LD (1<<3)
#define LH (LL+LR)
#define LV (LU+LD)

#define DL (1<<4)
#define DU (1<<5)
#define DR (1<<6)
#define DD (1<<7)
#define DH (DL+DR)
#define DV (DU+DD)

#define HL (LL+DL)
#define HU (LU+DU)
#define HR (LR+DR)
#define HD (LD+DD)
#define HH (HL+HR)
#define HV (HU+HD)

#define TL (1<<0)
#define TR (1<<1)
#define BL (1<<2)
#define BR (1<<3)

/* boxdata[256] maps U+2500-U+25FF (BOXDRAW_BLOCK) to drawing instructions.
 * Generated from Unicode Blocks.txt - do not edit manually.
 * Index = low byte of codepoint (u & 0xFF). */
static const unsigned short boxdata[256] = {

	[0x00] = BDL + LH,
	[0x02] = BDL + LV,
	[0x0c] = BDL + LD + LR,
	[0x10] = BDL + LD + LL,
	[0x14] = BDL + LU + LR,
	[0x18] = BDL + LU + LL,
	[0x1c] = BDL + LV + LR,
	[0x24] = BDL + LV + LL,
	[0x2c] = BDL + LH + LD,
	[0x34] = BDL + LH + LU,
	[0x3c] = BDL + LV + LH,
	[0x74] = BDL + LL,
	[0x75] = BDL + LU,
	[0x76] = BDL + LR,
	[0x77] = BDL + LD,

	[0x01] = BDL + HH,
	[0x03] = BDL + HV,
	[0x0d] = BDL + HR + LD,
	[0x0e] = BDL + HD + LR,
	[0x0f] = BDL + HD + HR,
	[0x11] = BDL + HL + LD,
	[0x12] = BDL + HD + LL,
	[0x13] = BDL + HD + HL,
	[0x15] = BDL + HR + LU,
	[0x16] = BDL + HU + LR,
	[0x17] = BDL + HU + HR,
	[0x19] = BDL + HL + LU,
	[0x1a] = BDL + HU + LL,
	[0x1b] = BDL + HU + HL,
	[0x1d] = BDL + HR + LV,
	[0x1e] = BDL + HU + LD + LR,
	[0x1f] = BDL + HD + LR + LU,
	[0x20] = BDL + HV + LR,
	[0x21] = BDL + HU + HR + LD,
	[0x22] = BDL + HD + HR + LU,
	[0x23] = BDL + HV + HR,
	[0x25] = BDL + HL + LV,
	[0x26] = BDL + HU + LD + LL,
	[0x27] = BDL + HD + LU + LL,
	[0x28] = BDL + HV + LL,
	[0x29] = BDL + HU + HL + LD,
	[0x2a] = BDL + HD + HL + LU,
	[0x2b] = BDL + HV + HL,
	[0x2d] = BDL + HL + LD + LR,
	[0x2e] = BDL + HR + LL + LD,
	[0x2f] = BDL + HH + LD,
	[0x30] = BDL + HD + LH,
	[0x31] = BDL + HD + HL + LR,
	[0x32] = BDL + HR + HD + LL,
	[0x33] = BDL + HH + HD,
	[0x35] = BDL + HL + LU + LR,
	[0x36] = BDL + HR + LU + LL,
	[0x37] = BDL + HH + LU,
	[0x38] = BDL + HU + LH,
	[0x39] = BDL + HU + HL + LR,
	[0x3a] = BDL + HU + HR + LL,
	[0x3b] = BDL + HH + HU,
	[0x3d] = BDL + HL + LV + LR,
	[0x3e] = BDL + HR + LV + LL,
	[0x3f] = BDL + HH + LV,
	[0x40] = BDL + HU + LH + LD,
	[0x41] = BDL + HD + LH + LU,
	[0x42] = BDL + HV + LH,
	[0x43] = BDL + HU + HL + LD + LR,
	[0x44] = BDL + HU + HR + LD + LL,
	[0x45] = BDL + HD + HL + LU + LR,
	[0x46] = BDL + HD + HR + LU + LL,
	[0x47] = BDL + HH + HU + LD,
	[0x48] = BDL + HH + HD + LU,
	[0x49] = BDL + HV + HL + LR,
	[0x4a] = BDL + HV + HR + LL,
	[0x4b] = BDL + HV + HH,
	[0x78] = BDL + HL,
	[0x79] = BDL + HU,
	[0x7a] = BDL + HR,
	[0x7b] = BDL + HD,
	[0x7c] = BDL + HR + LL,
	[0x7d] = BDL + HD + LU,
	[0x7e] = BDL + HL + LR,
	[0x7f] = BDL + HU + LD,

	[0x50] = BDL + DH,
	[0x51] = BDL + DV,
	[0x52] = BDL + DR + LD,
	[0x53] = BDL + DD + LR,
	[0x54] = BDL + DR + DD,
	[0x55] = BDL + DL + LD,
	[0x56] = BDL + DD + LL,
	[0x57] = BDL + DL + DD,
	[0x58] = BDL + DR + LU,
	[0x59] = BDL + DU + LR,
	[0x5a] = BDL + DU + DR,
	[0x5b] = BDL + DL + LU,
	[0x5c] = BDL + DU + LL,
	[0x5d] = BDL + DL + DU,
	[0x5e] = BDL + DR + LV,
	[0x5f] = BDL + DV + LR,
	[0x60] = BDL + DV + DR,
	[0x61] = BDL + DL + LV,
	[0x62] = BDL + DV + LL,
	[0x63] = BDL + DV + DL,
	[0x64] = BDL + DH + LD,
	[0x65] = BDL + DD + LH,
	[0x66] = BDL + DD + DH,
	[0x67] = BDL + DH + LU,
	[0x68] = BDL + DU + LH,
	[0x69] = BDL + DH + DU,
	[0x6a] = BDL + DH + LV,
	[0x6b] = BDL + DV + LH,
	[0x6c] = BDL + DH + DV,

	[0x6d] = BDA + LD + LR,
	[0x6e] = BDA + LD + LL,
	[0x6f] = BDA + LU + LL,
	[0x70] = BDA + LU + LR,

	[0x81] = BBD + 7, [0x82] = BBD + 6, [0x83] = BBD + 5, [0x84] = BBD + 4,
	[0x85] = BBD + 3, [0x86] = BBD + 2, [0x87] = BBD + 1, [0x88] = BBD + 0,

	[0x89] = BBL + 7, [0x8a] = BBL + 6, [0x8b] = BBL + 5, [0x8c] = BBL + 4,
	[0x8d] = BBL + 3, [0x8e] = BBL + 2, [0x8f] = BBL + 1,

	[0x80] = BBU + 4, [0x94] = BBU + 1,
	[0x90] = BBR + 4, [0x95] = BBR + 7,

	[0x96] = BBQ + BL,
	[0x97] = BBQ + BR,
	[0x98] = BBQ + TL,
	[0x99] = BBQ + TL + BL + BR,
	[0x9a] = BBQ + TL + BR,
	[0x9b] = BBQ + TL + TR + BL,
	[0x9c] = BBQ + TL + TR + BR,
	[0x9d] = BBQ + TR,
	[0x9e] = BBQ + BL + TR,
	[0x9f] = BBQ + BL + TR + BR,

	[0x91] = BBS + 1, [0x92] = BBS + 2, [0x93] = BBS + 3,

};
