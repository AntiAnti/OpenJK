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

#include <malloc.h>

#pragma warning(suppress : 6387)

#define OGG_BUFFER_SIZE		(8 * 1024)
#define OGG_PCM_SAMPLEWIDTH	2 // audio bytes per sample (16-bit audio)

// audio processing is under developing, everything should change
#define SIZEOF_RAWBUFF		(2*2 * 1024*16)
#define MIN_AUDIO_PRELOAD	500		/* 200 in ms */
#define MAX_AUDIO_PRELOAD	8000	/*4096*/

#define _clamp(value, vmin, vmax) (value > vmax ? vmax : (value < vmin ? vmin : value))
#define _texture_frame_mismatch(table) (table->drawX != table->CIN_WIDTH || table->drawY != table->CIN_HEIGHT)

// ogg, theora, vorbis state
typedef struct
{
	ogg_sync_state			sync_state;		// sync incoming bitstream
	ogg_stream_state		stream_audio;	// audio stream
	ogg_stream_state		stream_video;	// video stream

	vorbis_dsp_state		v_decoder;		// central working state for the packet->PCM decoder
	vorbis_info				v_info;			// struct that stores all the static vorbis bitstream settings
	vorbis_comment			v_comment;		// struct that stores all the bitstream user comments

	theora_state			th_state;		// dump_video.c(example decoder): td
	theora_info				th_info;        // dump_video.c(example decoder): ti
	theora_comment			th_comment;		// dump_video.c(example decoder): tc
	yuv_buffer				th_yuvbuffer;	// YUV buffer for video frame

	ogg_int64_t				VFrameCount;	// output video-stream
	ogg_int64_t				Vtime_unit;
	int						currentTime;	// input from Run-function

	// decoded audio dataaudio 
	short					audioBuffer[SIZEOF_RAWBUFF * 2];
	ogg_int64_t				audioQueuedPairs;		// audio samples pushed to mixed
	int						audioStartTime = 0;		// in ms
	// processing video frame
	byte*					frameBufferTemp = NULL;	// used in BlitFrameToTexture frameBufferTemp[2048 * 2048 * 4];
	int						frameBufferTempSize = 0;
} cin_ogv_t;

cin_ogv_t					g_ogm;			// OGV data
extern int					s_soundtime;	// sample PAIRS
//alignas(16) uint8_t		YUVa[3840 * 2160 * 4]; // 4K buffer for one frame YUV-RGB converstion

namespace ogv {
	cin_interface			cin_info;
	cin_cache*				activeTable;
}

// predefinitions
qboolean OGV_LoadBlockToSync();
int OGV_LoadPagesToStreams();
int OGV_LoadVideoFrame();
qboolean OGV_LoadAudio();
// I'd really, really want to move YUV processing to renderer
void yuv420_to_argb8888(uint8_t* yp, uint8_t* up, uint8_t* vp,
	uint32_t sy, uint32_t suv,
	int width, int height,
	uint32_t* rgb, uint32_t srgb);
//static void OGV_YUV2ARGB(const byte* yuv, int width, int height, byte* rgba_out);

//=============================================================

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

	unsigned long totalOffsetBytes = (ogv::activeTable->drawX * bpp) * (frameHeight - 1);
	unsigned long totalInFrameSizeBytes = frameWidth * frameHeight * bpp;
	unsigned long newLineSize = ogv::activeTable->drawX * bpp;
	unsigned long oldLineSize = frameWidth * bpp;

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
	}
	else // need to use temp buffer
	{
		// Prepare buffer
		long expectedTempBufferSize = frameWidth * frameHeight * bpp;
		// Copy all to temp buffer
		memcpy(g_ogm.frameBufferTemp, ogv::activeTable->buf, expectedTempBufferSize);
		BlitFrameToTexture(g_ogm.frameBufferTemp, frameWidth, frameHeight, ogv::activeTable->buf, ogv::activeTable->drawX, ogv::activeTable->drawY, bpp);
	}
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
	ogg_packet op;
	while (ogg_stream_packetout(&g_ogm.stream_video, &op) > 0)
	{
		//int thisTimeA = Sys_Milliseconds();
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
				ogv::activeTable->buf = ogv::cin_info.cin->linbuf + ogv::activeTable->screenDelta;
			else
				ogv::activeTable->buf = ogv::cin_info.cin->linbuf;
			byte* out = ogv::activeTable->buf;

			//int thisTimeB = Sys_Milliseconds();

			yuv_buffer* yuv = &g_ogm.th_yuvbuffer;
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

			//OGV_YUV2ARGB((const byte*)g_ogm.th_yuvbuffer.y, g_ogm.th_yuvbuffer.y_width, g_ogm.th_yuvbuffer.y_height, ogv::activeTable->buf);

			if (_texture_frame_mismatch(ogv::activeTable))
			{
				OGV_BlitFrameToTexture(yuv->y_width, yuv->y_height);
			}

			g_ogm.VFrameCount = th_frame;
			ogv::activeTable->numQuads++;

			//int thisTimeC = Sys_Milliseconds();
			//Com_Printf("Frame decode: %d | yuv-to-rgb32: %d\n", thisTimeB - thisTimeA, thisTimeC - thisTimeB);

			return 1; // has frame
		}
	}
	return 0; // no frame
}

static inline int AudioBufferedMs()
{
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

qboolean OGV_LoadAudio()
{
	qboolean couldObtainSomeData = qtrue;
	float** pcm; // 32-bit data???
	int frames, frameNeeded;
	ogg_packet op;
	vorbis_block vb;

	if (ogv::activeTable->silent)
	{
		// don't need audio
		return qfalse;
	}

	Com_Memset(&op, 0, sizeof(op));
	Com_Memset(&vb, 0, sizeof(vb));
	vorbis_block_init(&g_ogm.v_decoder, &vb);

	int prevSamples = g_ogm.audioQueuedPairs;

	//while (couldObtainSomeData && AudioBufferedMs() < MAX_AUDIO_PRELOAD)
	while (couldObtainSomeData && ((g_ogm.currentTime + MAX_AUDIO_PRELOAD) > (int)(g_ogm.v_decoder.granulepos * 1000 / g_ogm.v_info.rate)))
	{
		couldObtainSomeData = qfalse;
		int RemainingMsInBuffer = AudioBufferedMs();

		/* if there's pending, decoded audio, grab it */
		frames = vorbis_synthesis_pcmout(&g_ogm.v_decoder, &pcm);
		if (frames > 0)
		{
			frameNeeded = SIZEOF_RAWBUFF / (OGG_PCM_SAMPLEWIDTH * g_ogm.v_info.channels);
			if (frames < frameNeeded)
			{
				frameNeeded = frames;
			}

			// convert 32bit audio to 16bit 
			short* ptr = (short*)g_ogm.audioBuffer;
			for (int i = 0; i < frameNeeded; i++)
			{
				for (int j = 0; j < g_ogm.v_info.channels; j++)
				{
					*(ptr++) = (short)(_clamp(pcm[j][i], -1.f, 1.f) * 32767.f);
				}
			}

			// debug
			if (RemainingMsInBuffer < 100)
			{
				Com_Printf("s_soundtime: %d | s_rawend: %d | audioQueuedPairs: %d | RemainingMsInBuffer: %d\n", s_soundtime, s_rawend, g_ogm.audioQueuedPairs, RemainingMsInBuffer);
			}

			if (g_ogm.audioQueuedPairs == 0) {
				S_Update();
				s_rawend = s_soundtime;
				g_ogm.audioStartTime = s_soundtime;
			}

			S_RawSamples(frameNeeded, (int)g_ogm.v_info.rate, OGG_PCM_SAMPLEWIDTH, g_ogm.v_info.channels, (byte*)g_ogm.audioBuffer, s_volume->value, qtrue);
			g_ogm.audioQueuedPairs += frameNeeded;

			// tell libvorbis how many samples we actually consumed (we ate them all!)
			vorbis_synthesis_read(&g_ogm.v_decoder, frameNeeded);
			couldObtainSomeData = qtrue;
		}
		else
		{
			/* no pending audio; is there a pending packet to decode? */
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
	//return (qboolean)(AudioBufferedMs() < MIN_AUDIO_PRELOAD);
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

	if (ogv::activeTable->iFile)
	{
		buffer = ogg_sync_buffer(&g_ogm.sync_state, OGG_BUFFER_SIZE);
		bytes = FS_Read(buffer /*ogv::cin_info.cin->file*/, OGG_BUFFER_SIZE, ogv::activeTable->iFile);
		ogv::activeTable->RoQPlayed += OGG_BUFFER_SIZE;
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

	if (AudioPages && VideoPages)
	{
		r = 0;
	}

	return r;
}

//===================================================================================

void OGV_InitSystem(cin_interface shared_data, cin_cache* table)
{
	ogv::cin_info = shared_data;
	ogv::activeTable = table;
}

void OGV_Shutdown(void)
{
	if (ogv::activeTable)
	{
		OGV_StopVideo(ogv::activeTable);
	}
}

/******************************************************************************
*
* Function: OGV_StartFile
*
* Description: Load header of OGV file
*
******************************************************************************/

qboolean OGV_StartFile(cin_cache* table)
{
	int        status;
	ogg_page   og;
	ogg_packet op;
	int        i;

	table->numQuads = -1;
	table->RoQPlayed = 0;
	//FS_Read(ogv::cin_info.cin->file, 16, table->iFile);
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
		return qfalse;
	}

	if (!g_ogm.stream_video.serialno)
	{
		Com_Printf(S_COLOR_YELLOW "WARNING: Haven't found a video (theora) stream in ogm-file!\n");
		return qfalse;
	}

	// load vorbis header
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

	ogv::activeTable->startTime = table->lastTime = Sys_Milliseconds() * com_timescale->value;

	/*	get frame rate */
	table->roqFPS = (g_ogm.th_info.fps_denominator > 0)
		? (long)((double)g_ogm.th_info.fps_numerator / (double)g_ogm.th_info.fps_denominator)
		: 30;

	if (table->hSFX)
	{
		S_StartLocalSound(table->hSFX, CHAN_AUTO);
	}

	g_ogm.audioQueuedPairs = 0;
	g_ogm.audioStartTime = s_soundtime;
	ogv::activeTable->xsize = ogv::activeTable->drawX = g_ogm.th_info.frame_width;
	ogv::activeTable->ysize = ogv::activeTable->drawY = g_ogm.th_info.frame_height;
	ogv::activeTable->CIN_WIDTH = ogv::activeTable->xsize;
	ogv::activeTable->CIN_HEIGHT = ogv::activeTable->ysize;

	// We support now non-square video, but texture size must be in powers of two
	bool bInitW = true, bInitH = true;
	for (int texture_size = 64; texture_size <= cls.glconfig.maxTextureSize; texture_size *= 2)
	{
		if (bInitW && texture_size >= ogv::activeTable->CIN_WIDTH) {
			ogv::activeTable->drawX = texture_size; bInitW = false;
		}
		if (bInitH && texture_size >= ogv::activeTable->CIN_HEIGHT) {
			ogv::activeTable->drawY = texture_size; bInitH = false;
		}
		if (!bInitW && !bInitH) break;
	}

	ogv::activeTable->samplesPerLine = ogv::activeTable->drawX * ogv::activeTable->samplesPerPixel;
	ogv::activeTable->screenDelta = ogv::activeTable->drawY * ogv::activeTable->samplesPerLine;

	// Reallocate linbuf to fit arbitrary sizes
	int twoFramesBufferSize = ogv::activeTable->screenDelta * 2; // two frames
	if (ogv::cin_info.cin->linbufCapacity < twoFramesBufferSize || !ogv::cin_info.cin->linbuf)
	{
		if (ogv::cin_info.cin->linbuf) { Z_Free(ogv::cin_info.cin->linbuf); ogv::cin_info.cin->linbuf = NULL; ogv::cin_info.cin->linbufCapacity = 0; }
		ogv::cin_info.cin->linbuf = (byte*)Z_Malloc(twoFramesBufferSize, TAG_TEMP_HUNKALLOC);
		ogv::cin_info.cin->linbufCapacity = twoFramesBufferSize;
	}
	ogv::activeTable->buf = ogv::cin_info.cin->linbuf + ogv::activeTable->screenDelta;

	ogv::activeTable->half = qfalse;
	ogv::activeTable->smootheddouble = qfalse;

	ogv::activeTable->t[0] = ogv::activeTable->screenDelta;
	ogv::activeTable->t[1] = -ogv::activeTable->screenDelta;

	// This safety check is completely unnecessary for all videocards since voodoo2
	// Unless you try to feed the game a video with resolution higher than 16K
	ogv::activeTable->drawX = _clamp(ogv::activeTable->drawX, 1, cls.glconfig.maxTextureSize);
	ogv::activeTable->drawY = _clamp(ogv::activeTable->drawY, 1, cls.glconfig.maxTextureSize);
	if (_texture_frame_mismatch(ogv::activeTable))
	{
		if (ogv::activeTable->CIN_WIDTH != 256 || ogv::activeTable->CIN_HEIGHT != 256) {
			Com_Printf("HACK: approxmimating cinematic for Rage Pro or Voodoo\n");
		}

		// prepare temp buffer for BlitFrameToTexture
		long totalOffsetBytes = (ogv::activeTable->drawX * ogv::activeTable->samplesPerPixel) * (ogv::activeTable->CIN_HEIGHT - 1);
		long totalInFrameSizeBytes = ogv::activeTable->CIN_WIDTH * ogv::activeTable->CIN_HEIGHT * ogv::activeTable->samplesPerPixel;
		// do we even need a buffer to copy frame
		if (totalInFrameSizeBytes < totalOffsetBytes)
		{
			long expectedTempBufferSize = totalInFrameSizeBytes;
			if (g_ogm.frameBufferTempSize < expectedTempBufferSize)
			{
				if (g_ogm.frameBufferTemp) Z_Free(g_ogm.frameBufferTemp);
				g_ogm.frameBufferTemp = (byte*)Z_Malloc(g_ogm.frameBufferTempSize, TAG_TEMP_HUNKALLOC);
				g_ogm.frameBufferTempSize = expectedTempBufferSize;
			}
		}
	}

	table->status = FMV_PLAY;

	return qtrue;
}

void OGV_Reset(cin_cache* table)
{
	if (!table) return;
	ogv::activeTable = table;

	FS_FCloseFile(table->iFile);
	FS_FOpenFileRead(table->fileName, &table->iFile, qtrue);
	OGV_StartFile(table);
	
	table->status = FMV_LOOPED;
}

qboolean OGVInterrupt()
{
	bool anyDataTransferred = true;
	qboolean needVOutputData = qtrue;
	qboolean audioWantsMoreData = qfalse;
	int status;

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
			audioWantsMoreData = OGV_LoadAudio();
		}
	}

	return (qboolean)anyDataTransferred;
}

void OGV_ReadFrame(cin_cache* table, int timeNow)
{
	ogv::activeTable = table;

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
		if (!OGVInterrupt())
		{
			// EOF reached
			Com_DPrintf("eof reached\n");
			if (table->holdAtEnd == qfalse) {
				if (table->looping) {
					OGV_Reset(table);
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
}

void OGV_StopVideo(cin_cache* table)
{
	ogv::activeTable = table;

	// Probably should move it to cl_cin.cpp
	if (ogv::cin_info.cin->linbuf) { Z_Free(ogv::cin_info.cin->linbuf); ogv::cin_info.cin->linbuf = NULL; ogv::cin_info.cin->linbufCapacity = 0; }

	if (g_ogm.frameBufferTemp)
	{
		Z_Free(g_ogm.frameBufferTemp);
		g_ogm.frameBufferTemp = NULL;
		g_ogm.frameBufferTempSize = 0;
	}

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

// YUV to RGB SSE2
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