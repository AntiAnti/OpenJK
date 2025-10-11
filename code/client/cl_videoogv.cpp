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

#ifdef DECODER_OGV

#include "../server/exe_headers.h"

/*****************************************************************************
 * name:		cl_videoogv.c
 *
 * desc:		ogv/theora/vorbis video and audio decoder
 *
 *****************************************************************************/

#include "client.h"
#include "client_ui.h"	// CHC
#include "snd_local.h"
#include "qcommon/stringed_ingame.h"
#include <emmintrin.h>

// OGG-Vorbis-Theora
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <theora/theora.h>

#pragma warning(suppress : 6387)

#define OGG_BUFFER_SIZE		(8 * 1024)
#define OGG_PCM_SAMPLEWIDTH	2 // audio bytes per sample (16-bit audio)

// audio processing is under development, everything has to change
#define SIZEOF_RAWBUFF		(2*2 * 1024*16)
#define MIN_AUDIO_PRELOAD	200		/* 500 in ms */
#define MAX_AUDIO_PRELOAD	2800	/* 8000 ms */

#define _clamp(value, vmin, vmax) (value > vmax ? vmax : (value < vmin ? vmin : value))
#define _texture_frame_mismatch(table) (table->drawX != table->CIN_WIDTH || table->drawY != table->CIN_HEIGHT)

// ogg, theora, vorbis state
typedef struct
{
	ogg_sync_state			sync_state;			// sync incoming bitstream
	ogg_stream_state		stream_audio;		// audio stream
	ogg_stream_state		stream_video;		// video stream

	vorbis_dsp_state		v_decoder;			// central working state for the packet->PCM decoder
	vorbis_info				v_info;				// struct that stores all the static vorbis bitstream settings
	vorbis_comment			v_comment;			// struct that stores all the bitstream user comments

	theora_state			th_state;			// dump_video.c(example decoder): td
	theora_info				th_info;		   // dump_video.c(example decoder): ti
	theora_comment			th_comment;			// dump_video.c(example decoder): tc
	yuv_buffer				th_yuvbuffer;		// YUV buffer for video frame

	ogg_int64_t				VFrameCount;		// output video-stream
	ogg_int64_t				Vtime_unit;
	int						currentTime;		// input from Run-function

	// decoded audio data
	ogg_int64_t				audioQueuedPairs;	// audio samples pushed to mixed
	unsigned int			audioBufferUsed = 0;
} cin_ogv_t;

cin_ogv_t					g_ogms[16];			// OGV data
extern int					s_soundtime;		// sample PAIRS

namespace ogv {
	cinematics_t*			cin;
	cin_cache*				tables;
	cin_cache*				activeTable;		// only valid in helper functions called by interface functions
	int						activeHandle = -1;	// only valid in helper functions called by interface functions
}

// Some predefinitions
qboolean OGV_LoadBlockToSync();
int OGV_LoadPagesToStreams();
int OGV_LoadVideoFrame();
qboolean OGV_LoadAudio(cin_cache* table);
// Convert YUV420 to RGB32 using x86 SSE2 optimization (Written by Nils Liaaen Corneliusen, 2012)
void yuv420_to_argb8888(uint8_t* yp, uint8_t* up, uint8_t* vp, uint32_t sy, uint32_t suv, int width, int height, uint32_t* rgb, uint32_t srgb);

// ==========================================================================================================================
// ==========================================================================================================================
// ==========================================================================================================================

void BlitFrameToTexture(const uint8_t* src, int video_w, int video_h, uint8_t* dst, int tex_w, int tex_h, int bpp = 4 /* byte per fixel */)
{
	int pad = (tex_w - video_w) * bpp;
	int copy = video_w * bpp;

	for (int y = 0; y < video_h; y++) {
		memcpy(dst, src, copy);
		memset(dst + copy, 0, pad);
		src += copy;
		dst += tex_w * bpp;
	}

	int remain = tex_h - video_h;
	if (remain > 0) {
		memset(dst, 0, remain * tex_w * bpp);
	}
}

void OGV_BlitFrameToTexture(int frameWidth, int frameHeight)
{
	if (frameHeight > ogv::activeTable->CIN_HEIGHT) frameHeight = ogv::activeTable->CIN_HEIGHT;
	if (frameWidth != ogv::activeTable->CIN_WIDTH || frameHeight > ogv::activeTable->CIN_HEIGHT)
	{
		Com_Printf("Error: variable frame size isn't supported. Frame size: %dx%d, expected: %dx%d \n", frameWidth, frameHeight, ogv::activeTable->CIN_WIDTH, ogv::activeTable->CIN_HEIGHT);
		return;
	}

	const int bpp = ogv::activeTable->samplesPerPixel;
	int linePadBytes = (ogv::activeTable->drawX - ogv::activeTable->CIN_WIDTH) * bpp;
	if (linePadBytes <= 0)
	{
		// unexpected error
		return;
	}

	long totalOffsetBytes = (ogv::activeTable->drawX * bpp) * (frameHeight - 1);
	long totalInFrameSizeBytes = frameWidth * frameHeight * bpp;
	long newLineSize = ogv::activeTable->drawX * bpp;
	long oldLineSize = frameWidth * bpp;

	if (totalInFrameSizeBytes < totalOffsetBytes)
	{
		// no need to copy data to temp buffer
		byte* bufferSrc = ogv::activeTable->buf + (frameHeight - 1) * oldLineSize;
		byte* bufferDest = ogv::activeTable->buf + (frameHeight - 1) * newLineSize;

		for (int lineIndex = frameHeight - 1; lineIndex > 0; lineIndex--)
		{
			// copy
			memcpy(bufferDest, bufferSrc, oldLineSize);
			// clean line pad
			memset(bufferDest + oldLineSize, 0x00, linePadBytes);
			bufferSrc -= oldLineSize;
			bufferDest -= newLineSize;
		}

		if (frameHeight < ogv::activeTable->drawY)
		{
			bufferDest = ogv::activeTable->buf + frameHeight * newLineSize;
			memset(bufferDest, 0x00, newLineSize);
		}
	}
	else // need to use temp buffer
	{
		// Prepare buffer
		long expectedTempBufferSize = frameWidth * frameHeight * bpp;
		// Copy all to temp buffer
		memcpy(ogv::activeTable->frameBufferTemp, ogv::activeTable->buf, expectedTempBufferSize);
		BlitFrameToTexture(ogv::activeTable->frameBufferTemp, frameWidth, frameHeight, ogv::activeTable->buf, ogv::activeTable->drawX, ogv::activeTable->drawY, bpp);
	}
}

qboolean OGV_DataFormatYUV()
{
	// We DON'T process on GPU video rendered to texture (see CIN_UploadCinematic)
	// We can process video on GPU if we use rend2
	return (qboolean)(Q_stristr(cl_renderer->string, "rend2") != NULL);
}

/******************************************************************************
*
* Function: OGV_LoadVideoFrame
*
* Description: Decode next theora frame and convert to RGB
*
******************************************************************************/

int OGV_LoadVideoFrame()
{
	auto& g_ogm = g_ogms[ogv::activeHandle];

	ogg_packet op;
	int thisTimeA = Sys_Milliseconds();
	int cumulatedDecodeTime = 0;
	while (ogg_stream_packetout(&g_ogm.stream_video, &op) > 0)
	{
		ogg_int64_t th_frame;

		theora_decode_packetin(&g_ogm.th_state, &op);
		th_frame = theora_granule_frame(&g_ogm.th_state, g_ogm.th_state.granulepos);

		int NextNeededFrame = (int)(g_ogm.currentTime * (ogg_int64_t)10000 / g_ogm.Vtime_unit);
		if (g_ogm.VFrameCount < th_frame && th_frame >= NextNeededFrame)
		{
			// have new frame
			theora_decode_YUVout(&g_ogm.th_state, &g_ogm.th_yuvbuffer);

			// buffer for rgb
			if (ogv::activeTable->numQuads & 1)
				ogv::activeTable->buf = ogv::cin->linbuf + ogv::activeTable->screenDelta;
			else
				ogv::activeTable->buf = ogv::cin->linbuf;
			byte* out = ogv::activeTable->buf;

			int thisTimeB = Sys_Milliseconds();
			int thisTimeC = thisTimeB;

			yuv_buffer* yuv = &g_ogm.th_yuvbuffer;
			if (OGV_DataFormatYUV() && !ogv::activeTable->shader) {
				// rend2: process video frame on GPU

				// let's hope offsets are zero
				//ogg_uint32_t offset_x = g_ogm.th_info.offset_x;
				//ogg_uint32_t offset_y = g_ogm.th_info.offset_y;

				ogv::activeTable->bufY = yuv->y;
				ogv::activeTable->bufU = yuv->u;
				ogv::activeTable->bufV = yuv->v;
				ogv::activeTable->bufY_stride = yuv->y_stride;
				ogv::activeTable->bufUV_stride = yuv->uv_stride;
			}
			else {
				// vanilla renderer
				yuv420_to_argb8888(
					yuv->y,
					yuv->u,
					yuv->v,
					yuv->y_stride,
					yuv->uv_stride,
					yuv->y_width,
					yuv->y_height,
					(uint32_t*)ogv::activeTable->buf,
					yuv->y_width
				);

				thisTimeC = Sys_Milliseconds();

				if (_texture_frame_mismatch(ogv::activeTable))// && !ogv::activeTable->shader)
				{
					OGV_BlitFrameToTexture(yuv->y_width, yuv->y_height);
				}
			}

			g_ogm.VFrameCount = th_frame;
			ogv::activeTable->numQuads++;

			int thisTimeD = Sys_Milliseconds();
			// Debug
			if (thisTimeD - thisTimeA > 25) //1000 / ogv::activeTable->decoderFPS)
			{
				Com_Printf("Frame decode: %d | yuv-to-rgb32: %d | blit: %d\n", thisTimeB - thisTimeA, thisTimeC - thisTimeB, thisTimeD - thisTimeC);
			}

			return 1; // has frame
		}

		cumulatedDecodeTime += (Sys_Milliseconds() - thisTimeA);
		thisTimeA = Sys_Milliseconds();
		if (cumulatedDecodeTime > 20)
		{
			Com_Printf("Decode time over limit: %d\n", cumulatedDecodeTime);
			break;
		}
	}
	return 0; // no frame
}

static inline int AudioBufferedMs()
{
	auto& g_ogm = g_ogms[ogv::activeHandle];

	ogg_int64_t queued_pairs = (ogg_int64_t)(s_rawend - s_soundtime); // g_ogm.audioQueuedPairs - (ogg_int64_t)(s_soundtime - g_ogm.audioStartTime);
	if (queued_pairs < 0) queued_pairs = 0;
	return (int)((queued_pairs * 1000) / g_ogm.v_info.rate);
}

/******************************************************************************
*
* Function: OGV_LoadAudio
*
* Description: Decode vorbis packets into PCM and queue them
*
******************************************************************************/

qboolean OGV_LoadAudio(cin_cache* table)
{
	qboolean couldObtainSomeData = qtrue;
	float** pcm; // 32-bit data???
	int frames, frameNeeded;
	ogg_packet op;
	vorbis_block vb;

	auto& g_ogm = g_ogms[ogv::activeHandle];

	if (!g_ogm.stream_audio.serialno)
	{
		// don't have audio stream
		return qfalse;
	}
	if (table->silent)
	{
		// don't need audio
		return qfalse;
	}

	int SamplesInBufferMs = (int)g_ogm.audioBufferUsed * 1000 /* ms in s */ / (int)(g_ogm.v_info.channels /* 2 */ * (int)g_ogm.v_info.rate /* 22050 */);

	Com_Memset(&op, 0, sizeof(op));
	Com_Memset(&vb, 0, sizeof(vb));
	vorbis_block_init(&g_ogm.v_decoder, &vb);

	while (couldObtainSomeData && (SamplesInBufferMs < MAX_AUDIO_PRELOAD))
	{
		couldObtainSomeData = qfalse;
		int RemainingMsInBuffer = AudioBufferedMs();

		// if there's pending, decoded audio, grab it 
		frames = vorbis_synthesis_pcmout(&g_ogm.v_decoder, &pcm);
		if (frames > 0)
		{
			frameNeeded = (SIZEOF_RAWBUFF - g_ogm.audioBufferUsed) / (OGG_PCM_SAMPLEWIDTH * g_ogm.v_info.channels);
			if (frameNeeded == 0) continue;
			if (frames < frameNeeded)
			{
				frameNeeded = frames;
			}

			// convert 32bit audio to 16bit 
			short* ptr = &table->audioBuffer[g_ogm.audioBufferUsed];
			for (int i = 0; i < frameNeeded; i++)
			{
				for (int j = 0; j < g_ogm.v_info.channels; j++)
				{
					*(ptr++) = (short)(_clamp(pcm[j][i], -1.f, 1.f) * 32767.f);
				}
			}

			if (g_ogm.audioQueuedPairs == 0) {
				S_Update();
				s_rawend = s_soundtime;
			}

			//S_RawSamples(frameNeeded, (int)g_ogm.v_info.rate, OGG_PCM_SAMPLEWIDTH, g_ogm.v_info.channels, (byte*)table->audioBuffer, s_volume->value, qtrue);
			g_ogm.audioQueuedPairs += frameNeeded;
			g_ogm.audioBufferUsed += (frameNeeded * g_ogm.v_info.channels);
			SamplesInBufferMs = (int)g_ogm.audioBufferUsed * 1000 / (int)(g_ogm.v_info.channels * (int)g_ogm.v_info.rate);

			// tell libvorbis how many samples we actually consumed (we ate them all!)
			vorbis_synthesis_read(&g_ogm.v_decoder, frameNeeded);
			couldObtainSomeData = qtrue;
		}
		else
		{
			// no pending audio; is there a pending packet to decode?
			if (ogg_stream_packetout(&g_ogm.stream_audio, &op))
			{
				if (vorbis_synthesis(&vb, &op) == 0)
				{
					vorbis_synthesis_blockin(&g_ogm.v_decoder, &vb);
				}
				couldObtainSomeData = qtrue;
			}
		}
	}

	vorbis_block_clear(&vb);
	
	return (qboolean)(SamplesInBufferMs < MIN_AUDIO_PRELOAD);
	/*
	while (couldObtainSomeData && ((g_ogm.currentTime + MAX_AUDIO_PRELOAD) > (int)(g_ogm.v_decoder.granulepos * 1000 / g_ogm.v_info.rate)))
	{
		couldObtainSomeData = qfalse;
		int RemainingMsInBuffer = AudioBufferedMs();

		// if there's pending, decoded audio, grab it
		frames = vorbis_synthesis_pcmout(&g_ogm.v_decoder, &pcm);
		if (frames > 0)
		{
			frameNeeded = SIZEOF_RAWBUFF / (OGG_PCM_SAMPLEWIDTH * g_ogm.v_info.channels);
			if (frames < frameNeeded)
			{
				frameNeeded = frames;
			}

			// convert 32bit audio to 16bit 
			short* ptr = &table->audioBuffer[g_ogm.audioBufferUsed];
			for (int i = 0; i < frameNeeded; i++)
			{
				for (int j = 0; j < g_ogm.v_info.channels; j++)
				{
					*(ptr++) = (short)(_clamp(pcm[j][i], -1.f, 1.f) * 32767.f);
				}
			}

			if (g_ogm.audioQueuedPairs == 0) {
				S_Update();
				s_rawend = s_soundtime;
			}

			//S_RawSamples(frameNeeded, (int)g_ogm.v_info.rate, OGG_PCM_SAMPLEWIDTH, g_ogm.v_info.channels, (byte*)table->audioBuffer, s_volume->value, qtrue);
			g_ogm.audioQueuedPairs += frameNeeded;
			g_ogm.audioBufferUsed += (frameNeeded * g_ogm.v_info.channels);

			// tell libvorbis how many samples we actually consumed (we ate them all!)
			vorbis_synthesis_read(&g_ogm.v_decoder, frameNeeded);
			couldObtainSomeData = qtrue;
		}
		else
		{
			// no pending audio; is there a pending packet to decode?
			if (ogg_stream_packetout(&g_ogm.stream_audio, &op))
			{
				if (vorbis_synthesis(&vb, &op) == 0)
				{
					vorbis_synthesis_blockin(&g_ogm.v_decoder, &vb);
				}
				couldObtainSomeData = qtrue;
			}
		}
	}

	vorbis_block_clear(&vb);

	return (qboolean)(g_ogm.currentTime + MIN_AUDIO_PRELOAD > (int)(g_ogm.v_decoder.granulepos * 1000 / g_ogm.v_info.rate));
	*/
}

/******************************************************************************
*
* Function: OGV_LoadBlockToSync
*
* Description:!0 -> no data transferred
*
******************************************************************************/

qboolean OGV_LoadBlockToSync()
{
	int  r = -1;
	char* buffer;
	int  bytes;

	auto& g_ogm = g_ogms[ogv::activeHandle];

	if (ogv::activeTable->iFile)
	{
		buffer = ogg_sync_buffer(&g_ogm.sync_state, OGG_BUFFER_SIZE);
		bytes = FS_Read(buffer /*ogv::cin_info.cin->file*/, OGG_BUFFER_SIZE, ogv::activeTable->iFile);
		ogv::activeTable->playedInBytes += OGG_BUFFER_SIZE;
		ogg_sync_wrote(&g_ogm.sync_state, bytes);

		r = (bytes == 0);
	}

	return (qboolean)r;
}

/******************************************************************************
*
* Function: OGV_LoadPagesToStreams
*
 * @param cin - unused
 * @return !0 -> no data transferred (or not for all Streams)
*
******************************************************************************/

int OGV_LoadPagesToStreams()
{
	int              r = -1;
	int              AudioPages = 0;
	int              VideoPages = 0;
	ogg_stream_state* osptr = NULL;
	ogg_page         og;

	auto& g_ogm = g_ogms[ogv::activeHandle];

	while (!AudioPages || !VideoPages)
	{
		if (ogg_sync_pageout(&g_ogm.sync_state, &og) != 1)
		{
			break;
		}

		if (g_ogm.stream_audio.serialno == ogg_page_serialno(&og))
		{
			osptr = &g_ogm.stream_audio;
			++AudioPages;
		}
		if (g_ogm.stream_video.serialno == ogg_page_serialno(&og))
		{
			osptr = &g_ogm.stream_video;
			++VideoPages;
		}

		if (osptr != NULL)
		{
			ogg_stream_pagein(osptr, &og);
		}
	}

	if (AudioPages || VideoPages)
	{
		r = 0;
	}

	return r;
}

/******************************************************************************
*
* Function: OGV_InitSystem
*
* Description: Initialize references to cl_cin
*
******************************************************************************/

void OGV_InitSystem(cinematics_t* cin_ptr, cin_cache* tables)
{
	ogv::cin = cin_ptr;
	ogv::tables = tables;
}

/******************************************************************************
*
* Function: OGV_Shutdown
*
* Description: 
*
******************************************************************************/

void OGV_Shutdown(void)
{
}

/******************************************************************************
*
* Function: OGV_StartFile
*
* Description: Load header of OGV file
*
******************************************************************************/

qboolean OGV_StartFile(int handle)
{
	int        status;
	ogg_page   og;
	ogg_packet op;
	int        i;

	if (ogv::tables && handle >= 0 && handle < 16)
	{
		ogv::activeTable = &ogv::tables[handle];
		ogv::activeHandle = handle;
	}
	else return qfalse;

	auto& g_ogm = g_ogms[handle];
	cin_cache* activeTable = &ogv::tables[handle];

	activeTable->numQuads = -1;
	activeTable->playedInBytes = 0;

	memset(&g_ogm, 0, sizeof(cin_ogv_t));
	
	ogg_sync_init(&g_ogm.sync_state);

	while (!g_ogm.stream_audio.serialno || !g_ogm.stream_video.serialno) {
		if (ogg_sync_pageout(&g_ogm.sync_state, &og) == 1) {
			if (strstr((char*)(og.body + 1), "vorbis"))
			{
				//FIXME? better way to find audio stream
				if (g_ogm.stream_audio.serialno) {
					Com_Printf(S_COLOR_YELLOW "WARNING: more than one audio stream, in the ogv file!\n");
				}
				else {
					ogg_stream_init(&g_ogm.stream_audio, ogg_page_serialno(&og));
					ogg_stream_pagein(&g_ogm.stream_audio, &og);
				}
			}
			if (strstr((char*)(og.body + 1), "theora"))
			{
				if (g_ogm.stream_video.serialno) {
					Com_Printf(S_COLOR_YELLOW "WARNING: more than one video stream, in the ogv file!\n");
				}
				else {
					ogg_stream_init(&g_ogm.stream_video, ogg_page_serialno(&og));
					ogg_stream_pagein(&g_ogm.stream_video, &og);
				}
			}
		}
		else if (OGV_LoadBlockToSync()) {
			break;
		}
	}

	if (!g_ogm.stream_audio.serialno) {
		Com_Printf(S_COLOR_YELLOW "WARNING: Haven't found a audio (vorbis) stream in ogm-file!\n");
	}

	if (!g_ogm.stream_video.serialno)
	{
		Com_Printf(S_COLOR_YELLOW "WARNING: Haven't found a video (theora) stream in ogm-file!\n");
		return qfalse;
	}

	// load vorbis header
	if (g_ogm.stream_audio.serialno)
	{
		vorbis_info_init(&g_ogm.v_info);
		vorbis_comment_init(&g_ogm.v_comment);
		i = 0;
		while (i < 3) {
			status = ogg_stream_packetout(&g_ogm.stream_audio, &op);
			if (status < 0) {
				Com_Printf(S_COLOR_YELLOW "WARNING: Corrupt ogg packet while loading vorbis-headers\n");
				return qfalse;
			}
			if (status > 0) {
				status = vorbis_synthesis_headerin(&g_ogm.v_info, &g_ogm.v_comment, &op);
				if (i == 0 && status < 0)
				{
					Com_Printf(S_COLOR_YELLOW "WARNING: This Ogg bitstream does not contain Vorbis audio data\n");
					return qfalse;
				}
				++i;
			}
			else if (OGV_LoadPagesToStreams()) {
				if (OGV_LoadBlockToSync()) {
					Com_Printf(S_COLOR_YELLOW "WARNING: Couldn't find all vorbis headers before end of the ofv file\n");
					return qfalse;
				}
			}
		}

		vorbis_synthesis_init(&g_ogm.v_decoder, &g_ogm.v_info);
	}

	// Load theora header
	theora_info_init(&g_ogm.th_info);
	theora_comment_init(&g_ogm.th_comment);
	i = 0;
	while (i < 3) {
		status = ogg_stream_packetout(&g_ogm.stream_video, &op);
		if (status < 0) {
			Com_Printf(S_COLOR_YELLOW "WARNING: Corrupt ogg packet while loading theora-headers\n");
			return qfalse;
		}
		else if (status > 0) {
			status = theora_decode_header(&g_ogm.th_info, &g_ogm.th_comment, &op);
			if (i == 0 && status != 0) {
				Com_Printf(S_COLOR_YELLOW "WARNING: This Ogg bitstream does not contain theora data\n");
				return qfalse;
			}
			++i;
		}
		else if (OGV_LoadPagesToStreams())
		{
			if (OGV_LoadBlockToSync())
			{
				Com_Printf(S_COLOR_YELLOW "WARNING: Couldn't find all theora headers before end of the file\n");
				return qfalse;
			}
		}
	}
	// init theora decoder
	theora_decode_init(&g_ogm.th_state, &g_ogm.th_info);

	// init table
	g_ogm.Vtime_unit = ((ogg_int64_t)g_ogm.th_info.fps_denominator * 1000 * 10000 / g_ogm.th_info.fps_numerator);

	activeTable->startTime = activeTable->lastTime = Sys_Milliseconds() * com_timescale->value;

	/*	get frame rate */
	activeTable->decoderFPS = (g_ogm.th_info.fps_denominator > 0)
		? (long)((double)g_ogm.th_info.fps_numerator / (double)g_ogm.th_info.fps_denominator)
		: 30;

	if (activeTable->hSFX)
	{
		S_StartLocalSound(activeTable->hSFX, CHAN_AUTO);
	}

	g_ogm.audioQueuedPairs = 0;
	g_ogm.audioBufferUsed = 0;
	activeTable->xsize = activeTable->drawX = g_ogm.th_info.frame_width;
	activeTable->ysize = activeTable->drawY = g_ogm.th_info.frame_height;
	activeTable->CIN_WIDTH = activeTable->xsize;
	activeTable->CIN_HEIGHT = activeTable->ysize;

	// We support now non-square video, but texture size must be in powers of two
	bool bInitW = true, bInitH = true;
	for (int texture_size = 64; texture_size <= cls.glconfig.maxTextureSize; texture_size *= 2)
	{
		if (bInitW && texture_size >= activeTable->CIN_WIDTH) {
			activeTable->drawX = texture_size; bInitW = false;
		}
		if (bInitH && texture_size >= activeTable->CIN_HEIGHT) {
			activeTable->drawY = texture_size; bInitH = false;
		}
		if (!bInitW && !bInitH) break;
	}

	activeTable->samplesPerLine = activeTable->drawX * activeTable->samplesPerPixel;
	activeTable->screenDelta = activeTable->drawY * activeTable->samplesPerLine;

	// Reallocate linbuf to fit arbitrary sizes
	int twoFramesBufferSize = activeTable->screenDelta * 2; // two frames
	if (ogv::cin->linbufCapacity < twoFramesBufferSize || !ogv::cin->linbuf)
	{
		if (ogv::cin->linbuf) { Z_Free(ogv::cin->linbuf); ogv::cin->linbuf = NULL; ogv::cin->linbufCapacity = 0; }
		ogv::cin->linbuf = (byte*)Z_Malloc(twoFramesBufferSize, TAG_TEMP_HUNKALLOC);
		ogv::cin->linbufCapacity = twoFramesBufferSize;
	}
	activeTable->buf = ogv::cin->linbuf + activeTable->screenDelta;

	activeTable->t[0] = activeTable->screenDelta;
	activeTable->t[1] = -activeTable->screenDelta;

	// This safety check is completely unnecessary for all videocards since voodoo2
	// Unless you try to feed the game a video with resolution higher than 16K
	activeTable->drawX = _clamp(activeTable->drawX, 1, cls.glconfig.maxTextureSize);
	activeTable->drawY = _clamp(activeTable->drawY, 1, cls.glconfig.maxTextureSize);
	if (_texture_frame_mismatch(activeTable))
	{
		// prepare temp buffer for BlitFrameToTexture
		long totalOffsetBytes = (activeTable->drawX * activeTable->samplesPerPixel) * (activeTable->CIN_HEIGHT - 1);
		long totalInFrameSizeBytes = (activeTable->CIN_WIDTH + 16 /* keep pad for alignment */) * activeTable->CIN_HEIGHT * activeTable->samplesPerPixel;
		// do we even need a buffer to copy frame
		if (totalInFrameSizeBytes < totalOffsetBytes)
		{
			long expectedTempBufferSize = totalInFrameSizeBytes;
			if (activeTable->frameBufferTempSize < expectedTempBufferSize)
			{
				if (activeTable->frameBufferTemp) Z_Free(activeTable->frameBufferTemp);
				activeTable->frameBufferTemp = (byte*)Z_Malloc(activeTable->frameBufferTempSize, TAG_TEMP_HUNKALLOC);
				activeTable->frameBufferTempSize = expectedTempBufferSize;
			}
		}
	}

	if (activeTable->audioBufferCapacity < SIZEOF_RAWBUFF * 2)
	{
		if (activeTable->audioBuffer) Z_Free(activeTable->audioBuffer);
		activeTable->audioBufferCapacity = SIZEOF_RAWBUFF * 2;
		activeTable->audioBuffer = (short*)Z_Malloc(activeTable->audioBufferCapacity*sizeof(short), TAG_TEMP_WORKSPACE);
	}

	activeTable->status = FMV_PLAY;

	return qtrue;
}

/******************************************************************************
*
* Function: OGV_Reset
*
* Description: Restart video (used for looping)
*
******************************************************************************/

void OGV_Reset(int handle)
{
	if (ogv::tables && handle >= 0 && handle < 16)
	{
		ogv::activeTable = &ogv::tables[handle];
		ogv::activeHandle = handle;
	}
	else return;

	FS_FCloseFile(ogv::activeTable->iFile);
	FS_FOpenFileRead(ogv::activeTable->fileName, &ogv::activeTable->iFile, qtrue);
	OGV_StartFile(handle);
	
	ogv::activeTable->status = FMV_LOOPED;
}

qboolean OGVInterrupt(cin_cache* table)
{
	bool anyDataTransferred = true;
	qboolean needVOutputData = qtrue;
	qboolean audioWantsMoreData = qfalse;
	int status;

	auto& g_ogm = g_ogms[ogv::activeHandle];

	// bad, should rewrite

	while (anyDataTransferred && (needVOutputData || audioWantsMoreData))
	{
		anyDataTransferred = qfalse;

		if (needVOutputData && (status = OGV_LoadVideoFrame())) {
			needVOutputData = qfalse;
			anyDataTransferred = (qboolean)(status > 0);
		}

		if (needVOutputData || audioWantsMoreData) {
			// try to transfer Pages to the audio- and video-Stream
			if (OGV_LoadPagesToStreams() != 0) {
				// try to load a datablock from file
				anyDataTransferred |= !(bool)OGV_LoadBlockToSync();
			}
			else {
				// successful OGV_LoadPagesToStreams()
				anyDataTransferred = qtrue;
			}
		}

		// load all Audio after loading new pages ...
		if (g_ogm.VFrameCount > 0)  // wait some videoframes (it's better to have some delay, than a laggy sound)
		{
			audioWantsMoreData = OGV_LoadAudio(table);
		}
	}

	return (qboolean)(anyDataTransferred || status);
}

/******************************************************************************
*
* Function: OGV_ReadFrame
*
* Description: Read next frame if needed
*
******************************************************************************/

void OGV_ReadFrame(int handle, int timeNow)
{
	if (ogv::tables && handle >= 0 && handle < 16)
	{
		ogv::activeTable = &ogv::tables[handle];
		ogv::activeHandle = handle;
	}
	else return;

	auto& g_ogm = g_ogms[ogv::activeHandle];
	cin_cache* table = &ogv::tables[handle];

	if (!table->startTime)
	{
		table->startTime = timeNow;
	}

	g_ogm.currentTime = timeNow - table->startTime;
	timeNow = timeNow - table->startTime + 20;

	table->dirty = qfalse;

	while ((!g_ogm.VFrameCount || timeNow >= (int)(g_ogm.VFrameCount * g_ogm.Vtime_unit / 10000)) && table->status == FMV_PLAY)
	{
		table->dirty = qtrue;
		if (!OGVInterrupt(table))
		{
			// EOF reached
			Com_DPrintf("eof reached\n");
			if (table->holdAtEnd == qfalse) {
				if (table->looping) {
					OGV_Reset(handle);
				}
				else {
					table->status = FMV_EOF;
				}
			}
			else {
				table->status = FMV_IDLE;
			}
		}
	}

	// Need to push audio?
	const int framesPerSecond = g_ogm.v_info.channels * g_ogm.v_info.rate;
	int bufferedAudioInEngineMs = (s_rawend - s_soundtime) * 1000 / g_ogm.v_info.rate;

	if ((bufferedAudioInEngineMs < 150 && g_ogm.audioBufferUsed > 0) || ((g_ogm.audioBufferUsed / framesPerSecond) > 2000)) // || g_ogm.audioBufferUsed  > framesPerSecond) // || g_ogm.audioBufferUsed * 1000 / framesPerSecond > MAX_AUDIO_PRELOAD
	{
		S_RawSamples(g_ogm.audioBufferUsed / g_ogm.v_info.channels, (int)g_ogm.v_info.rate, OGG_PCM_SAMPLEWIDTH, g_ogm.v_info.channels, (byte*)table->audioBuffer, s_volume->value, qtrue);

		//Com_Printf("remaining sound samples: %d | push %d samples\n", bufferedAudioInEngineMs, (g_ogm.audioBufferUsed * 1000 / framesPerSecond));

		g_ogm.audioBufferUsed = 0;
	}
	else
	{
		//Com_Printf("remaining sound samples: %d\n", bufferedAudioInEngineMs);
	}	
}

/******************************************************************************
*
* Function: OGV_StopVideo
*
* Description: stop playing video
*
******************************************************************************/

void OGV_StopVideo(int handle)
{
	if (ogv::tables && handle >= 0 && handle < 16)
	{
		ogv::activeTable = &ogv::tables[handle];
		ogv::activeHandle = handle;
	}

	auto& g_ogm = g_ogms[ogv::activeHandle];

	if (ogv::activeTable->frameBufferTemp)
	{
		Z_Free(ogv::activeTable->frameBufferTemp);
		ogv::activeTable->frameBufferTemp = NULL;
		ogv::activeTable->frameBufferTempSize = 0;
	}

	ogv::activeTable->bufU = ogv::activeTable->bufV = ogv::activeTable->bufY = NULL;

	// Cleanup
	theora_clear(&g_ogm.th_state);
	theora_comment_clear(&g_ogm.th_comment);
	theora_info_clear(&g_ogm.th_info);
	vorbis_dsp_clear(&g_ogm.v_decoder);
	vorbis_comment_clear(&g_ogm.v_comment);
	vorbis_info_clear(&g_ogm.v_info);
	ogg_stream_clear(&g_ogm.stream_audio);
	ogg_stream_clear(&g_ogm.stream_video);
	ogg_sync_clear(&g_ogm.sync_state);
}

// YUV to RGB with SSE2
// Written by Nils Liaaen Corneliusen 2012.
// License: CC0 1.0 Universal (CC0 1.0) Public Domain Dedication license
// https://www.ignorantus.com
// Note: I swapped R and B bytes in output
void yuv420_to_argb8888(uint8_t* yp, uint8_t* up, uint8_t* vp,
	uint32_t sy, uint32_t suv,
	int width, int height,
	uint32_t* rgb, uint32_t srgb /* in uint32_t, not bytes, i. e. just width */)
{
	__m128i y0r0, y0r1, u0, v0;
	__m128i y00r0, y01r0, y00r1, y01r1;
	__m128i u00, u01, v00, v01;
	__m128i rv00, rv01, gu00, gu01, gv00, gv01, bu00, bu01;
	__m128i r00, r01, g00, g01, b00, b01;
	__m128i rgb0123, rgb4567, rgb89ab, rgbcdef;
	__m128i gbgb;
	__m128i ysub, uvsub;
	__m128i zero, facy, facrv, facgu, facgv, facbu;
	__m128i* srcy128r0, * srcy128r1;
	__m128i* dstrgb128r0, * dstrgb128r1;
	__m64* srcu64, * srcv64;
	int x, y;

	ysub = _mm_set1_epi32(0x00100010);
	uvsub = _mm_set1_epi32(0x00800080);

	facy = _mm_set1_epi32(0x004a004a);
	facrv = _mm_set1_epi32(0x00660066);
	facgu = _mm_set1_epi32(0x00190019);
	facgv = _mm_set1_epi32(0x00340034);
	facbu = _mm_set1_epi32(0x00810081);

	zero = _mm_set1_epi32(0x00000000);

	for (y = 0; y < height; y += 2) {

		srcy128r0 = (__m128i*)(yp + sy * y);
		srcy128r1 = (__m128i*)(yp + sy * y + sy);
		srcu64 = (__m64*)(up + suv * (y / 2));
		srcv64 = (__m64*)(vp + suv * (y / 2));

		dstrgb128r0 = (__m128i*)(rgb + srgb * y);
		dstrgb128r1 = (__m128i*)(rgb + srgb * y + srgb);

		for (x = 0; x < width; x += 16) {

			u0 = _mm_loadl_epi64((__m128i*)srcu64); srcu64++;
			v0 = _mm_loadl_epi64((__m128i*)srcv64); srcv64++;

			y0r0 = _mm_load_si128(srcy128r0++);
			y0r1 = _mm_load_si128(srcy128r1++);

			// constant y factors
			y00r0 = _mm_mullo_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(y0r0, zero), ysub), facy);
			y01r0 = _mm_mullo_epi16(_mm_sub_epi16(_mm_unpackhi_epi8(y0r0, zero), ysub), facy);
			y00r1 = _mm_mullo_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(y0r1, zero), ysub), facy);
			y01r1 = _mm_mullo_epi16(_mm_sub_epi16(_mm_unpackhi_epi8(y0r1, zero), ysub), facy);

			// expand u and v so they're aligned with y values
			u0 = _mm_unpacklo_epi8(u0, zero);
			u00 = _mm_sub_epi16(_mm_unpacklo_epi16(u0, u0), uvsub);
			u01 = _mm_sub_epi16(_mm_unpackhi_epi16(u0, u0), uvsub);

			v0 = _mm_unpacklo_epi8(v0, zero);
			v00 = _mm_sub_epi16(_mm_unpacklo_epi16(v0, v0), uvsub);
			v01 = _mm_sub_epi16(_mm_unpackhi_epi16(v0, v0), uvsub);

			// common factors on both rows.
			rv00 = _mm_mullo_epi16(facrv, v00);
			rv01 = _mm_mullo_epi16(facrv, v01);
			gu00 = _mm_mullo_epi16(facgu, u00);
			gu01 = _mm_mullo_epi16(facgu, u01);
			gv00 = _mm_mullo_epi16(facgv, v00);
			gv01 = _mm_mullo_epi16(facgv, v01);
			bu00 = _mm_mullo_epi16(facbu, u00);
			bu01 = _mm_mullo_epi16(facbu, u01);

			// row 0
			r00 = _mm_srai_epi16(_mm_add_epi16(y00r0, rv00), 6);
			r01 = _mm_srai_epi16(_mm_add_epi16(y01r0, rv01), 6);
			g00 = _mm_srai_epi16(_mm_sub_epi16(_mm_sub_epi16(y00r0, gu00), gv00), 6);
			g01 = _mm_srai_epi16(_mm_sub_epi16(_mm_sub_epi16(y01r0, gu01), gv01), 6);
			b00 = _mm_srai_epi16(_mm_add_epi16(y00r0, bu00), 6);
			b01 = _mm_srai_epi16(_mm_add_epi16(y01r0, bu01), 6);

			r00 = _mm_packus_epi16(r00, r01);         // rrrr.. saturated
			g00 = _mm_packus_epi16(g00, g01);         // gggg.. saturated
			b00 = _mm_packus_epi16(b00, b01);         // bbbb.. saturated

			// SWAPPED: Use b00 where r00 was used, and r00 where b00 was used
			b01 = _mm_unpacklo_epi8(b00, zero); // 0b0b.. (blue where red was)
			gbgb = _mm_unpacklo_epi8(r00, g00);  // rgrg.. (red where blue was)
			rgb0123 = _mm_unpacklo_epi16(gbgb, b01);  // 0bgr0bgr.. -> RGBΑ
			rgb4567 = _mm_unpackhi_epi16(gbgb, b01);  // 0bgr0bgr..

			b01 = _mm_unpackhi_epi8(b00, zero);
			gbgb = _mm_unpackhi_epi8(r00, g00);
			rgb89ab = _mm_unpacklo_epi16(gbgb, b01);
			rgbcdef = _mm_unpackhi_epi16(gbgb, b01);

			_mm_store_si128(dstrgb128r0++, rgb0123);
			_mm_store_si128(dstrgb128r0++, rgb4567);
			_mm_store_si128(dstrgb128r0++, rgb89ab);
			_mm_store_si128(dstrgb128r0++, rgbcdef);

			// row 1
			r00 = _mm_srai_epi16(_mm_add_epi16(y00r1, rv00), 6);
			r01 = _mm_srai_epi16(_mm_add_epi16(y01r1, rv01), 6);
			g00 = _mm_srai_epi16(_mm_sub_epi16(_mm_sub_epi16(y00r1, gu00), gv00), 6);
			g01 = _mm_srai_epi16(_mm_sub_epi16(_mm_sub_epi16(y01r1, gu01), gv01), 6);
			b00 = _mm_srai_epi16(_mm_add_epi16(y00r1, bu00), 6);
			b01 = _mm_srai_epi16(_mm_add_epi16(y01r1, bu01), 6);

			r00 = _mm_packus_epi16(r00, r01);         // rrrr.. saturated
			g00 = _mm_packus_epi16(g00, g01);         // gggg.. saturated
			b00 = _mm_packus_epi16(b00, b01);         // bbbb.. saturated

			// SWAPPED: Use b00 where r00 was used, and r00 where b00 was used
			b01 = _mm_unpacklo_epi8(b00, zero); // 0b0b.. (blue where red was)
			gbgb = _mm_unpacklo_epi8(r00, g00);  // rgrg.. (red where blue was)
			rgb0123 = _mm_unpacklo_epi16(gbgb, b01);  // 0bgr0bgr.. -> RGBΑ
			rgb4567 = _mm_unpackhi_epi16(gbgb, b01);  // 0bgr0bgr..

			b01 = _mm_unpackhi_epi8(b00, zero);
			gbgb = _mm_unpackhi_epi8(r00, g00);
			rgb89ab = _mm_unpacklo_epi16(gbgb, b01);
			rgbcdef = _mm_unpackhi_epi16(gbgb, b01);

			_mm_store_si128(dstrgb128r1++, rgb0123);
			_mm_store_si128(dstrgb128r1++, rgb4567);
			_mm_store_si128(dstrgb128r1++, rgb89ab);
			_mm_store_si128(dstrgb128r1++, rgbcdef);
		}
	}
}


#undef _clamp

#endif