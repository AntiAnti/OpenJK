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
 * name:		cl_cin.c
 *
 * desc:		video and cinematic playback
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

// Keep video aspect ratio
// May be replaced with r_ratioFix cvar
#define ASPECT_RATIO_FIX	1
// If qtrue, stretch video height to screen height while keeping aspect ratio
// If qfalse, stretch video to screen width and add negative vertical offset to keep it at the center
// qfalse is preferred to hide back bars which already exist in game videos
#define ASPECT_RATIO_STRETCH_TO_HEIGHT qfalse

#define _clamp(value, vmin, vmax) (value > vmax ? vmax : (value < vmin ? vmin : value))

#define MAX_VIDEO_HANDLES	16

extern void S_CIN_StopSound(sfxHandle_t sfxHandle);
static void CIN_StopVideo(cinematics_t* cin, cin_cache* table);

/******************************************************************************
*
* Class:		trFMV
*
* Description:	RoQ/RnR manipulation routines
*				not entirely complete for first run
*
******************************************************************************/

extern qboolean S_FileExists(const char* psFilename);
static unsigned short yuv_to_rgb(long y, long u, long v);
static unsigned int yuv_to_rgb24(long y, long u, long v);
cinVideoFormat ProcessVideoFileName(char* outFileName, int outSize, const char* inFileName, qboolean bShader);

/*
* ================================================================
* Decoder interface
* ================================================================
*/
typedef struct {
	void (*Init)(cinematics_t* cin_ptr, cin_cache* tables);
	void (*Shutdown)();
	qboolean(*Start)(int handle);
	void (*ReadFrame)(int handle, int timeNow);
	void (*ResetToStart)(int handle);
	void (*Stop)(int handle);
	qboolean(*DataFormatYUV)(); // qtrue if YUV, qfalse if RGB32
} videoDecoder;

static videoDecoder videoDecoders[] =
{
	{ ROQ_InitSystem, ROQ_Shutdown, ROQ_StartFile, ROQ_ReadFrame, ROQ_Reset, ROQ_StopVideo, ROQ_DataFormatYUV },
#ifdef DECODER_OGV
	{ OGV_InitSystem, OGV_Shutdown, OGV_StartFile, OGV_ReadFrame, OGV_Reset, OGV_StopVideo, OGV_DataFormatYUV },
#endif
};

/*
* ================================================================
* End of decoder interface
* ================================================================
*/

static cinematics_t		cin;
static cin_cache		cinTable[MAX_VIDEO_HANDLES];
static int				currentHandle = -1;
static int				CL_handle = -1;
static int				CL_iPlaybackStartTime;	// so I can stop users quitting playback <1 second after it starts

extern int				s_soundtime;		// sample PAIRS
extern int   			s_paintedtime; 		// sample PAIRS

static int				nScreenRatioFixOffset = 0; // Can remove now, since I swtiched to JAEnhanced algorithm and strtch video to screen width

void CIN_CloseAllVideos(void) {
	int		i;

	for ( i = 0 ; i < MAX_VIDEO_HANDLES ; i++ ) {
		if (cinTable[i].fileName[0] != 0 ) {
			CIN_StopCinematic(i);
		}
	}
	if (cin.linbuf) { Z_Free(cin.linbuf); cin.linbuf = NULL; cin.linbufCapacity = 0; }
	if (cin.qStatus[0]) { Z_Free(cin.qStatus[0]); cin.qStatus[0] = NULL; }
	if (cin.qStatus[1]) { Z_Free(cin.qStatus[1]); cin.qStatus[1] = NULL; }
	cin.qStatusCapacity = 0;
}


static int CIN_HandleForVideo(void) {
	int		i;

	for ( i = 0 ; i < MAX_VIDEO_HANDLES ; i++ ) {
		if ( cinTable[i].fileName[0] == 0 ) {
			return i;
		}
	}
	Com_Error( ERR_DROP, "CIN_HandleForVideo: none free" );
	return -1;
}


/******************************************************************************
*
* Function:
*
* Description: shared
*
******************************************************************************/

static void ROQ_GenYUVTables(void)
{
	float t_ub, t_vr, t_ug, t_vg;
	long i;

	t_ub = (1.77200f / 2.0f) * (float)(1 << 6) + 0.5f;
	t_vr = (1.40200f / 2.0f) * (float)(1 << 6) + 0.5f;
	t_ug = (0.34414f / 2.0f) * (float)(1 << 6) + 0.5f;
	t_vg = (0.71414f / 2.0f) * (float)(1 << 6) + 0.5f;
	for (i = 0; i < 256; i++) {
		float x = (float)(2 * i - 255);

		cin.ROQ_UB_tab[i] = (long)((t_ub * x) + (1 << 5));
		cin.ROQ_VR_tab[i] = (long)((t_vr * x) + (1 << 5));
		cin.ROQ_UG_tab[i] = (long)((-t_ug * x));
		cin.ROQ_VG_tab[i] = (long)((-t_vg * x) + (1 << 5));
		cin.ROQ_YY_tab[i] = (long)((i << 6) | (i >> 2));
	}
}

//-----------------------------------------------------------------------------
// RllSetupTable
//
// Shared function. Allocates and initializes the square table.
//
// Parameters:	None
//
// Returns:		Nothing
//-----------------------------------------------------------------------------
static void RllSetupTable( void )
{
	int z;

	for (z=0;z<128;z++) {
		cin.sqrTable[z] = (short)(z*z);
		cin.sqrTable[z+128] = (short)(-cin.sqrTable[z]);
	}
}

//-----------------------------------------------------------------------------
// RllDecodeStereoToMono
//
// Decode stereo source data into a mono buffer.
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= # of bytes of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
/*
static long RllDecodeStereoToMono(unsigned char *from,short *to,unsigned int size,char signedOutput, unsigned short flag)
{
	unsigned int z;
	int prevL,prevR;

	if (signedOutput) {
		prevL = (flag & 0xff00) - 0x8000;
		prevR = ((flag & 0x00ff) << 8) -0x8000;
	} else {
		prevL = flag & 0xff00;
		prevR = (flag & 0x00ff) << 8;
	}

	for (z=0;z<size;z+=1) {
		prevL= prevL + cin.sqrTable[from[z*2]];
		prevR = prevR + cin.sqrTable[from[z*2+1]];
		to[z] = (short)((prevL + prevR)/2);
	}

	return size;
}
*/

/******************************************************************************
*
* Function:
*
* Description: Globally initialize ROQ
*
******************************************************************************/

static void InitYUVTables(void)
{
	// Currently used only for ROQ
	ROQ_GenYUVTables();
	RllSetupTable();
}

/*
==================
CIN_StopCinematic
==================
*/

e_status CIN_StopCinematic(int handle) {

	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return FMV_EOF;
	currentHandle = handle;

	Com_DPrintf("trFMV::stop(), closing %s\n", cinTable[currentHandle].fileName);

	if (!cinTable[currentHandle].buf) {
		if (cinTable[currentHandle].iFile) {
//			assert( 0 && "ROQ handle leak-prevention WAS needed!");
			FS_FCloseFile( cinTable[currentHandle].iFile );
			cinTable[currentHandle].iFile = 0;
			cinTable[currentHandle].fileName[0] = 0;
			if (cinTable[currentHandle].hSFX) {
				S_CIN_StopSound( cinTable[currentHandle].hSFX );
			}
		}
		return FMV_EOF;
	}

	if (cinTable[currentHandle].alterGameState) {
		if ( cls.state != CA_CINEMATIC ) {
			return cinTable[currentHandle].status;
		}
	}
	cinTable[currentHandle].status = FMV_EOF;
	CIN_StopVideo(&cin, &cinTable[currentHandle]);

	return FMV_EOF;
}

/*
==================
SCR_RunCinematic

Fetch and decompress the pending frame
==================
*/


e_status CIN_RunCinematic (int handle)
{
	int	start = 0;
	int thisTime = 0;

	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return FMV_EOF;

	if (cin.currentHandle != handle) {
		currentHandle = handle;
		cin.currentHandle = currentHandle;
		cinTable[currentHandle].status = FMV_EOF;
		videoDecoders[cinTable[currentHandle].videoFormat].ResetToStart(currentHandle);
	}

	if (cinTable[handle].playonwalls < -1)
	{
		return cinTable[handle].status;
	}

	currentHandle = handle;

	if (cinTable[currentHandle].alterGameState) {
		if ( cls.state != CA_CINEMATIC ) {
			return cinTable[currentHandle].status;
		}
	}

	if (cinTable[currentHandle].status == FMV_IDLE) {
		return cinTable[currentHandle].status;
	}

	thisTime = Sys_Milliseconds()*com_timescale->value;
	if (cinTable[currentHandle].shader && (abs(thisTime - (double)cinTable[currentHandle].lastTime))>100) {
		cinTable[currentHandle].startTime += thisTime - cinTable[currentHandle].lastTime;
	}
	cinTable[currentHandle].tfps = ((((Sys_Milliseconds()*com_timescale->value) - cinTable[currentHandle].startTime)*cinTable[currentHandle].DecoderFPS)/1000);

	// process frame using active decoder
	videoDecoders[cinTable[currentHandle].videoFormat].ReadFrame(currentHandle, thisTime);

	cinTable[currentHandle].lastTime = thisTime;

	if (cinTable[currentHandle].status == FMV_LOOPED) {
		cinTable[currentHandle].status = FMV_PLAY;
	}

	if (cinTable[currentHandle].status == FMV_EOF) {
		if (cinTable[currentHandle].looping) {
			videoDecoders[cinTable[currentHandle].videoFormat].ResetToStart(currentHandle);
		} else {
			CIN_StopVideo(&cin, &cinTable[currentHandle]);
		}
	}

	return cinTable[currentHandle].status;
}

void Menus_CloseAll(void);
void UI_Cursor_Show(qboolean flag);

/*
==================
CL_PlayCinematic

==================
*/
int CIN_PlayCinematic( const char *arg, int x, int y, int w, int h, int systemBits, const char *psAudioFile /* = NULL */ )
{
	char	name[MAX_OSPATH];
	int		i;

	qboolean bShader = (qboolean)((systemBits & CIN_shader) != 0);
	cinVideoFormat Format = ProcessVideoFileName(name, MAX_OSPATH, arg, bShader);

	if (!(systemBits & CIN_system)) {
		for ( i = 0 ; i < MAX_VIDEO_HANDLES ; i++ ) {
			if (!strcmp(cinTable[i].fileName, name) ) {
				return i;
			}
		}
	}

	Com_DPrintf("CIN_PlayCinematic( %s )\n", arg);

	memset(&cin, 0, sizeof(cinematics_t) );
	currentHandle = CIN_HandleForVideo();

	cin.currentHandle = currentHandle;

	Q_strncpyz(cinTable[currentHandle].fileName, name, MAX_OSPATH);

	cinTable[currentHandle].videoFormat = Format;
	cinTable[currentHandle].fileTotalSize = 0;
	cinTable[currentHandle].fileTotalSize = FS_FOpenFileRead (cinTable[currentHandle].fileName, &cinTable[currentHandle].iFile, qtrue);

	if (cinTable[currentHandle].fileTotalSize <= 0) {
		Com_Printf(S_COLOR_RED"ERROR: playCinematic: %s not found!\n", arg);
		cinTable[currentHandle].fileName[0] = 0;
		return -1;
	}

	CIN_SetExtents(currentHandle, x, y, w, h);
	CIN_SetLooping(currentHandle, (qboolean)((systemBits & CIN_loop) != 0));

	cinTable[currentHandle].CIN_HEIGHT = 512; // placeholder, will read actual value from file
	cinTable[currentHandle].CIN_WIDTH  = 512; // placeholder, will read actual value from file
	cinTable[currentHandle].holdAtEnd = (qboolean)((systemBits & CIN_hold) != 0);
	cinTable[currentHandle].alterGameState = (qboolean)((systemBits & CIN_system) != 0);
	cinTable[currentHandle].playonwalls = 1;
	cinTable[currentHandle].silent = (qboolean)((systemBits & CIN_silent) != 0);
	cinTable[currentHandle].shader = bShader;
	if (psAudioFile)
	{
		cinTable[currentHandle].hSFX = S_RegisterSound(psAudioFile);
	}
	else
	{
		cinTable[currentHandle].hSFX = 0;
	}
	cinTable[currentHandle].hCRAWLTEXT = 0;

	if (cinTable[currentHandle].alterGameState)
	{
		// close the menu
		Con_Close();
		if (cls.uiStarted)
		{
			UI_Cursor_Show(qfalse);
			Menus_CloseAll();
		}
	}
	else
	{
		cinTable[currentHandle].playonwalls = cl_inGameVideo->integer;
	}

	// Ensure shared tables are initialzed
	InitYUVTables();
	// Ensure decoder is initialized
	videoDecoders[Format].Init(&cin, cinTable);
	// load video container header
	if (videoDecoders[Format].Start(currentHandle))
	{
		//RoQ_init();
		cinTable[currentHandle].status = FMV_PLAY;
		Com_DPrintf("trFMV::play(), playing %s\n", arg);

		if (cinTable[currentHandle].alterGameState) {
			cls.state = CA_CINEMATIC;
		}

		// Ensure console is closed
		Con_Close();

		if (!cinTable[currentHandle].silent)
		{
			s_rawend = s_soundtime;
		}

		return currentHandle;
	}
	Com_DPrintf("trFMV::play(), invalid RoQ ID\n");

	CIN_StopVideo(&cin, &cinTable[currentHandle]);
	return -1;
}

void CIN_SetExtents (int handle, int x, int y, int w, int h) {
	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return;
	cinTable[handle].xpos = x;
	cinTable[handle].ypos = y;
	cinTable[handle].width = w;
	cinTable[handle].height = h;
	cinTable[handle].dirty = qtrue;
}

void CIN_SetLooping(int handle, qboolean loop) {
	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return;
	cinTable[handle].looping = loop;
}

// Text crawl defines
#define TC_PLANE_WIDTH	250
#define TC_PLANE_NEAR	90
#define TC_PLANE_FAR	715
#define TC_PLANE_TOP	0
#define TC_PLANE_BOTTOM	1100

#define TC_DELAY 9000
#define TC_STOPTIME 81000
static void CIN_AddTextCrawl()
{
	refdef_t	refdef;
	polyVert_t	verts[4];

	// Set up refdef
	memset( &refdef, 0, sizeof( refdef ));

	refdef.rdflags = RDF_NOWORLDMODEL;
	AxisClear( refdef.viewaxis );

	refdef.fov_x = 130;
	refdef.fov_y = 130;

	refdef.x = 0;
	refdef.y = -50;
	refdef.width = cls.glconfig.vidWidth;
	refdef.height = cls.glconfig.vidHeight * 2; // deliberately extend off the bottom of the screen

	// use to set shaderTime for scrolling shaders
	refdef.time = 0;

	// Set up the poly verts
	float fadeDown = 1.0;
	if (cls.realtime-CL_iPlaybackStartTime >= (TC_STOPTIME-2500))
	{
		fadeDown = (TC_STOPTIME - (cls.realtime-CL_iPlaybackStartTime))/ 2480.0f;
		if (fadeDown < 0)
		{
			fadeDown = 0;
		}
		if (fadeDown > 1)
		{
			fadeDown = 1;
		}
	}
	for ( int i = 0; i < 4; i++ )
	{
		verts[i].modulate[0] = 255*fadeDown; // gold color?
		verts[i].modulate[1] = 235*fadeDown;
		verts[i].modulate[2] = 127*fadeDown;
		verts[i].modulate[3] = 255*fadeDown;
	}

	VectorScaleM( verts[2].modulate, 0.1f, verts[2].modulate ); // darken at the top??
	VectorScaleM( verts[3].modulate, 0.1f, verts[3].modulate );

#define TIMEOFFSET  +(cls.realtime-CL_iPlaybackStartTime-TC_DELAY)*0.000015f -1
	VectorSet( verts[0].xyz, TC_PLANE_NEAR, -TC_PLANE_WIDTH, TC_PLANE_TOP );
	verts[0].st[0] = 1;
	verts[0].st[1] = 1 TIMEOFFSET;

	VectorSet( verts[1].xyz, TC_PLANE_NEAR, TC_PLANE_WIDTH, TC_PLANE_TOP );
	verts[1].st[0] = 0;
	verts[1].st[1] = 1 TIMEOFFSET;

	VectorSet( verts[2].xyz, TC_PLANE_FAR, TC_PLANE_WIDTH, TC_PLANE_BOTTOM );
	verts[2].st[0] = 0;
	verts[2].st[1] = 0 TIMEOFFSET;

	VectorSet( verts[3].xyz, TC_PLANE_FAR, -TC_PLANE_WIDTH, TC_PLANE_BOTTOM );
	verts[3].st[0] = 1;
	verts[3].st[1] = 0 TIMEOFFSET;

	// render it out
	re.ClearScene();
	re.AddPolyToScene(cinTable[CL_handle].hCRAWLTEXT, 4, verts, 1);
	re.RenderScene(&refdef);

	//time's up
	if (cls.realtime-CL_iPlaybackStartTime >= TC_STOPTIME)
	{
		cinTable[CL_handle].status = FMV_EOF;
		CIN_StopVideo(&cin, &cinTable[currentHandle]);
		SCR_StopCinematic();	// change ROQ from FMV_IDLE to FMV_EOF, and clear some other vars
	}
}

/*
==================
CIN_ResampleCinematic

Resample cinematic to 256x256 and store in buf2
==================
*/
void CIN_ResampleCinematic(int handle, int *buf2) {
	int ix, iy, *buf3, xm, ym, ll;
	byte	*buf;

	buf = cinTable[handle].buf;

	xm = cinTable[handle].CIN_WIDTH/256;
	ym = cinTable[handle].CIN_HEIGHT/256;
	ll = 8;
	// Why? Let's say so.
	if (cinTable[handle].CIN_WIDTH == 512 || cinTable[handle].CIN_WIDTH == 2048 || cinTable[handle].CIN_WIDTH == 1024) {
		ll = 9;
	}

	buf3 = (int*)buf;
	if (xm==2 && ym==2) {
		byte *bc2, *bc3;
		int	ic, iiy;

		bc2 = (byte *)buf2;
		bc3 = (byte *)buf3;
		for (iy = 0; iy<256; iy++) {
			iiy = iy<<12;
			for (ix = 0; ix<2048; ix+=8) {
				for(ic = ix;ic<(ix+4);ic++) {
					*bc2=(bc3[iiy+ic]+bc3[iiy+4+ic]+bc3[iiy+2048+ic]+bc3[iiy+2048+4+ic])>>2;
					bc2++;
				}
			}
		}
	} else if (xm==2 && ym==1) {
		byte *bc2, *bc3;
		int	ic, iiy;

		bc2 = (byte *)buf2;
		bc3 = (byte *)buf3;
		for (iy = 0; iy<256; iy++) {
			iiy = iy<<11;
			for (ix = 0; ix<2048; ix+=8) {
				for(ic = ix;ic<(ix+4);ic++) {
					*bc2=(bc3[iiy+ic]+bc3[iiy+4+ic])>>1;
					bc2++;
				}
			}
		}
	} else {
		for (iy = 0; iy<256; iy++) {
			for (ix = 0; ix<256; ix++) {
					buf2[(iy<<8)+ix] = buf3[((iy*ym)<<ll) + (ix*xm)];
			}
		}
	}
}

/*
==================
CIN_DrawCinematic

==================
*/
void CIN_DrawCinematic (int handle) {
	float	x, y, w, h;
	byte	*buf;

	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return;

	if (!cinTable[handle].buf) {
		return;
	}

	x = cinTable[handle].xpos;
	y = cinTable[handle].ypos;
	w = cinTable[handle].width;
	h = cinTable[handle].height;
	buf = cinTable[handle].buf;

	
	if (!cinTable[handle].dirty && cinTable[handle].holdAtEnd)
	{
		// always render "holdAtEnd" videos, even if it has no new frame
		cinTable[handle].dirty = qtrue;
	}

	// Is video frame resampled to max texture size supported by videocard?
	if (cinTable[handle].dirty && (cinTable[handle].CIN_WIDTH != cinTable[handle].drawX || cinTable[handle].CIN_HEIGHT != cinTable[handle].drawY)) {
		if (cinTable[handle].drawX == 256 && cinTable[handle].drawY == 256)
		{
			// legacy stub for pre-voodoo2 videocards
			int* buf2 = (int*)Z_Malloc(cinTable[handle].drawX * cinTable[handle].drawY * 4, TAG_TEMP_WORKSPACE, qfalse);

			CIN_ResampleCinematic(handle, buf2);

			re.DrawStretchRaw(x, y, w, h, cinTable[handle].drawX, cinTable[handle].drawY, (byte*)buf2, handle, qtrue);
			cinTable[handle].dirty = qfalse;
			Z_Free(buf2);
		}
		else // we have non-square video output (for example, 1080p)
		{
			// Use hardware acceleration?
			if (videoDecoders[cinTable[handle].videoFormat].DataFormatYUV() && !cinTable[handle].shader) {
				re.DrawStretchVideoFrame(x, y, w, h, cinTable[handle].CIN_WIDTH, cinTable[handle].CIN_HEIGHT,
					cinTable[handle].bufY, cinTable[handle].bufU, cinTable[handle].bufV,
					cinTable[handle].bufY_stride, cinTable[handle].bufUV_stride,
					0, cinTable[handle].dirty);
			}
			else {
				int newWidth = (w * cinTable[handle].drawX / cinTable[handle].CIN_WIDTH);
				int newHeight = (h * cinTable[handle].drawX / cinTable[handle].CIN_HEIGHT);
				re.DrawStretchRaw(x, y, newWidth, newHeight, cinTable[handle].drawX, cinTable[handle].drawY, buf, handle, cinTable[handle].dirty);
			}
			cinTable[handle].dirty = qfalse;
		}
		return;
	}

	// Used to fix aspect ratio by fitting video to screen height
	if (nScreenRatioFixOffset > 0)
	{
		// Fill left and right bars from 4x3 video on 16x9 display with black
		SCR_FillRect(0, 0, nScreenRatioFixOffset, SCREEN_HEIGHT, g_color_table[0] /* black color */);
		SCR_FillRect(SCREEN_WIDTH - nScreenRatioFixOffset, 0, nScreenRatioFixOffset, SCREEN_HEIGHT, g_color_table[0] /* black color */);
	}

	if (cinTable[handle].dirty)
	{
		if (videoDecoders[cinTable[handle].videoFormat].DataFormatYUV() && !cinTable[handle].shader) {
			// video decoder uses YUV420, and renderer supports it
			re.DrawStretchVideoFrame(x, y, w, h, cinTable[handle].CIN_WIDTH, cinTable[handle].CIN_HEIGHT,
				cinTable[handle].bufY, cinTable[handle].bufU, cinTable[handle].bufV, cinTable[handle].bufY_stride, cinTable[handle].bufUV_stride,
				0, cinTable[handle].dirty);
		}
		else // RGB32
		{
			re.DrawStretchRaw(x, y, w, h, cinTable[handle].drawX, cinTable[handle].drawY, buf, handle, cinTable[handle].dirty);
		}
		cinTable[handle].dirty = qfalse;
	}
}

// external vars so I can check if the game is setup enough that I can play the intro video...
//
extern qboolean	com_fullyInitialized;
extern qboolean s_soundStarted, s_soundMuted;
//
// ... and if the app isn't ready yet (which should only apply for the intro video), then I use these...
//
static char	 sPendingCinematic_Arg	[256]={0};
static char	 sPendingCinematic_s	[256]={0};
static qboolean gbPendingCinematic = qfalse;
//
// This stuff is for EF1-type ingame cinematics...
//
static qboolean qbPlayingInGameCinematic = qfalse;
static qboolean qbInGameCinematicOnStandBy = qfalse;
static char	 sInGameCinematicStandingBy[MAX_QPATH];
static char	 sTextCrawlFixedCinematic[MAX_QPATH];
static qboolean qbTextCrawlFixed = qfalse;
static int	 stopCinematicCallCount = 0;



static qboolean CIN_HardwareReadyToPlayVideos(void)
{
	if (com_fullyInitialized && cls.rendererStarted &&
								cls.soundStarted	&&
								cls.soundRegistered
		)
	{
		return qtrue;
	}

	return qfalse;
}


static void PlayCinematic(const char *arg, const char *s, qboolean qbInGame)
{
	qboolean bFailed = qfalse;

	Cvar_Set( "timescale", "1" );			// jic we were skipping a scripted cinematic, return to normal after playing video
	Cvar_Set( "skippingCinematic", "0" );	// ""

	if(qbInGameCinematicOnStandBy == qfalse)
	{
		qbTextCrawlFixed = qfalse;
	}
	else
	{
		qbInGameCinematicOnStandBy = qfalse;
	}

	int bits = qbInGame ? 0 : CIN_system;

	Com_DPrintf("CL_PlayCinematic_f\n");

	// Get file name from coded name (for example, [jk10] or [video/jk10] --> [video/jk10.roq)
	char sTemp[1024], sShortFileName[1024];
	cinVideoFormat Format = ProcessVideoFileName(sTemp, 1024, arg, qfalse);
	arg = &sTemp[0];

	// If file exists
	if (Format != cinVideoFormat::MAX)
	{
		Q_strncpyz(sShortFileName, sTemp, 1024);
		sShortFileName[strlen(sShortFileName) - 4] = 0; // file name without extension
			
		SCR_StopCinematic();
		// command-line hack to avoid problems when playing intro video before app is fully setup...
		//
		if (!CIN_HardwareReadyToPlayVideos())
		{
			Q_strncpyz(sPendingCinematic_Arg,arg, 256);
			Q_strncpyz(sPendingCinematic_s , (s&&s[0])?s:"", 256);
			gbPendingCinematic = qtrue;
			return;
		}

		qbPlayingInGameCinematic = qbInGame;

		if ((s && s[0] == '1') || (Q_stricmp(sShortFileName, "video/end")==0)) {
			bits |= CIN_hold;
		}
		if (s && s[0] == '2') {
			bits |= CIN_loop;
		}

		S_StopAllSounds();


		////////////////////////////////////////////////////////////////////
		//
		// work out associated audio-overlay file, if any...
		//
		extern cvar_t *s_language;
		qboolean	bIsForeign	= (qboolean)(s_language && Q_stricmp(s_language->string,"english") && Q_stricmp(s_language->string,""));
		const char *psAudioFile	= NULL;
		qhandle_t	hCrawl = 0;

		const bool bStarWarsText = !Q_stricmp(sShortFileName, "video/jk0101_sw");
		if (bStarWarsText)
		{
			psAudioFile = "music/cinematic_1";
#ifdef JK2_MODE
			hCrawl = re.RegisterShaderNoMip( va("menu/video/tc_%d", sp_language->integer) );
			if(!hCrawl)
			{
				// failed, so go back to english
				hCrawl = re.RegisterShaderNoMip( "menu/video/tc_0" );
			}
#else
			hCrawl = re.RegisterShaderNoMip( va("menu/video/tc_%s",se_language->string) );
			if (!hCrawl)
			{
				hCrawl = re.RegisterShaderNoMip( "menu/video/tc_english" );//failed, so go back to english
			}
#endif
			bits |= CIN_hold;
		}
		else
		{
			if (bIsForeign)
			{
				if (!Q_stricmp(sShortFileName, "video/jk05"))
				{
					psAudioFile = "sound/chars/video/cinematic_5";
					bits |= CIN_silent;	// knock out existing english track
				}
				else if (!Q_stricmp(sShortFileName, "video/jk06"))
				{
					psAudioFile = "sound/chars/video/cinematic_6";
					bits |= CIN_silent;	// knock out existing english track
				}
			}
		}
		//
		////////////////////////////////////////////////////////////////////
		
		////////////////////////////////////////////////////////////////////
		// 
		// Fix display ratio

		// While video header isn't loaded yet, we make an assumption
		// that OGV video has format 16:9 (while original ROQ videos are 4:3)
#ifdef DECODER_OGV
		const float VideoRatio = (Format == cinVideoFormat::VIDEO_OGV)
			? 1080.f / 1920.f
			: (float)SCREEN_HEIGHT / (float)SCREEN_WIDTH;
#else
		const float VideoRatio = (float)SCREEN_HEIGHT / (float)SCREEN_WIDTH;
#endif

		float scrWidthOffs = ((float)cls.glconfig.vidWidth /* screen width */ - (float)cls.glconfig.vidHeight / VideoRatio /* desired width */) * 0.5f;
		nScreenRatioFixOffset = (int)(scrWidthOffs * (float)SCREEN_WIDTH / (float)cls.glconfig.vidWidth);

#if ASPECT_RATIO_FIX
		if (ASPECT_RATIO_STRETCH_TO_HEIGHT /* stretch video to screen height */)
		{
			bool bHardCodedStretchedVideo = bStarWarsText || !Q_stricmp(arg, "video/ja01.roq");

			if (bHardCodedStretchedVideo || nScreenRatioFixOffset < 5 || nScreenRatioFixOffset > SCREEN_WIDTH / 2)
			{
				nScreenRatioFixOffset = 0;
				CL_handle = CIN_PlayCinematic(arg, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, bits, psAudioFile);
			}
			else
			{
				CL_handle = CIN_PlayCinematic(arg, nScreenRatioFixOffset, 0, SCREEN_WIDTH - nScreenRatioFixOffset * 2, SCREEN_HEIGHT, bits, psAudioFile);
			}
		}
		else /* stretch video to screen width */
		{
			// From JHAEnhanced
			float new_height = SCREEN_HEIGHT;
			float offset = 0;
			if (nScreenRatioFixOffset > 5)
			{
				float ratio = (float)(SCREEN_WIDTH * cls.glconfig.vidHeight) / (float)(SCREEN_HEIGHT * cls.glconfig.vidWidth);
				ratio = Com_Clamp(0.75f, 1.0f, ratio);
				new_height = SCREEN_HEIGHT / ratio;
				offset = (SCREEN_HEIGHT - (SCREEN_HEIGHT / ratio)) / 2.0f;
			}
			nScreenRatioFixOffset = 0;
			CL_handle = CIN_PlayCinematic(arg, 0, offset, SCREEN_WIDTH, new_height, bits, psAudioFile);
			// END From JHAEnhanced
		}
#else
		nScreenRatioFixOffset = 0;
		CL_handle = CIN_PlayCinematic(arg, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, bits, psAudioFile);
#endif

		if (CL_handle >= 0)
		{
			cinTable[CL_handle].hCRAWLTEXT = hCrawl;
			do
			{
				SCR_RunCinematic();
			}
			while (cinTable[currentHandle].buf == NULL && cinTable[currentHandle].status == FMV_PLAY);		// wait for first frame (load codebook and sound)

			if (qbInGame)
			{
				Cvar_SetValue( "cl_paused", 1);	// remove-menu call will have unpaused us, so we sometimes need to re-pause
			}

			CL_iPlaybackStartTime = cls.realtime;	// special use to avoid accidentally skipping ingame videos via fast-firing
		}
		else
		{
			// failed to open video...
			//
			bFailed = qtrue;
		}
	}
	else
	{
		// failed to open video...
		//
		bFailed = qtrue;
	}

	if (bFailed)
	{
		Com_Printf(S_COLOR_RED "PlayCinematic(): Failed to open \"%s\"\n",arg);
		//S_RestartMusic();	//restart the level music
		SCR_StopCinematic();	// I know this seems pointless, but it clears a bunch of vars as well
	}
	else
	{
		// this doesn't work for now...
		//
//		if (cls.state == CA_ACTIVE){
//			re.InitDissolve(qfalse);	// so we get a dissolve between previous screen image and cinematic
//		}
	}
}


qboolean CL_CheckPendingCinematic(void)
{
	if ( gbPendingCinematic && CIN_HardwareReadyToPlayVideos() )
	{
		gbPendingCinematic = qfalse;	// BEFORE next line, or we get recursion
		PlayCinematic(sPendingCinematic_Arg,sPendingCinematic_s[0]?sPendingCinematic_s:NULL,qfalse);
		return qtrue;
	}
	return qfalse;
}

/*
==================
CL_CompleteCinematic
==================
*/
void CL_CompleteCinematic( char *args, int argNum ) {
	if ( argNum == 2 )
		Field_CompleteFilename( "video", "roq", qtrue, qfalse );
}

void CL_PlayCinematic_f(void)
{
	const char	*arg, *s;

	arg = Cmd_Argv( 1 );
	s = Cmd_Argv(2);
	PlayCinematic(arg,s,qfalse);
}

void CL_PlayInGameCinematic_f(void)
{
	const char *arg = Cmd_Argv( 1 );
	if (cls.state == CA_ACTIVE)
	{
		PlayCinematic(arg,NULL,qtrue);
	}
	else if( !qbInGameCinematicOnStandBy )
	{
		Q_strncpyz(sInGameCinematicStandingBy, arg, MAX_QPATH);
		qbInGameCinematicOnStandBy = qtrue;
	}
	else
	{
		// hack in order to fix text crawl --eez
		Q_strncpyz(sTextCrawlFixedCinematic, arg, MAX_QPATH);
		qbTextCrawlFixed = qtrue;
	}
}


// Externally-called only, and only if cls.state == CA_CINEMATIC (or CL_IsRunningInGameCinematic() == true now)
//
void SCR_DrawCinematic (void)
{
	if (CL_InGameCinematicOnStandBy())
	{
		PlayCinematic(sInGameCinematicStandingBy,NULL,qtrue);
	}
	else if( qbTextCrawlFixed && stopCinematicCallCount > 1)
	{
		PlayCinematic(sTextCrawlFixedCinematic, NULL, qtrue);
	}

	if (CL_handle >= 0 && CL_handle < MAX_VIDEO_HANDLES) {
		CIN_DrawCinematic(CL_handle);
		if (cinTable[CL_handle].hCRAWLTEXT && (cls.realtime - CL_iPlaybackStartTime >= TC_DELAY))
		{
			CIN_AddTextCrawl();
		}
	}
}

void SCR_RunCinematic (void)
{
	CL_CheckPendingCinematic();

	if (CL_handle >= 0 && CL_handle < MAX_VIDEO_HANDLES) {
		e_status Status = CIN_RunCinematic(CL_handle);

		if (CL_IsRunningInGameCinematic() && Status == FMV_IDLE  && !cinTable[CL_handle].holdAtEnd)
		{
			SCR_StopCinematic();	// change ROQ from FMV_IDLE to FMV_EOF, and clear some other vars
		}
	}
}

void SCR_StopCinematic( qboolean bAllowRefusal /* = qfalse */ )
{
	if (bAllowRefusal)
	{
		if ( (CL_handle >= 0 && CL_handle < MAX_VIDEO_HANDLES)
			&&
			cls.realtime < CL_iPlaybackStartTime + 1200	// 1.2 seconds have to have elapsed
			)
		{
			return;
		}
	}

	if ( CL_IsRunningInGameCinematic())
	{
		Com_DPrintf("In-game Cinematic Stopped\n");
	}

	if (CL_handle >= 0 && CL_handle < MAX_VIDEO_HANDLES &&
		stopCinematicCallCount != 1) {			// hello no, don't want this plz
		CIN_StopCinematic(CL_handle);
		S_StopAllSounds();
		CL_handle = -1;
		if (CL_IsRunningInGameCinematic()){
			re.InitDissolve(qfalse);	// dissolve from cinematic to underlying ingame
		}
	}

	if (cls.state == CA_CINEMATIC)
	{
		Com_DPrintf("Cinematic Stopped\n");
		cls.state =  CA_DISCONNECTED;
	}

	if(sInGameCinematicStandingBy[0] &&
		qbTextCrawlFixed)
	{
		// Hacky fix to help deal with broken text crawl..
		// If we are skipping past the one on standby, DO NOT SKIP THE OTHER ONES!
		stopCinematicCallCount++;
	}
	else if(stopCinematicCallCount == 1)
	{
		stopCinematicCallCount++;
	}
	else
	{
		// Skipping the last one in the list, go ahead and kill it.
		qbTextCrawlFixed = qfalse;
		sTextCrawlFixedCinematic[0] = 0;
		stopCinematicCallCount = 0;
	}

	if(stopCinematicCallCount != 2)
	{
		qbPlayingInGameCinematic = qfalse;
		qbInGameCinematicOnStandBy = qfalse;
		sInGameCinematicStandingBy[0]=0;
		Cvar_SetValue( "cl_paused", 0 );
	}
	if (cls.state != CA_DISCONNECTED)	// cut down on needless calls to music code
	{
		S_RestartMusic();	//restart the level music
	}
}


void CIN_UploadCinematic(int handle) {
	if (handle >= 0 && handle < MAX_VIDEO_HANDLES) {
		if (!cinTable[handle].buf) {
			return;
		}
		if (cinTable[handle].playonwalls <= 0 && cinTable[handle].dirty) {
			if (cinTable[handle].playonwalls == 0) {
				cinTable[handle].playonwalls = -1;
			} else {
				if (cinTable[handle].playonwalls == -1) {
					cinTable[handle].playonwalls = -2;
				} else {
					cinTable[handle].dirty = qfalse;
				}
			}
		}

		// Resample video if needed
		if (cinTable[handle].dirty && (cinTable[handle].CIN_WIDTH != cinTable[handle].drawX || cinTable[handle].CIN_HEIGHT != cinTable[handle].drawY)) {
			if (cinTable[handle].drawX == 256 && cinTable[handle].drawY == 256)
			{
				int* buf2;
				buf2 = (int*)Z_Malloc(256 * 256 * 4, TAG_TEMP_WORKSPACE, qfalse);

				CIN_ResampleCinematic(handle, buf2);

				re.UploadCinematic(256, 256, (byte*)buf2, handle, qtrue);
				cinTable[handle].dirty = qfalse;
				Z_Free(buf2);
			}
			else
			{
				// Upload video at normal resolution
				re.UploadCinematic(cinTable[handle].CIN_WIDTH, cinTable[handle].CIN_HEIGHT,
					cinTable[handle].buf, handle, cinTable[handle].dirty);
				cinTable[handle].dirty = qfalse;
			}
		} else {
			// Upload video at normal resolution
			re.UploadCinematic( cinTable[handle].drawX, cinTable[handle].drawY,
				cinTable[handle].buf, handle, cinTable[handle].dirty);
			cinTable[handle].dirty = qfalse;
		}

		if (cl_inGameVideo->integer == 0 && cinTable[handle].playonwalls == 1) {
			cinTable[handle].playonwalls--;
		}
		else if (cl_inGameVideo->integer != 0 && cinTable[handle].playonwalls != 1) {
			cinTable[handle].playonwalls = 1;
		}
	}
}


qboolean CL_IsRunningInGameCinematic(void)
{
	return qbPlayingInGameCinematic;
}

qboolean CL_InGameCinematicOnStandBy(void)
{
	return qbInGameCinematicOnStandBy;
}

cinVideoFormat ProcessVideoFileName(char* outFileName, int outSize, const char* inFileName, qboolean bShader)
{
	cinVideoFormat Format = cinVideoFormat::VIDEO_ROQ;
	/*
	* for file name video/mycinematic or video/mycinematic.roq:
	* if OGV isn't supported, use video/mycinematic.ROQ
	* if OGV is supported:
	*	a. GPU acceleration is supported (i. e. using rend2):
	*		Try to find and load video/mycinematic.OGV,
	*		if it isn't exist fall back to video/mycinematic.ROQ
	*	b. GPU acceleration isn't supported (vanilla renderer):
	*		At first try to find video/mycinematic_sd.OGV. If it
	*		doesn't exist, use video/mycinematic.OGV. If it also
	*		doesn't exist, use video/mycinematic.ROQ.
	*/

	if (strstr(inFileName, "/") == NULL && strstr(inFileName, "\\") == NULL) {
		Com_sprintf(outFileName, outSize, "video/%s", inFileName);
	}
	else {
		Com_sprintf(outFileName, outSize, "%s", inFileName);
	}

#ifdef DECODER_OGV
	// Get file format
	const char* extension = COM_GetExtension(outFileName);
	// Remove existing extension, need to try ogv first
	if (strlen(extension) > 0) {
		outFileName[strlen(outFileName) - 4] = 0;
	}

	// if possible I want to play SD video instead of HD, if GPU acceleration isn't available
	char name_ogv[MAX_OSPATH];
	Q_strncpyz(name_ogv, outFileName, MAX_OSPATH);

	if (bShader || !videoDecoders[cinVideoFormat::VIDEO_OGV].DataFormatYUV()) {
		Q_strcat(name_ogv, MAX_OSPATH, "_sd.ogv");
		if (FS_FileIsInPAK(name_ogv) <= 0) {
			Q_strncpyz(name_ogv, outFileName, MAX_OSPATH);
			COM_DefaultExtension(name_ogv, sizeof(name_ogv), ".ogv");
		}
	}
	else {
		COM_DefaultExtension(name_ogv, sizeof(name_ogv), ".ogv");
	}

	if (FS_FileIsInPAK(name_ogv) > 0) {
		Q_strncpyz(outFileName, name_ogv, MAX_OSPATH);
		Format = cinVideoFormat::VIDEO_OGV;
	}
	else
	{
		COM_DefaultExtension(outFileName, outSize, ".roq");
	}
#else
	COM_DefaultExtension(outFileName, outSize, ".roq");
#endif
	
	if (FS_FileIsInPAK(outFileName) < 0)
	{
		Format = cinVideoFormat::MAX;
	}

	return Format;
}

/******************************************************************************
*
* Function:
*
* Description:
*
******************************************************************************/

static void CIN_StopVideo(cinematics_t* cin, cin_cache* table)
{
	const char* s;

	if (!table->buf) {
		if (table->iFile) {
			//			assert( 0 && "ROQ handle leak-prevention WAS needed!");
			FS_FCloseFile(table->iFile);
			table->iFile = 0;
			if (table->hSFX) {
				S_CIN_StopSound(table->hSFX);
			}
		}
		return;
	}

	if (table->status == FMV_IDLE) {
		return;
	}

	Com_DPrintf("finished cinematic\n");
	table->status = FMV_IDLE;
	table->buf = NULL;

	int handle = 0;
	for (; handle < MAX_VIDEO_HANDLES; handle++)
	{
		if (!Q_stricmp(cinTable[handle].fileName, table->fileName))
		{
			videoDecoders[table->videoFormat].Stop(handle);
			break;
		}
	}
	if (handle == MAX_VIDEO_HANDLES)
	{
		videoDecoders[table->videoFormat].Stop(currentHandle);
	}

	// Free image buffer
	if (cin->linbuf)
	{
		Z_Free(cin->linbuf);
		cin->linbuf = NULL;
		cin->linbufCapacity = 0;
	}
	// Free roq status buffers
	if (cin->qStatus[0])
	{
		Z_Free(cin->qStatus[0]);
		cin->qStatus[0] = NULL;
	}
	if (cin->qStatus[1])
	{
		Z_Free(cin->qStatus[1]);
		cin->qStatus[1] = NULL;
	}
	cin->qStatusCapacity = 0;
	// Free audio buffer
	if (table->audioBuffer)
	{
		Z_Free(table->audioBuffer);
		table->audioBuffer = NULL;
		table->audioBufferCapacity = 0;
	}

	if (table->iFile) {
		FS_FCloseFile(table->iFile);
		table->iFile = 0;
		if (table->hSFX) {
			S_CIN_StopSound(table->hSFX);
		}
	}

	if (table->alterGameState) {
		cls.state = CA_DISCONNECTED;
		// we can't just do a vstr nextmap, because
		// if we are aborting the intro cinematic with
		// a devmap command, nextmap would be valid by
		// the time it was referenced
		s = Cvar_VariableString("nextmap");
		if (s[0]) {
			Cbuf_ExecuteText(EXEC_APPEND, va("%s\n", s));
			Cvar_Set("nextmap", "");
		}
		CL_handle = -1;
	}
	table->fileName[0] = 0;
	currentHandle = -1;
}

#undef _clamp