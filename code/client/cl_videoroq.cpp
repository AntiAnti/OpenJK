/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "../server/exe_headers.h"

/*****************************************************************************
 * name:		cl_videoroq.c
 *
 * desc:		roq video decoder
 *
 * $Archive: /MissionPack/code/client/cl_cin.c $
 * $Author: Ttimo $
 * $Revision: 82 $
 * $Modtime: 4/13/01 4:48p $
 * $Date: 4/13/01 4:48p $
 *
 * cl_glconfig.hwtype trtypes 3dfx/ragepro need 256x256
 *
 *****************************************************************************/

#include "client.h"
#include "client_ui.h"	// CHC
#include "snd_local.h"
#include "qcommon/stringed_ingame.h"

 ////////////////////////////////////////////////////////////////

static void RoQ_init(cin_cache* table);
void RoQInterrupt(cin_cache* table);
unsigned short				vq2[256 * 16 * 4];
unsigned short				vq4[256 * 64 * 4];
unsigned short				vq8[256 * 256 * 4];
extern int					s_soundtime;		// sample PAIRS

namespace roq {
	cin_interface			cin_info;
	cin_cache*				activeTable;
	short					soundBufferTemp[32768];
}

////////////////////////////////////////////////////////////////

#define MAXSIZE				8
#define MINSIZE				4

#define ROQ_QUAD			0x1000
#define ROQ_QUAD_INFO		0x1001
#define ROQ_CODEBOOK		0x1002
#define ROQ_QUAD_VQ			0x1011
#define ROQ_QUAD_JPEG		0x1012
#define ROQ_QUAD_HANG		0x1013
#define ROQ_PACKET			0x1030
#define ZA_SOUND_MONO		0x1020
#define ZA_SOUND_STEREO		0x1021
#define ROQ_VQ_MOT			0x0000
#define ROQ_VQ_FCC			0x4000
#define ROQ_VQ_SLD			0x8000
#define ROQ_VQ_CCC			0xC000

////////////////////////////////////////////////////////////////

#define VQ2TO4(a,b,c,d) { \
    	*c++ = a[0];	\
	*d++ = a[0];	\
	*d++ = a[0];	\
	*c++ = a[1];	\
	*d++ = a[1];	\
	*d++ = a[1];	\
	*c++ = b[0];	\
	*d++ = b[0];	\
	*d++ = b[0];	\
	*c++ = b[1];	\
	*d++ = b[1];	\
	*d++ = b[1];	\
	*d++ = a[0];	\
	*d++ = a[0];	\
	*d++ = a[1];	\
	*d++ = a[1];	\
	*d++ = b[0];	\
	*d++ = b[0];	\
	*d++ = b[1];	\
	*d++ = b[1];	\
	a += 2; b += 2; }

#define VQ2TO2(a,b,c,d) { \
	*c++ = *a;	\
	*d++ = *a;	\
	*d++ = *a;	\
	*c++ = *b;	\
	*d++ = *b;	\
	*d++ = *b;	\
	*d++ = *a;	\
	*d++ = *a;	\
	*d++ = *b;	\
	*d++ = *b;	\
	a++; b++; }

#define _clamp(value, vmin, vmax) (value > vmax ? vmax : (value < vmin ? vmin : value))

////////////////////////////////////////////////////////////////

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void recurseQuad(long startX, long startY, long quadSize, long xOff, long yOff)
{
	byte* scroff;
	long bigx, bigy, lowx, lowy, useY;
	long offset;

	offset = roq::activeTable->screenDelta;

	lowx = lowy = 0;
	bigx = roq::activeTable->xsize;
	bigy = roq::activeTable->ysize;

	if (bigx > roq::activeTable->CIN_WIDTH) bigx = roq::activeTable->CIN_WIDTH;
	if (bigy > roq::activeTable->CIN_HEIGHT) bigy = roq::activeTable->CIN_HEIGHT;

	if ((startX >= lowx) && (startX + quadSize) <= (bigx) && (startY + quadSize) <= (bigy) && (startY >= lowy) && quadSize <= MAXSIZE) {
		useY = startY;
		scroff = roq::cin_info.cin->linbuf + (useY + ((roq::activeTable->CIN_HEIGHT - bigy) >> 1) + yOff) * (roq::activeTable->samplesPerLine) + (((startX + xOff)) * roq::activeTable->samplesPerPixel);

		roq::cin_info.cin->qStatus[0][roq::activeTable->onQuad] = scroff;
		roq::cin_info.cin->qStatus[1][roq::activeTable->onQuad++] = scroff + offset;
	}

	if (quadSize != MINSIZE) {
		quadSize >>= 1;
		recurseQuad(startX, startY, quadSize, xOff, yOff);
		recurseQuad(startX + quadSize, startY, quadSize, xOff, yOff);
		recurseQuad(startX, startY + quadSize, quadSize, xOff, yOff);
		recurseQuad(startX + quadSize, startY + quadSize, quadSize, xOff, yOff);
	}
}


/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void setupQuad(long xOff, long yOff)
{
	long numQuadCels, i, x, y;
	byte* temp;

	if (xOff == roq::cin_info.cin->oldXOff && yOff == roq::cin_info.cin->oldYOff && roq::activeTable->ysize == (unsigned)roq::cin_info.cin->oldysize && roq::activeTable->xsize == (unsigned)roq::cin_info.cin->oldxsize) {
		return;
	}

	roq::cin_info.cin->oldXOff = xOff;
	roq::cin_info.cin->oldYOff = yOff;
	roq::cin_info.cin->oldysize = roq::activeTable->ysize;
	roq::cin_info.cin->oldxsize = roq::activeTable->xsize;

	numQuadCels = (roq::activeTable->xsize * roq::activeTable->ysize) / (16);
	numQuadCels += numQuadCels / 4;
	numQuadCels += 64;							  // for overflow

	roq::activeTable->onQuad = 0;

	// Reallocate qStatus arrays for arbitrary frame sizes
	if (numQuadCels > roq::cin_info.cin->qStatusCapacity || !roq::cin_info.cin->qStatus[0] || !roq::cin_info.cin->qStatus[1])
	{
		if (roq::cin_info.cin->qStatus[0]) { Z_Free(roq::cin_info.cin->qStatus[0]); roq::cin_info.cin->qStatus[0] = NULL; }
		if (roq::cin_info.cin->qStatus[1]) { Z_Free(roq::cin_info.cin->qStatus[1]); roq::cin_info.cin->qStatus[1] = NULL; }
		roq::cin_info.cin->qStatus[0] = (byte**)Z_Malloc(sizeof(byte*) * numQuadCels, TAG_TEMP_HUNKALLOC);
		roq::cin_info.cin->qStatus[1] = (byte**)Z_Malloc(sizeof(byte*) * numQuadCels, TAG_TEMP_HUNKALLOC);
		roq::cin_info.cin->qStatusCapacity = numQuadCels;
	}

	for (y = 0; y < (long)roq::activeTable->ysize; y += 16)
		for (x = 0; x < (long)roq::activeTable->xsize; x += 16)
			recurseQuad(x, y, 16, xOff, yOff);

	temp = NULL;

	for (i = (numQuadCels - 64); i < numQuadCels; i++) {
		roq::cin_info.cin->qStatus[0][i] = temp;			  // eoq
		roq::cin_info.cin->qStatus[1][i] = temp;			  // eoq
	}
}


/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void readQuadInfo(byte* qData)
{
	roq::activeTable->xsize = qData[0] + qData[1] * 256; //512
	roq::activeTable->ysize = qData[2] + qData[3] * 256; //512
	roq::activeTable->maxsize = qData[4] + qData[5] * 256; //8
	roq::activeTable->minsize = qData[6] + qData[7] * 256; //4

	roq::activeTable->CIN_HEIGHT = roq::activeTable->ysize;
	roq::activeTable->CIN_WIDTH = roq::activeTable->xsize;

	roq::activeTable->samplesPerLine = roq::activeTable->CIN_WIDTH * roq::activeTable->samplesPerPixel;
	roq::activeTable->screenDelta = roq::activeTable->CIN_HEIGHT * roq::activeTable->samplesPerLine;

	// Reallocate linbuf to fit arbitrary sizes
	int twoFramesBufferSize = roq::activeTable->screenDelta * 2; // two frames
	if (roq::cin_info.cin->linbufCapacity < twoFramesBufferSize || !roq::cin_info.cin->linbuf)
	{
		if (roq::cin_info.cin->linbuf) { Z_Free(roq::cin_info.cin->linbuf); roq::cin_info.cin->linbuf = NULL; roq::cin_info.cin->linbufCapacity = 0; }
		roq::cin_info.cin->linbuf = (byte*)Z_Malloc(twoFramesBufferSize, TAG_TEMP_HUNKALLOC);
		roq::cin_info.cin->linbufCapacity = twoFramesBufferSize;
	}
	roq::activeTable->buf = roq::cin_info.cin->linbuf + roq::activeTable->screenDelta;

	roq::activeTable->half = qfalse;
	roq::activeTable->smootheddouble = qfalse;

	roq::activeTable->VQ0 = roq::activeTable->VQNormal;
	roq::activeTable->VQ1 = roq::activeTable->VQBuffer;

	roq::activeTable->t[0] = roq::activeTable->screenDelta;
	roq::activeTable->t[1] = -roq::activeTable->screenDelta;

	roq::activeTable->drawX = _clamp(roq::activeTable->CIN_WIDTH, 1, cls.glconfig.maxTextureSize);
	roq::activeTable->drawY = _clamp(roq::activeTable->CIN_HEIGHT, 1, cls.glconfig.maxTextureSize);

	// This safety check is completely unnecessary for all videocards since voodoo2
	// Unless you try to feed the game a video with resolution higher than 16K
	if (roq::activeTable->drawX != roq::activeTable->CIN_WIDTH || roq::activeTable->drawY != roq::activeTable->CIN_HEIGHT)
	{
		if (roq::activeTable->CIN_WIDTH != 256 || roq::activeTable->CIN_HEIGHT != 256) {
			Com_Printf("HACK: approxmimating cinematic for Rage Pro or Voodoo\n");
		}
		// Just set to minimum, nah
		roq::activeTable->drawX = 256;
		roq::activeTable->drawY = 256;
	}
}

/******************************************************************************
*
* Function:
*
* Description:
*
******************************************************************************/

static void decodeCodeBook(byte* input, unsigned short roq_flags)
{
	long	i, j, two, four;
	unsigned short* aptr, * bptr, * cptr, * dptr;
	long	y0, y1, y2, y3, cr, cb;
	byte* bbptr, * baptr, * bcptr, * bdptr;
	union {
		unsigned int* i;
		unsigned short* s;
	} iaptr, ibptr, icptr, idptr;

	if (!roq_flags) {
		two = four = 256;
	}
	else {
		two = roq_flags >> 8;
		if (!two) two = 256;
		four = roq_flags & 0xff;
	}

	four *= 2;

	bptr = (unsigned short*)vq2;

	if (!roq::activeTable->half) {
		if (!roq::activeTable->smootheddouble) {
			//
			// normal height
			//
			if (roq::activeTable->samplesPerPixel == 2) {
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*bptr++ = roq::cin_info.yuv2rgb(y0, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(y1, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(y2, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(y3, cr, cb);
				}

				cptr = (unsigned short*)vq4;
				dptr = (unsigned short*)vq8;

				for (i = 0; i < four; i++) {
					aptr = (unsigned short*)vq2 + (*input++) * 4;
					bptr = (unsigned short*)vq2 + (*input++) * 4;
					for (j = 0; j < 2; j++)
						VQ2TO4(aptr, bptr, cptr, dptr);
				}
			}
			else if (roq::activeTable->samplesPerPixel == 4) {
				ibptr.s = bptr;
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y0, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y1, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y2, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y3, cr, cb);
				}

				icptr.s = vq4;
				idptr.s = vq8;

				for (i = 0; i < four; i++) {
					iaptr.s = vq2;
					iaptr.i += (*input++) * 4;
					ibptr.s = vq2;
					ibptr.i += (*input++) * 4;
					for (j = 0; j < 2; j++)
						VQ2TO4(iaptr.i, ibptr.i, icptr.i, idptr.i);
				}
			}
			else if (roq::activeTable->samplesPerPixel == 1) {
				bbptr = (byte*)bptr;
				for (i = 0; i < two; i++) {
					*bbptr++ = roq::activeTable->gray[*input++];
					*bbptr++ = roq::activeTable->gray[*input++];
					*bbptr++ = roq::activeTable->gray[*input++];
					*bbptr++ = roq::activeTable->gray[*input]; input += 3;
				}

				bcptr = (byte*)vq4;
				bdptr = (byte*)vq8;

				for (i = 0; i < four; i++) {
					baptr = (byte*)vq2 + (*input++) * 4;
					bbptr = (byte*)vq2 + (*input++) * 4;
					for (j = 0; j < 2; j++)
						VQ2TO4(baptr, bbptr, bcptr, bdptr);
				}
			}
		}
		else {
			//
			// double height, smoothed
			//
			if (roq::activeTable->samplesPerPixel == 2) {
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*bptr++ = roq::cin_info.yuv2rgb(y0, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(y1, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(((y0 * 3) + y2) / 4, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(((y1 * 3) + y3) / 4, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb((y0 + (y2 * 3)) / 4, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb((y1 + (y3 * 3)) / 4, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(y2, cr, cb);
					*bptr++ = roq::cin_info.yuv2rgb(y3, cr, cb);
				}

				cptr = (unsigned short*)vq4;
				dptr = (unsigned short*)vq8;

				for (i = 0; i < four; i++) {
					aptr = (unsigned short*)vq2 + (*input++) * 8;
					bptr = (unsigned short*)vq2 + (*input++) * 8;
					for (j = 0; j < 2; j++) {
						VQ2TO4(aptr, bptr, cptr, dptr);
						VQ2TO4(aptr, bptr, cptr, dptr);
					}
				}
			}
			else if (roq::activeTable->samplesPerPixel == 4) {
				ibptr.s = bptr;
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y0, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y1, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(((y0 * 3) + y2) / 4, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(((y1 * 3) + y3) / 4, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24((y0 + (y2 * 3)) / 4, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24((y1 + (y3 * 3)) / 4, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y2, cr, cb);
					*ibptr.i++ = roq::cin_info.yuv2rgb24(y3, cr, cb);
				}

				icptr.s = vq4;
				idptr.s = vq8;

				for (i = 0; i < four; i++) {
					iaptr.s = vq2;
					iaptr.i += (*input++) * 8;
					ibptr.s = vq2;
					ibptr.i += (*input++) * 8;
					for (j = 0; j < 2; j++) {
						VQ2TO4(iaptr.i, ibptr.i, icptr.i, idptr.i);
						VQ2TO4(iaptr.i, ibptr.i, icptr.i, idptr.i);
					}
				}
			}
			else if (roq::activeTable->samplesPerPixel == 1) {
				bbptr = (byte*)bptr;
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input; input += 3;
					*bbptr++ = roq::activeTable->gray[y0];
					*bbptr++ = roq::activeTable->gray[y1];
					*bbptr++ = roq::activeTable->gray[((y0 * 3) + y2) / 4];
					*bbptr++ = roq::activeTable->gray[((y1 * 3) + y3) / 4];
					*bbptr++ = roq::activeTable->gray[(y0 + (y2 * 3)) / 4];
					*bbptr++ = roq::activeTable->gray[(y1 + (y3 * 3)) / 4];
					*bbptr++ = roq::activeTable->gray[y2];
					*bbptr++ = roq::activeTable->gray[y3];
				}

				bcptr = (byte*)vq4;
				bdptr = (byte*)vq8;

				for (i = 0; i < four; i++) {
					baptr = (byte*)vq2 + (*input++) * 8;
					bbptr = (byte*)vq2 + (*input++) * 8;
					for (j = 0; j < 2; j++) {
						VQ2TO4(baptr, bbptr, bcptr, bdptr);
						VQ2TO4(baptr, bbptr, bcptr, bdptr);
					}
				}
			}
		}
	}
	else {
		//
		// 1/4 screen
		//
		if (roq::activeTable->samplesPerPixel == 2) {
			for (i = 0; i < two; i++) {
				y0 = (long)*input; input += 2;
				y2 = (long)*input; input += 2;
				cr = (long)*input++;
				cb = (long)*input++;
				*bptr++ = roq::cin_info.yuv2rgb(y0, cr, cb);
				*bptr++ = roq::cin_info.yuv2rgb(y2, cr, cb);
			}

			cptr = (unsigned short*)vq4;
			dptr = (unsigned short*)vq8;

			for (i = 0; i < four; i++) {
				aptr = (unsigned short*)vq2 + (*input++) * 2;
				bptr = (unsigned short*)vq2 + (*input++) * 2;
				for (j = 0; j < 2; j++) {
					VQ2TO2(aptr, bptr, cptr, dptr);
				}
			}
		}
		else if (roq::activeTable->samplesPerPixel == 1) {
			bbptr = (byte*)bptr;

			for (i = 0; i < two; i++) {
				*bbptr++ = roq::activeTable->gray[*input]; input += 2;
				*bbptr++ = roq::activeTable->gray[*input]; input += 4;
			}

			bcptr = (byte*)vq4;
			bdptr = (byte*)vq8;

			for (i = 0; i < four; i++) {
				baptr = (byte*)vq2 + (*input++) * 2;
				bbptr = (byte*)vq2 + (*input++) * 2;
				for (j = 0; j < 2; j++) {
					VQ2TO2(baptr, bbptr, bcptr, bdptr);
				}
			}
		}
		else if (roq::activeTable->samplesPerPixel == 4) {
			ibptr.s = bptr;
			for (i = 0; i < two; i++) {
				y0 = (long)*input; input += 2;
				y2 = (long)*input; input += 2;
				cr = (long)*input++;
				cb = (long)*input++;
				*ibptr.i++ = roq::cin_info.yuv2rgb24(y0, cr, cb);
				*ibptr.i++ = roq::cin_info.yuv2rgb24(y2, cr, cb);
			}

			icptr.s = vq4;
			idptr.s = vq8;

			for (i = 0; i < four; i++) {
				iaptr.s = vq2;
				iaptr.i += (*input++) * 2;
				ibptr.s = vq2 + (*input++) * 2;
				ibptr.i += (*input++) * 2;
				for (j = 0; j < 2; j++) {
					VQ2TO2(iaptr.i, ibptr.i, icptr.i, idptr.i);
				}
			}
		}
	}
}


/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void move8_32(byte* src, byte* dst, int spl)
{
	int i;

	for (i = 0; i < 8; ++i)
	{
		memcpy(dst, src, 32);
		src += spl;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void move4_32(byte* src, byte* dst, int spl)
{
	int i;

	for (i = 0; i < 4; ++i)
	{
		memcpy(dst, src, 16);
		src += spl;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void blit8_32(byte* src, byte* dst, int spl)
{
	int i;

	for (i = 0; i < 8; ++i)
	{
		memcpy(dst, src, 32);
		src += 32;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void blit4_32(byte* src, byte* dst, int spl)
{
	int i;

	for (i = 0; i < 4; ++i)
	{
		memmove(dst, src, 16);
		src += 16;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void blit2_32(byte* src, byte* dst, int spl)
{
	memcpy(dst, src, 8);
	memcpy(dst + spl, src + 8, 8);
}

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void blitVQQuad32fs(byte** status, unsigned char* data)
{
	unsigned short	newd, celdata, code;
	unsigned int	index, i;
	int		spl;

	//if (!status) return;

	newd = 0;
	celdata = 0;
	index = 0;

	spl = roq::activeTable->samplesPerLine;

	do {
		if (!newd) {
			newd = 7;
			celdata = data[0] + data[1] * 256;
			data += 2;
		}
		else {
			newd--;
		}

		code = (unsigned short)(celdata & 0xc000);
		celdata <<= 2;

		switch (code) {
		case ROQ_VQ_SLD:												// vq code
			blit8_32((byte*)&vq8[(*data) * 128], status[index], spl);
			data++;
			index += 5;
			break;
		case ROQ_VQ_CCC:												// drop
			index++;													// skip 8x8
			for (i = 0; i < 4; i++) {
				if (!newd) {
					newd = 7;
					celdata = data[0] + data[1] * 256;
					data += 2;
				}
				else {
					newd--;
				}

				code = (unsigned short)(celdata & 0xc000); celdata <<= 2;

				switch (code) {											// code in top two bits of code
				case ROQ_VQ_SLD:										// 4x4 vq code
					blit4_32((byte*)&vq4[(*data) * 32], status[index], spl);
					data++;
					break;
				case ROQ_VQ_CCC:										// 2x2 vq code
					blit2_32((byte*)&vq2[(*data) * 8], status[index], spl);
					data++;
					blit2_32((byte*)&vq2[(*data) * 8], status[index] + 8, spl);
					data++;
					blit2_32((byte*)&vq2[(*data) * 8], status[index] + spl * 2, spl);
					data++;
					blit2_32((byte*)&vq2[(*data) * 8], status[index] + spl * 2 + 8, spl);
					data++;
					break;
				case ROQ_VQ_FCC:										// motion compensation
					move4_32(status[index] + roq::cin_info.cin->mcomp[(*data)], status[index], spl);
					data++;
					break;
				}
				index++;
			}
			break;
		case ROQ_VQ_FCC:													// motion compensation
			move8_32(status[index] + roq::cin_info.cin->mcomp[(*data)], status[index], spl);
			data++;
			index += 5;
			break;
		case ROQ_VQ_MOT:
			index += 5;
			break;
		}
	} while (status[index] != NULL);
}

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void RoQPrepMcomp(long xoff, long yoff)
{
	long i, j, x, y, temp, temp2;

	i = roq::activeTable->samplesPerLine; j = roq::activeTable->samplesPerPixel;
	if (roq::activeTable->xsize == (roq::activeTable->ysize * 4) && !roq::activeTable->half) { j = j + j; i = i + i; }

	for (y = 0; y < 16; y++) {
		temp2 = (y + yoff - 8) * i;
		for (x = 0; x < 16; x++) {
			temp = (x + xoff - 8) * j;
			roq::cin_info.cin->mcomp[(x * 16) + y] = roq::activeTable->normalBuffer0 - (temp2 + temp);
		}
	}
}

/******************************************************************************
*
* Function:
*
* Description: ROQ
*
******************************************************************************/

static void RoQ_init(cin_cache* table)
{
	roq::activeTable = table;

	table->startTime = table->lastTime = Sys_Milliseconds() * com_timescale->value;
	table->RoQPlayed = 24;

	/*	get frame rate */
	table->roqFPS = roq::cin_info.cin->file[6] + roq::cin_info.cin->file[7] * 256;

	if (!table->roqFPS) table->roqFPS = 30;

	table->numQuads = -1;
	table->roq_id = roq::cin_info.cin->file[8] + roq::cin_info.cin->file[9] * 256;
	table->RoQFrameSize = roq::cin_info.cin->file[10] + roq::cin_info.cin->file[11] * 256 + roq::cin_info.cin->file[12] * 65536;
	table->roq_flags = roq::cin_info.cin->file[14] + roq::cin_info.cin->file[15] * 256;

	if (table->RoQFrameSize > MAX_ROQ_FRAME_SIZE || !table->RoQFrameSize)
	{
		return;
	}

	if (table->hSFX)
	{
		S_StartLocalSound(table->hSFX, CHAN_AUTO);
	}
}

void ROQ_InitSystem(cin_interface shared_data, cin_cache* table)
{
	if (!table) return;
	roq::activeTable = table;

	roq::cin_info = shared_data;

	table->VQNormal = (void (*)(byte*, void*))blitVQQuad32fs;
	table->VQBuffer = (void (*)(byte*, void*))blitVQQuad32fs;
	table->samplesPerPixel = 4;
}

void ROQ_Shutdown(void)
{
	// do nothing
}

qboolean ROQ_StartFile(cin_cache* table)
{
	roq::activeTable = table;
	FS_Read(roq::cin_info.cin->file, 16, table->iFile);

	unsigned short RoQID = (roq::cin_info.cin->file[0]) | (roq::cin_info.cin->file[1] << 8);
	if (RoQID != 0x1084) {
		Com_DPrintf("RoQDecoder: invalid RoQ ID\n");
		return qfalse;
	}

	// initialize struct
	RoQ_init(table);
	// read format data
	/*
	int counter = 0;
	table->samplesPerLine = 0;
	int tmp = roq::activeTable->RoQFrameSize;
	while (!table->samplesPerLine && counter++ < 100)
	{
		ROQ_ReadFrame(roq::cin_info.cin, table);
	}
	*/

	//readQuadInfo(roq::cin_info.cin->file + 8);
	//setupQuad(0, 0);

	table->status = FMV_PLAY;
	return qtrue;
}

/******************************************************************************
*
* Function:
*
* Description: reset video to start when looping
*
******************************************************************************/

void ROQ_Reset(cin_cache* table) {

	if (!table) return;
	roq::activeTable = table;

	FS_FCloseFile(table->iFile);
	FS_FOpenFileRead(table->fileName, &table->iFile, qtrue);
	// let the background thread start reading ahead
	FS_Read(roq::cin_info.cin->file, 16, table->iFile);
	RoQ_init(table);

	table->status = FMV_LOOPED;
}


/******************************************************************************
*
* Function: Process ROQ data frame
*
* Description: ROQ
*
******************************************************************************/

void ROQ_ReadFrame(cin_cache* table, int thisTime)
{
	int start = table->startTime;
	while ((table->tfps != table->numQuads) && (table->status == FMV_PLAY))
	{
		RoQInterrupt(table);

		if ((unsigned)start != table->startTime) {
			table->tfps = ((((Sys_Milliseconds() * com_timescale->value) - table->startTime) * table->roqFPS) / 1000);
			start = table->startTime;
		}
	}
}

void RoQInterrupt(cin_cache* table)
{
	roq::activeTable = table;
	byte* framedata;
	int ssize;

	if (!table) return;

	FS_Read(roq::cin_info.cin->file, table->RoQFrameSize + 8, table->iFile);
	if (table->RoQPlayed >= table->ROQSize) {
		if (table->holdAtEnd == qfalse) {
			if (table->looping) {
				ROQ_Reset(table);
			}
			else {
				table->status = FMV_EOF;
			}
		}
		else {
			table->status = FMV_IDLE;
		}
		return;
	}

	framedata = roq::cin_info.cin->file;
	//
	// new frame is ready
	//
redump:
	switch (table->roq_id)
	{
	case	ROQ_QUAD_VQ:
		if ((table->numQuads & 1)) {
			table->normalBuffer0 = table->t[1];
			RoQPrepMcomp(table->roqF0, table->roqF1);
			if (roq::cin_info.cin->qStatus[1]) // dirty, but works
			{
				table->VQ1((byte*)roq::cin_info.cin->qStatus[1], framedata);
			}
			table->buf = roq::cin_info.cin->linbuf + table->screenDelta;
		}
		else {
			table->normalBuffer0 = table->t[0];
			RoQPrepMcomp(table->roqF0, table->roqF1);
			if (roq::cin_info.cin->qStatus[0]) // dirty, but works
			{
				table->VQ0((byte*)roq::cin_info.cin->qStatus[0], framedata);
			}
			table->buf = roq::cin_info.cin->linbuf;
		}
		if (table->numQuads == 0) {		// first frame
			Com_Memcpy(roq::cin_info.cin->linbuf + table->screenDelta, roq::cin_info.cin->linbuf, table->samplesPerLine * table->ysize);
		}
		table->numQuads++;
		table->dirty = qtrue;
		break;
	case	ROQ_CODEBOOK:
		decodeCodeBook(framedata, (unsigned short)table->roq_flags);
		break;
	case	ZA_SOUND_MONO:
		if (!table->silent) {
			ssize = roq::cin_info.Audio_DecodeMonoToStereo(framedata, roq::soundBufferTemp, table->RoQFrameSize, 0, (unsigned short)table->roq_flags);
			S_RawSamples(ssize, 22050, 2, 1, (byte*)roq::soundBufferTemp, s_volume->value, qtrue);
		}
		break;
	case	ZA_SOUND_STEREO:
		if (!table->silent) {
			if (table->numQuads == -1) {
				S_Update();
				s_rawend = s_soundtime;
			}
			ssize = roq::cin_info.Audio_DecodeStereoToStereo(framedata, roq::soundBufferTemp, table->RoQFrameSize, 0, (unsigned short)table->roq_flags);
			S_RawSamples(ssize, 22050, 2, 2, (byte*)roq::soundBufferTemp, s_volume->value, qtrue);
		}
		break;
	case	ROQ_QUAD_INFO:
		if (table->numQuads == -1) {
			readQuadInfo(framedata);
			setupQuad(0, 0);
			table->startTime = table->lastTime = Sys_Milliseconds() * com_timescale->value;
		}
		if (table->numQuads != 1) table->numQuads = 0;
		break;
	case	ROQ_PACKET:
		table->inMemory = (qboolean)table->roq_flags;
		table->RoQFrameSize = 0;           // for header
		break;
	case	ROQ_QUAD_HANG:
		table->RoQFrameSize = 0;
		break;
	case	ROQ_QUAD_JPEG:
		break;
	default:
		table->status = FMV_EOF;
		break;
	}
	//
	// read in next frame data
	//
	if (table->RoQPlayed >= table->ROQSize) {
		if (table->holdAtEnd == qfalse) {
			if (table->looping) {
				ROQ_Reset(table);
			}
			else {
				table->status = FMV_EOF;
			}
		}
		else {
			table->status = FMV_IDLE;
		}
		return;
	}

	framedata += table->RoQFrameSize;
	table->roq_id = framedata[0] + framedata[1] * 256;
	table->RoQFrameSize = framedata[2] + framedata[3] * 256 + framedata[4] * 65536;
	table->roq_flags = framedata[6] + framedata[7] * 256;
	table->roqF0 = (signed char)framedata[7];
	table->roqF1 = (signed char)framedata[6];

	if (table->RoQFrameSize > MAX_ROQ_FRAME_SIZE || table->roq_id == 0x1084) {
		Com_DPrintf("roq_size>65536||roq_id==0x1084\n");
		table->status = FMV_EOF;
		if (table->looping) {
			ROQ_Reset(table);
		}
		return;
	}
	if (table->inMemory && (table->status != FMV_EOF))
	{
		table->inMemory = (qboolean)(((int)table->inMemory) - 1);
		framedata += 8;
		goto redump;
	}
	//
	// one more frame hits the dust
	//
	//	assert(table->RoQFrameSize <= 65536);
	//	r = FS_Read( roq::cin_info.cin->file, table->RoQFrameSize+8, table->iFile );
	table->RoQPlayed += table->RoQFrameSize + 8;
}

void ROQ_StopVideo(cin_cache* table)
{
	roq::activeTable = table;
	// Free dynamic cinematic buffers
	if (roq::cin_info.cin->linbuf) { Z_Free(roq::cin_info.cin->linbuf); roq::cin_info.cin->linbuf = NULL; roq::cin_info.cin->linbufCapacity = 0; }
	if (roq::cin_info.cin->qStatus[0]) { Z_Free(roq::cin_info.cin->qStatus[0]); roq::cin_info.cin->qStatus[0] = NULL; }
	if (roq::cin_info.cin->qStatus[1]) { Z_Free(roq::cin_info.cin->qStatus[1]); roq::cin_info.cin->qStatus[1] = NULL; }
	roq::cin_info.cin->qStatusCapacity = 0;
}

#undef _clamp