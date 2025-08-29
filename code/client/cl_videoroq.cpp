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

unsigned short				vq2[256 * 16 * 4];
unsigned short				vq4[256 * 64 * 4];
unsigned short				vq8[256 * 256 * 4];
extern int					s_soundtime;		// sample PAIRS

namespace roq {
	//cin_interface			cin_info;
	cinematics_t*			cin;
	cin_cache*				tables;
	cin_cache*				activeTable;
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

// RoQ-specific cinematic parameters
// I took parameters from original cl_cin.cpp and preserved original names of variables to make it easier to catch up for other devs
typedef struct
{
	unsigned int		RoQFrameSize; // @TODO: move to roq child struct
	unsigned int		RoQ_Id;

	void				(*VQ0)(byte* status, void* qdata);
	void				(*VQ1)(byte* status, void* qdata);
	void				(*VQNormal)(byte* status, void* qdata);
	void				(*VQBuffer)(byte* status, void* qdata);

	byte*				gray; // unused (intended for greyscale displays)
	unsigned int		maxsize;
	unsigned int		minsize;

	qboolean			half, smootheddouble, inMemory;
	long				normalBuffer0;
	long				roq_flags;
	long				roqF0;
	long				roqF1;
} cin_roq_t;

// Some preferenitions
static void RoQ_init(cin_cache* table);
void RoQInterrupt(int handle);

// ==========================================================================================================================
// ==========================================================================================================================
// ==========================================================================================================================

//-----------------------------------------------------------------------------
// RllDecodeMonoToMono
//
// Decode mono source data into a mono buffer.
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= # of shorts of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
/*
static long RllDecodeMonoToMono(unsigned char *from,short *to,unsigned int size,char signedOutput ,unsigned short flag)
{
	unsigned int z;
	int prev;

	if (signedOutput)
		prev =  flag - 0x8000;
	else
		prev = flag;

	for (z=0;z<size;z++) {
		prev = to[z] = (short)(prev + cin.sqrTable[from[z]]);
	}
	return size;	//*sizeof(short));
}
*/

//-----------------------------------------------------------------------------
// RllDecodeMonoToStereo
//
// Shared function. Decode mono source data [8bit] into a stereo buffer [16bit].
// Output is 4 times the number of bytes in the input.
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= 1/4 # of bytes of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
static long RllDecodeMonoToStereo(const byte* from, short* to, unsigned int size, char signedOutput, unsigned short flag)
{
	unsigned int z;
	int prev;

	if (signedOutput)
		prev = flag - 0x8000;
	else
		prev = flag;

	for (z = 0; z < size; z++) {
		prev = (short)(prev + roq::cin->sqrTable[from[z]]);
		to[z * 2 + 0] = to[z * 2 + 1] = (short)(prev);
	}

	return size;	// framesNum=[in]size * [channels]=2 * [sizeof(short)]=2;
}


//-----------------------------------------------------------------------------
// RllDecodeStereoToStereo
//
// Shared function. Decode stereo source data [8bit] into a stereo buffer [16bit].
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= 1/2 # of bytes of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
static long RllDecodeStereoToStereo(const byte* from, short* to, unsigned int size, char signedOutput, unsigned short flag)
{
	unsigned int z;
	const byte* zz = from;
	int	prevL, prevR;

	if (signedOutput) { // -32767..32767
		prevL = (flag & 0xff00) - 0x8000;
		prevR = ((flag & 0x00ff) << 8) - 0x8000;
	}
	else { // 0..65535
		prevL = flag & 0xff00;
		prevR = (flag & 0x00ff) << 8;
	}

	for (z = 0; z < size; z += 2) {
		prevL = (short)(prevL + roq::cin->sqrTable[*zz++]);
		prevR = (short)(prevR + roq::cin->sqrTable[*zz++]);
		to[z + 0] = (short)(prevL);
		to[z + 1] = (short)(prevR);
	}

	return (size >> 1);	//*sizeof(short));
}

/******************************************************************************
*
* Function: YUV to RGB pixel
*
* Description: shared
*
******************************************************************************/

static unsigned short yuv_to_rgb(long y, long u, long v)
{
	long r, g, b, YY = (long)(roq::cin->ROQ_YY_tab[(y)]);

	r = (YY + roq::cin->ROQ_VR_tab[v]) >> 9;
	g = (YY + roq::cin->ROQ_UG_tab[u] + roq::cin->ROQ_VG_tab[v]) >> 8;
	b = (YY + roq::cin->ROQ_UB_tab[u]) >> 9;

	r = _clamp(r, 0, 31);
	g = _clamp(g, 0, 63);
	b = _clamp(b, 0, 31);

	return (unsigned short)((r << 11) + (g << 5) + (b));
}

/******************************************************************************
*
* Function: YUV to RGB pixel
*
* Description: shared
*
******************************************************************************/

static unsigned int yuv_to_rgb24(long y, long u, long v)
{
	long r, g, b, YY = (long)(roq::cin->ROQ_YY_tab[(y)]);

	r = (YY + roq::cin->ROQ_VR_tab[v]) >> 6;
	g = (YY + roq::cin->ROQ_UG_tab[u] + roq::cin->ROQ_VG_tab[v]) >> 6;
	b = (YY + roq::cin->ROQ_UB_tab[u]) >> 6;

	r = _clamp(r, 0, 255);
	g = _clamp(g, 0, 255);
	b = _clamp(b, 0, 255);

	return LittleLong((r) | (g << 8) | (b << 16) | (255 << 24));
}

/******************************************************************************
*
* Function: recurseQuad
*
* Description: ROQ
*
******************************************************************************/

static void recurseQuad(cin_cache* table, long startX, long startY, long quadSize, long xOff, long yOff)
{
	byte* scroff;
	long bigx, bigy, lowx, lowy, useY;
	long offset;

	offset = table->screenDelta;

	lowx = lowy = 0;
	bigx = table->xsize;
	bigy = table->ysize;

	if (bigx > table->CIN_WIDTH) bigx = table->CIN_WIDTH;
	if (bigy > table->CIN_HEIGHT) bigy = table->CIN_HEIGHT;

	if ((startX >= lowx) && (startX + quadSize) <= (bigx) && (startY + quadSize) <= (bigy) && (startY >= lowy) && quadSize <= MAXSIZE) {
		useY = startY;
		scroff = roq::cin->linbuf + (useY + ((table->CIN_HEIGHT - bigy) >> 1) + yOff) * (table->samplesPerLine) + (((startX + xOff)) * table->samplesPerPixel);

		roq::cin->qStatus[0][table->onQuad] = scroff;
		roq::cin->qStatus[1][table->onQuad++] = scroff + offset;
	}

	if (quadSize != MINSIZE) {
		quadSize >>= 1;
		recurseQuad(table, startX, startY, quadSize, xOff, yOff);
		recurseQuad(table, startX + quadSize, startY, quadSize, xOff, yOff);
		recurseQuad(table, startX, startY + quadSize, quadSize, xOff, yOff);
		recurseQuad(table, startX + quadSize, startY + quadSize, quadSize, xOff, yOff);
	}
}


/******************************************************************************
*
* Function: setupQuad
*
* Description: ROQ
*
******************************************************************************/

static void setupQuad(cin_cache* table, long xOff, long yOff)
{
	long numQuadCels, i, x, y;
	byte* temp;

	if (xOff == roq::cin->oldXOff
		&& yOff == roq::cin->oldYOff
		&& table->ysize == (unsigned)roq::cin->oldysize
		&& table->xsize == (unsigned)roq::cin->oldxsize
		&& roq::cin->qStatus[0]
		&& roq::cin->qStatus[1]) {
		return;
	}

	roq::cin->oldXOff = xOff;
	roq::cin->oldYOff = yOff;
	roq::cin->oldysize = table->ysize;
	roq::cin->oldxsize = table->xsize;

	numQuadCels = (table->xsize * table->ysize) / (16);
	numQuadCels += numQuadCels / 4;
	numQuadCels += 64;							  // for overflow

	table->onQuad = 0;

	// Reallocate qStatus arrays for arbitrary frame sizes
	if (numQuadCels > roq::cin->qStatusCapacity || !roq::cin->qStatus[0] || !roq::cin->qStatus[1])
	{
		if (roq::cin->qStatus[0]) { Z_Free(roq::cin->qStatus[0]); roq::cin->qStatus[0] = NULL; }
		if (roq::cin->qStatus[1]) { Z_Free(roq::cin->qStatus[1]); roq::cin->qStatus[1] = NULL; }
		roq::cin->qStatus[0] = (byte**)Z_Malloc(sizeof(byte*) * numQuadCels, TAG_TEMP_HUNKALLOC);
		roq::cin->qStatus[1] = (byte**)Z_Malloc(sizeof(byte*) * numQuadCels, TAG_TEMP_HUNKALLOC);
		roq::cin->qStatusCapacity = numQuadCels;
	}

	for (y = 0; y < (long)table->ysize; y += 16)
		for (x = 0; x < (long)table->xsize; x += 16)
			recurseQuad(table, x, y, 16, xOff, yOff);

	temp = NULL;

	for (i = (numQuadCels - 64); i < numQuadCels; i++) {
		roq::cin->qStatus[0][i] = temp;			  // eoq
		roq::cin->qStatus[1][i] = temp;			  // eoq
	}
}


/******************************************************************************
*
* Function: readQuadInfo
*
* Description: ROQ
*
******************************************************************************/

static void readQuadInfo(cin_cache* table, byte* qData)
{
	cin_roq_t* activeDecoderSettings = (cin_roq_t*)table->decoderSettings;

	table->xsize = qData[0] + qData[1] * 256; //512
	table->ysize = qData[2] + qData[3] * 256; //512
	activeDecoderSettings->maxsize = qData[4] + qData[5] * 256; //8
	activeDecoderSettings->minsize = qData[6] + qData[7] * 256; //4

	table->CIN_HEIGHT = table->ysize;
	table->CIN_WIDTH = table->xsize;

	table->samplesPerLine = table->CIN_WIDTH * table->samplesPerPixel;
	table->screenDelta = table->CIN_HEIGHT * table->samplesPerLine;

	// Reallocate linbuf to fit arbitrary sizes
	int twoFramesBufferSize = table->screenDelta * 2; // two frames
	if (roq::cin->linbufCapacity < twoFramesBufferSize || !roq::cin->linbuf)
	{
		if (roq::cin->linbuf) { Z_Free(roq::cin->linbuf); roq::cin->linbuf = NULL; roq::cin->linbufCapacity = 0; }
		roq::cin->linbuf = (byte*)Z_Malloc(twoFramesBufferSize, TAG_TEMP_HUNKALLOC);
		roq::cin->linbufCapacity = twoFramesBufferSize;
	}
	table->buf = roq::cin->linbuf + table->screenDelta;

	activeDecoderSettings->half = qfalse;
	activeDecoderSettings->smootheddouble = qfalse;

	activeDecoderSettings->VQ0 = activeDecoderSettings->VQNormal;
	activeDecoderSettings->VQ1 = activeDecoderSettings->VQBuffer;

	table->t[0] = table->screenDelta;
	table->t[1] = -table->screenDelta;

	table->drawX = _clamp(table->CIN_WIDTH, 1, cls.glconfig.maxTextureSize);
	table->drawY = _clamp(table->CIN_HEIGHT, 1, cls.glconfig.maxTextureSize);

	// This safety check is completely unnecessary for all videocards since voodoo2
	// Unless you try to feed the game a video with resolution higher than 16K
	/*
	if (table->drawX != table->CIN_WIDTH || table->drawY != table->CIN_HEIGHT)
	{
		if (table->CIN_WIDTH != 256 || table->CIN_HEIGHT != 256) {
			Com_Printf("HACK: approxmimating cinematic for Rage Pro or Voodoo\n");
		}
		// Just set to minimum, nah
		table->drawX = 256;
		table->drawY = 256;
	}
	*/
}

/******************************************************************************
*
* Function: decodeCodeBook
*
* Description:
*
******************************************************************************/

static void decodeCodeBook(cin_cache* table, byte* input, unsigned short roq_flags)
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

	cin_roq_t* activeDecoderSettings = (cin_roq_t*)table->decoderSettings;
	if (!activeDecoderSettings->half) {
		if (!activeDecoderSettings->smootheddouble) {
			//
			// normal height
			//
			if (table->samplesPerPixel == 2) {
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*bptr++ = yuv_to_rgb(y0, cr, cb);
					*bptr++ = yuv_to_rgb(y1, cr, cb);
					*bptr++ = yuv_to_rgb(y2, cr, cb);
					*bptr++ = yuv_to_rgb(y3, cr, cb);
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
			else if (table->samplesPerPixel == 4) {
				ibptr.s = bptr;
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*ibptr.i++ = yuv_to_rgb24(y0, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(y1, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(y2, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(y3, cr, cb);
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
			else if (table->samplesPerPixel == 1) {
				bbptr = (byte*)bptr;
				for (i = 0; i < two; i++) {
					*bbptr++ = activeDecoderSettings->gray[*input++];
					*bbptr++ = activeDecoderSettings->gray[*input++];
					*bbptr++ = activeDecoderSettings->gray[*input++];
					*bbptr++ = activeDecoderSettings->gray[*input]; input += 3;
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
			if (table->samplesPerPixel == 2) {
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*bptr++ = yuv_to_rgb(y0, cr, cb);
					*bptr++ = yuv_to_rgb(y1, cr, cb);
					*bptr++ = yuv_to_rgb(((y0 * 3) + y2) / 4, cr, cb);
					*bptr++ = yuv_to_rgb(((y1 * 3) + y3) / 4, cr, cb);
					*bptr++ = yuv_to_rgb((y0 + (y2 * 3)) / 4, cr, cb);
					*bptr++ = yuv_to_rgb((y1 + (y3 * 3)) / 4, cr, cb);
					*bptr++ = yuv_to_rgb(y2, cr, cb);
					*bptr++ = yuv_to_rgb(y3, cr, cb);
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
			else if (table->samplesPerPixel == 4) {
				ibptr.s = bptr;
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*ibptr.i++ = yuv_to_rgb24(y0, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(y1, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(((y0 * 3) + y2) / 4, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(((y1 * 3) + y3) / 4, cr, cb);
					*ibptr.i++ = yuv_to_rgb24((y0 + (y2 * 3)) / 4, cr, cb);
					*ibptr.i++ = yuv_to_rgb24((y1 + (y3 * 3)) / 4, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(y2, cr, cb);
					*ibptr.i++ = yuv_to_rgb24(y3, cr, cb);
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
			else if (table->samplesPerPixel == 1) {
				bbptr = (byte*)bptr;
				for (i = 0; i < two; i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input; input += 3;
					*bbptr++ = activeDecoderSettings->gray[y0];
					*bbptr++ = activeDecoderSettings->gray[y1];
					*bbptr++ = activeDecoderSettings->gray[((y0 * 3) + y2) / 4];
					*bbptr++ = activeDecoderSettings->gray[((y1 * 3) + y3) / 4];
					*bbptr++ = activeDecoderSettings->gray[(y0 + (y2 * 3)) / 4];
					*bbptr++ = activeDecoderSettings->gray[(y1 + (y3 * 3)) / 4];
					*bbptr++ = activeDecoderSettings->gray[y2];
					*bbptr++ = activeDecoderSettings->gray[y3];
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
		if (table->samplesPerPixel == 2) {
			for (i = 0; i < two; i++) {
				y0 = (long)*input; input += 2;
				y2 = (long)*input; input += 2;
				cr = (long)*input++;
				cb = (long)*input++;
				*bptr++ = yuv_to_rgb(y0, cr, cb);
				*bptr++ = yuv_to_rgb(y2, cr, cb);
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
		else if (table->samplesPerPixel == 1) {
			bbptr = (byte*)bptr;

			for (i = 0; i < two; i++) {
				*bbptr++ = activeDecoderSettings->gray[*input]; input += 2;
				*bbptr++ = activeDecoderSettings->gray[*input]; input += 4;
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
		else if (table->samplesPerPixel == 4) {
			ibptr.s = bptr;
			for (i = 0; i < two; i++) {
				y0 = (long)*input; input += 2;
				y2 = (long)*input; input += 2;
				cr = (long)*input++;
				cb = (long)*input++;
				*ibptr.i++ = yuv_to_rgb24(y0, cr, cb);
				*ibptr.i++ = yuv_to_rgb24(y2, cr, cb);
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
* Function: move8_32
*
******************************************************************************/

static void move8_32(byte* src, byte* dst, int spl)
{
	for (int i = 0; i < 8; ++i)
	{
		memcpy(dst, src, 32);
		src += spl;
		dst += spl;
	}
}

/******************************************************************************
*
* Function: move4_32
*
******************************************************************************/

static void move4_32(byte* src, byte* dst, int spl)
{
	for (int i = 0; i < 4; ++i)
	{
		memcpy(dst, src, 16);
		src += spl;
		dst += spl;
	}
}

/******************************************************************************
*
* Function: blit8_32
*
******************************************************************************/

static void blit8_32(byte* src, byte* dst, int spl)
{
	for (int i = 0; i < 8; ++i)
	{
		memcpy(dst, src, 32);
		src += 32;
		dst += spl;
	}
}

/******************************************************************************
*
* Function: blit4_32
*
******************************************************************************/

static void blit4_32(byte* src, byte* dst, int spl)
{
	for (int i = 0; i < 4; ++i)
	{
		memmove(dst, src, 16);
		src += 16;
		dst += spl;
	}
}

/******************************************************************************
*
* Function: blit2_32
*
******************************************************************************/

static void blit2_32(byte* src, byte* dst, int spl)
{
	memcpy(dst, src, 8);
	memcpy(dst + spl, src + 8, 8);
}

/******************************************************************************
*
* Function: blitVQQuad32fs
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
					move4_32(status[index] + roq::cin->mcomp[(*data)], status[index], spl);
					data++;
					break;
				}
				index++;
			}
			break;
		case ROQ_VQ_FCC:													// motion compensation
			move8_32(status[index] + roq::cin->mcomp[(*data)], status[index], spl);
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
* Function: RoQPrepMcomp
*
******************************************************************************/

static void RoQPrepMcomp(long xoff, long yoff)
{
	long i, j, x, y, temp, temp2;

	cin_roq_t* activeDecoderSettings = (cin_roq_t*)roq::activeTable->decoderSettings;

	i = roq::activeTable->samplesPerLine; j = roq::activeTable->samplesPerPixel;
	if (roq::activeTable->xsize == (roq::activeTable->ysize * 4) && !activeDecoderSettings->half) { j = j + j; i = i + i; }

	for (y = 0; y < 16; y++) {
		temp2 = (y + yoff - 8) * i;
		for (x = 0; x < 16; x++) {
			temp = (x + xoff - 8) * j;
			roq::cin->mcomp[(x * 16) + y] = activeDecoderSettings->normalBuffer0 - (temp2 + temp);
		}
	}
}

/******************************************************************************
*
* Function: RoQ_init
*
* Description: initial RoQ setup
*
******************************************************************************/

static void RoQ_init(cin_cache* table)
{
	roq::activeTable = table;
	cin_roq_t* activeDecoderSettings = (cin_roq_t*)table->decoderSettings;

	table->startTime = table->lastTime = Sys_Milliseconds() * com_timescale->value;
	table->playedInBytes = 24;

	/*	get frame rate */
	table->decoderFPS = roq::cin->file[6] + roq::cin->file[7] * 256;

	if (!table->decoderFPS) table->decoderFPS = 30;

	table->numQuads = -1;
	activeDecoderSettings->RoQ_Id = roq::cin->file[8] + roq::cin->file[9] * 256;
	activeDecoderSettings->RoQFrameSize = roq::cin->file[10] + roq::cin->file[11] * 256 + roq::cin->file[12] * 65536;
	activeDecoderSettings->roq_flags = roq::cin->file[14] + roq::cin->file[15] * 256;

	if (activeDecoderSettings->RoQFrameSize > MAX_ROQ_FRAME_SIZE || !activeDecoderSettings->RoQFrameSize)
	{
		return;
	}

	if (table->hSFX)
	{
		S_StartLocalSound(table->hSFX, CHAN_AUTO);
	}
}

/******************************************************************************
*
* Function: ROQ_InitSystem
*
* Description: initialize references
*
******************************************************************************/

void ROQ_InitSystem(cinematics_t* cin_ptr, cin_cache* tables)
{
	roq::cin = cin_ptr;
	roq::tables = tables;
}

/******************************************************************************
*
* Function: ROQ_Shutdown
*
******************************************************************************/

void ROQ_Shutdown(void)
{
	// do nothing
}

/******************************************************************************
*
* Function: ROQ_StartFile
*
* Description: start playing new file
*
******************************************************************************/

qboolean ROQ_StartFile(int handle)
{
	if (roq::tables && handle >= 0 && handle < 16)
	{
		roq::activeTable = &roq::tables[handle];
	}
	else return qfalse;

	if (roq::activeTable->decoderSettings)
	{
		Z_Free(roq::activeTable->decoderSettings);
	}
	roq::activeTable->decoderSettings = Z_Malloc(sizeof(cin_roq_t), TAG_TEMP_WORKSPACE);
	cin_roq_t* activeDecoderSettings = (cin_roq_t*)roq::activeTable->decoderSettings;
	memset(activeDecoderSettings, 0, sizeof(cin_roq_t));

	activeDecoderSettings->VQNormal = (void (*)(byte*, void*))blitVQQuad32fs;
	activeDecoderSettings->VQBuffer = (void (*)(byte*, void*))blitVQQuad32fs;
	roq::activeTable->samplesPerPixel = 4;

	FS_Read(roq::cin->file, 16, roq::activeTable->iFile);

	unsigned short RoQID = (roq::cin->file[0]) | (roq::cin->file[1] << 8);
	if (RoQID != 0x1084) {
		Com_DPrintf("RoQDecoder: invalid RoQ ID\n");
		return qfalse;
	}

	if (roq::activeTable->audioBufferCapacity < 32768)
	{
		if (roq::activeTable->audioBuffer) Z_Free(roq::activeTable->audioBuffer);
		roq::activeTable->audioBufferCapacity = 32768;
		roq::activeTable->audioBuffer = (short*)Z_Malloc(roq::activeTable->audioBufferCapacity * sizeof(short), TAG_TEMP_WORKSPACE);
	}

	// initialize struct
	RoQ_init(roq::activeTable);
	// read format data
	/*
	int counter = 0;
	table->samplesPerLine = 0;
	int tmp = roq::activeTable->RoQFrameSize;
	while (!table->samplesPerLine && counter++ < 100)
	{
		ROQ_ReadFrame(roq::cin, table);
	}
	*/

	//readQuadInfo(roq::cin->file + 8);
	//setupQuad(0, 0);

	roq::activeTable->status = FMV_PLAY;
	return qtrue;
}

/******************************************************************************
*
* Function: ROQ_Reset
*
* Description: reset video to start when looping
*
******************************************************************************/

void ROQ_Reset(int handle) {

	if (roq::tables && handle >= 0 && handle < 16)
	{
		roq::activeTable = &roq::tables[handle];
	}
	else return;

	FS_FCloseFile(roq::activeTable->iFile);
	FS_FOpenFileRead(roq::activeTable->fileName, &roq::activeTable->iFile, qtrue);
	// let the background thread start reading ahead
	FS_Read(roq::cin->file, 16, roq::activeTable->iFile);
	RoQ_init(roq::activeTable);

	roq::activeTable->status = FMV_LOOPED;
}


/******************************************************************************
*
* Function: ROQ_ReadFrame
*
* Description: Process ROQ file to read next data frame, if needed
*
******************************************************************************/

void ROQ_ReadFrame(int handle, int thisTime)
{
	if (roq::tables && handle >= 0 && handle < 16)
	{
		roq::activeTable = &roq::tables[handle];
	}
	else return;

	int start = roq::activeTable->startTime;
	while ((roq::activeTable->tfps != roq::activeTable->numQuads) && (roq::activeTable->status == FMV_PLAY))
	{
		RoQInterrupt(handle);

		if ((unsigned)start != roq::activeTable->startTime) {
			roq::activeTable->tfps = ((((Sys_Milliseconds() * com_timescale->value) - roq::activeTable->startTime) * roq::activeTable->decoderFPS) / 1000);
			start = roq::activeTable->startTime;
		}
	}
}

/******************************************************************************
*
* Function: RoQInterrupt
*
* Description: Reading next RoQ frame
*
******************************************************************************/

void RoQInterrupt(int handle)
{
	byte* framedata;
	int ssize;

	cin_roq_t* activeDecoderSettings = (cin_roq_t*)roq::activeTable->decoderSettings;

	FS_Read(roq::cin->file, activeDecoderSettings->RoQFrameSize + 8, roq::activeTable->iFile);
	if (roq::activeTable->playedInBytes >= roq::activeTable->fileTotalSize) {
		if (roq::activeTable->holdAtEnd == qfalse) {
			if (roq::activeTable->looping) {
				ROQ_Reset(handle);
			}
			else {
				roq::activeTable->status = FMV_EOF;
			}
		}
		else {
			roq::activeTable->status = FMV_IDLE;
		}
		return;
	}

	framedata = roq::cin->file;
	//
	// new frame is ready
	//
redump:
	switch (activeDecoderSettings->RoQ_Id)
	{
	case	ROQ_QUAD_VQ:
		if (roq::activeTable->numQuads & 1) {
			activeDecoderSettings->normalBuffer0 = roq::activeTable->t[1];
			RoQPrepMcomp(activeDecoderSettings->roqF0, activeDecoderSettings->roqF1);
			if (roq::cin->qStatus[1]) // dirty, but works
			{
				activeDecoderSettings->VQ1((byte*)roq::cin->qStatus[1], framedata);
			}
			roq::activeTable->buf = roq::cin->linbuf + roq::activeTable->screenDelta;
		}
		else {
			activeDecoderSettings->normalBuffer0 = roq::activeTable->t[0];
			RoQPrepMcomp(activeDecoderSettings->roqF0, activeDecoderSettings->roqF1);
			if (roq::cin->qStatus[0]) // dirty, but works
			{
				activeDecoderSettings->VQ0((byte*)roq::cin->qStatus[0], framedata);
			}
			roq::activeTable->buf = roq::cin->linbuf;
		}
		if (roq::activeTable->numQuads == 0) {		// first frame
			Com_Memcpy(roq::cin->linbuf + roq::activeTable->screenDelta, roq::cin->linbuf, roq::activeTable->samplesPerLine * roq::activeTable->ysize);
		}
		roq::activeTable->numQuads++;
		roq::activeTable->dirty = qtrue;
		break;
	case	ROQ_CODEBOOK:
		decodeCodeBook(roq::activeTable, framedata, (unsigned short)activeDecoderSettings->roq_flags);
		break;
	case	ZA_SOUND_MONO:
		if (!roq::activeTable->silent) {
			ssize = RllDecodeMonoToStereo(framedata, roq::activeTable->audioBuffer, activeDecoderSettings->RoQFrameSize, 0, (unsigned short)activeDecoderSettings->roq_flags);
			S_RawSamples(ssize, 22050, 2, 1, (byte*)roq::activeTable->audioBuffer, s_volume->value, qtrue);
		}
		break;
	case	ZA_SOUND_STEREO:
		if (!roq::activeTable->silent) {
			if (roq::activeTable->numQuads == -1) {
				S_Update();
				s_rawend = s_soundtime;
			}
			ssize = RllDecodeStereoToStereo(framedata, roq::activeTable->audioBuffer, activeDecoderSettings->RoQFrameSize, 0, (unsigned short)activeDecoderSettings->roq_flags);
			S_RawSamples(ssize, 22050, 2, 2, (byte*)roq::activeTable->audioBuffer, s_volume->value, qtrue);
		}
		break;
	case	ROQ_QUAD_INFO:
		if (roq::activeTable->numQuads == -1) {
			readQuadInfo(roq::activeTable, framedata);
			setupQuad(roq::activeTable, 0, 0);
			roq::activeTable->startTime = roq::activeTable->lastTime = Sys_Milliseconds() * com_timescale->value;
		}
		if (roq::activeTable->numQuads != 1) roq::activeTable->numQuads = 0;
		break;
	case	ROQ_PACKET:
		activeDecoderSettings->inMemory = (qboolean)activeDecoderSettings->roq_flags;
		activeDecoderSettings->RoQFrameSize = 0;           // for header
		break;
	case	ROQ_QUAD_HANG:
		activeDecoderSettings->RoQFrameSize = 0;
		break;
	case	ROQ_QUAD_JPEG:
		break;
	default:
		roq::activeTable->status = FMV_EOF;
		break;
	}
	//
	// read in next frame data
	//
	if (roq::activeTable->playedInBytes >= roq::activeTable->fileTotalSize) {
		if (roq::activeTable->holdAtEnd == qfalse) {
			if (roq::activeTable->looping) {
				ROQ_Reset(handle);
			}
			else {
				roq::activeTable->status = FMV_EOF;
			}
		}
		else {
			roq::activeTable->status = FMV_IDLE;
		}
		return;
	}

	framedata += activeDecoderSettings->RoQFrameSize;
	activeDecoderSettings->RoQ_Id = framedata[0] + framedata[1] * 256;
	activeDecoderSettings->RoQFrameSize = framedata[2] + framedata[3] * 256 + framedata[4] * 65536;
	activeDecoderSettings->roq_flags = framedata[6] + framedata[7] * 256;
	activeDecoderSettings->roqF0 = (signed char)framedata[7];
	activeDecoderSettings->roqF1 = (signed char)framedata[6];

	if (activeDecoderSettings->RoQFrameSize > MAX_ROQ_FRAME_SIZE || activeDecoderSettings->RoQ_Id == 0x1084) {
		Com_DPrintf("roq_size>65536||roq_id==0x1084\n");
		roq::activeTable->status = FMV_EOF;
		if (roq::activeTable->looping) {
			ROQ_Reset(handle);
		}
		return;
	}
	if (activeDecoderSettings->inMemory && (roq::activeTable->status != FMV_EOF))
	{
		activeDecoderSettings->inMemory = (qboolean)(((int)activeDecoderSettings->inMemory) - 1);
		framedata += 8;
		goto redump;
	}
	//
	// one more frame hits the dust
	//
	//	assert(roq::activeTable->RoQFrameSize <= 65536);
	//	r = FS_Read( roq::cin->file, roq::activeTable->RoQFrameSize+8, roq::activeTable->iFile );
	roq::activeTable->playedInBytes += activeDecoderSettings->RoQFrameSize + 8;
}

/******************************************************************************
*
* Function: ROQ_StopVideo
*
* Description: Stop active video
*
******************************************************************************/

void ROQ_StopVideo(int handle)
{
	if (roq::tables && handle >= 0 && handle < 16)
	{
		roq::activeTable = &roq::tables[handle];
	}
	else return;

	if (roq::activeTable->decoderSettings)
	{
		Z_Free(roq::activeTable->decoderSettings);
		roq::activeTable->decoderSettings = NULL;
	}
}

/******************************************************************************
*
* Function: ROQ_DataFormatYUV
*
* Description: We don't need any additional processing of RoQ video frame
*
******************************************************************************/

qboolean ROQ_DataFormatYUV()
{
	return qfalse;
}

#undef _clamp