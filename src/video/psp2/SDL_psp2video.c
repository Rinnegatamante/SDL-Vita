/*
	SDL - Simple DirectMedia Layer
	Copyright (C) 1997-2012 Sam Lantinga

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

	Sam Lantinga
	slouken@libsdl.org
*/
#include "SDL_config.h"

/* PSP2 SDL video driver implementation; this is just enough to make an
 *  SDL-based application THINK it's got a working video driver, for
 *  applications that call SDL_Init(SDL_INIT_VIDEO) when they don't need it,
 *  and also for use as a collection of stubs when porting SDL to a new
 *  platform for which you haven't yet written a valid video driver.
 *
 * This is also a great way to determine bottlenecks: if you think that SDL
 *  is a performance problem for a given platform, enable this driver, and
 *  then see if your application runs faster without video overhead.
 *
 * Initial work by Ryan C. Gordon (icculus@icculus.org). A good portion
 *  of this was cut-and-pasted from Stephane Peter's work in the AAlib
 *  SDL video driver.  Renamed to "DUMMY" by Sam Lantinga.
 */

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_psp2video.h"
#include "SDL_psp2events_c.h"
#include "SDL_psp2mouse_c.h"
#include "SDL_psp2keyboard_c.h"

#include "shaders/lcd3x_f.h"
#include "shaders/lcd3x_v.h"
#include "shaders/sharp_bilinear_f.h"
#include "shaders/sharp_bilinear_v.h"
#include "shaders/sharp_bilinear_simple_f.h"
#include "shaders/sharp_bilinear_simple_v.h"
#include "shaders/xbr_2x_fast_f.h"
#include "shaders/xbr_2x_fast_v.h"

#define PSP2VID_DRIVER_NAME "psp2"

#define SCREEN_W 960
#define SCREEN_H 544

typedef struct private_hwdata {
	GLuint texture;
	SDL_Rect dst;
} private_hwdata;

static int vsync = 1;
static unsigned int cur_fb = 0;

/* Initialization/Query functions */
static int PSP2_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **PSP2_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *PSP2_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int PSP2_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void PSP2_VideoQuit(_THIS);

/* Hardware surface functions */
static int PSP2_FlipHWSurface(_THIS, SDL_Surface *surface);
static int PSP2_AllocHWSurface(_THIS, SDL_Surface *surface);
static int PSP2_LockHWSurface(_THIS, SDL_Surface *surface);
static void PSP2_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void PSP2_FreeHWSurface(_THIS, SDL_Surface *surface);

/* etc. */
static void PSP2_UpdateRects(_THIS, int numrects, SDL_Rect *rects);

void (*callback)();

/* PSP2 driver bootstrap functions */
static int PSP2_Available(void)
{
	return 1;
}

static void PSP2_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *PSP2_CreateDevice(int devindex)
{
	SDL_VideoDevice *device;

	/* Initialize all variables that we clean on shutdown */
	device = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( device ) {
		SDL_memset(device, 0, (sizeof *device));
		device->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *device->hidden));
	}
	if ( (device == NULL) || (device->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( device ) {
			SDL_free(device);
		}
		return(0);
	}
	SDL_memset(device->hidden, 0, (sizeof *device->hidden));

	/* Set the function pointers */
	device->VideoInit = PSP2_VideoInit;
	device->ListModes = PSP2_ListModes;
	device->SetVideoMode = PSP2_SetVideoMode;
	device->CreateYUVOverlay = NULL;
	device->SetColors = PSP2_SetColors;
	device->UpdateRects = PSP2_UpdateRects;
	device->VideoQuit = PSP2_VideoQuit;
	device->AllocHWSurface = PSP2_AllocHWSurface;
	device->CheckHWBlit = NULL;
	device->FillHWRect = NULL;
	device->SetHWColorKey = NULL;
	device->SetHWAlpha = NULL;
	device->LockHWSurface = PSP2_LockHWSurface;
	device->UnlockHWSurface = PSP2_UnlockHWSurface;
	device->FlipHWSurface = PSP2_FlipHWSurface;
	device->FreeHWSurface = PSP2_FreeHWSurface;
	device->SetCaption = NULL;
	device->SetIcon = NULL;
	device->IconifyWindow = NULL;
	device->GrabInput = NULL;
	device->GetWMInfo = NULL;
	device->InitOSKeymap = PSP2_InitOSKeymap;
	device->PumpEvents = PSP2_PumpEvents;

	device->free = PSP2_DeleteDevice;

	return device;
}

float *texcoords = NULL;
uint16_t *indices = NULL;
GLint fs[4], vs[4], shaders[4];

typedef struct shader_uniforms{
	GLint video_size[2];
	GLint texture_size[2];
	GLint output_size[2];
} shader_uniforms;

shader_uniforms unif[4];
SDL_Shader cur_shader = SDL_SHADER_NONE;
float texture_size[2], output_size[2];

int PSP2_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_4X);
	vglWaitVblankStart(vsync);
	
	glEnable(GL_TEXTURE_2D);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0.0f, SCREEN_W, SCREEN_H, 0.0f, 0.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	vformat->BitsPerPixel = 32;
	vformat->BytesPerPixel = 4;
	vformat->Rmask = 0x000000FF;
	vformat->Gmask = 0x0000FF00;
	vformat->Bmask = 0x00FF0000;
	vformat->Amask = 0xFF000000;

	PSP2_InitKeyboard();
	PSP2_InitMouse();
	
	texcoords = malloc(sizeof(float)*8);
	texcoords[0] = 0.0f;
	texcoords[1] = 0.0f;
	texcoords[2] = 1.0f;
	texcoords[3] = 0.0f;
	texcoords[4] = 1.0f;
	texcoords[5] = 1.0f;
	texcoords[6] = 0.0f;
	texcoords[7] = 1.0f;

	indices = malloc(sizeof(uint16_t)*4);
	indices[0] = 0;
	indices[1] = 1;
	indices[2] = 2;
	indices[3] = 3;
	
    callback = NULL;
	
	int i;
	for (i = 0; i < 4; i++){
		fs[i] = glCreateShader(GL_FRAGMENT_SHADER);
		vs[i] = glCreateShader(GL_VERTEX_SHADER);
		shaders[i] = glCreateProgram();
	}
    
	glShaderBinary(1, &fs[SDL_SHADER_LCD3X], 0, lcd3x_f, size_lcd3x_f);
	glShaderBinary(1, &fs[SDL_SHADER_SHARP_BILINEAR], 0, sharp_bilinear_f, size_sharp_bilinear_f);
	glShaderBinary(1, &fs[SDL_SHADER_SHARP_BILINEAR_SIMPLE], 0, sharp_bilinear_simple_f, size_sharp_bilinear_simple_f);
	glShaderBinary(1, &fs[SDL_SHADER_XBR_2X_FAST], 0, xbr_2x_fast_f, size_xbr_2x_fast_f);
	glShaderBinary(1, &vs[SDL_SHADER_LCD3X], 0, lcd3x_v, size_lcd3x_v);
	glShaderBinary(1, &vs[SDL_SHADER_SHARP_BILINEAR], 0, sharp_bilinear_v, size_sharp_bilinear_v);
	glShaderBinary(1, &vs[SDL_SHADER_SHARP_BILINEAR_SIMPLE], 0, sharp_bilinear_simple_v, size_sharp_bilinear_simple_v);
	glShaderBinary(1, &vs[SDL_SHADER_XBR_2X_FAST], 0, xbr_2x_fast_v, size_xbr_2x_fast_v);
	
	for (i = 0; i < 4; i++) {
		glAttachShader(shaders[i], fs[i]);
		glAttachShader(shaders[i], vs[i]);
		vglBindAttribLocation(shaders[i], 0, "aPosition", 4, GL_FLOAT);
		vglBindAttribLocation(shaders[i], 1, "aTexcoord", 2, GL_FLOAT);
		unif[i].video_size[0] = glGetUniformLocation(shaders[i], "IN.video_size");
		unif[i].texture_size[0] = glGetUniformLocation(shaders[i], "IN.texture_size");
		unif[i].output_size[0] = glGetUniformLocation(shaders[i], "IN.output_size");
		unif[i].video_size[1] = glGetUniformLocation(shaders[i], "IN2.video_size");
		unif[i].texture_size[1] = glGetUniformLocation(shaders[i], "IN2.texture_size");
		unif[i].output_size[1] = glGetUniformLocation(shaders[i], "IN2.output_size");
		glLinkProgram(shaders[i]);
	}
	
	return(0);
}

SDL_Surface *PSP2_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
	switch(bpp)
	{
		case 16:
			if (!SDL_ReallocFormat(current, 16, 0xF800, 0x07C0, 0x003E, 0x0001))
			{
				SDL_SetError("Couldn't allocate new pixel format for requested mode");
				return(NULL);
			}
		break;

		case 24:
			if (!SDL_ReallocFormat(current, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0x00000000))
			{
				SDL_SetError("Couldn't allocate new pixel format for requested mode");
				return(NULL);
			}
		break;

		case 32:
			if (!SDL_ReallocFormat(current, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000))
			{
				SDL_SetError("Couldn't allocate new pixel format for requested mode");
				return(NULL);
			}
		break;

		default:
			if (!SDL_ReallocFormat(current, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000))
			{
				SDL_SetError("Couldn't allocate new pixel format for requested mode");
				return(NULL);
			}
		break;
	}
	
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0f, SCREEN_W, SCREEN_H, 0.0f, 0.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	
	current->flags = flags | SDL_FULLSCREEN | SDL_DOUBLEBUF;
	current->w = width;
	current->h = height;
	texture_size[0] = width;
	texture_size[1] = height;
	output_size[0] = SCREEN_W;
	output_size[1] = SCREEN_H;
	if(current->hwdata == NULL)
	{
		PSP2_AllocHWSurface(this, current);
	}

	return(current);
}

SDL_Rect **PSP2_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	static SDL_Rect PSP2_Rects[] = {
		{0, 0, 960, 544},
	};
	static SDL_Rect *PSP2_modes[] = {
		&PSP2_Rects[0],
		NULL
	};
	SDL_Rect **modes = PSP2_modes;

	// support only 16/24 bits for now
	switch(format->BitsPerPixel)
	{
		case 16:
		case 24:
		case 32:
		return modes;

		default:
		return (SDL_Rect **) -1;
	}
}

static int PSP2_AllocHWSurface(_THIS, SDL_Surface *surface)
{
	surface->hwdata = (private_hwdata*) SDL_malloc (sizeof (private_hwdata));
	if (surface->hwdata == NULL)
	{
		SDL_OutOfMemory();
		return -1;
	}
	SDL_memset (surface->hwdata, 0, sizeof(private_hwdata));

	// set initial texture destination
	surface->hwdata->dst.x = 0;
	surface->hwdata->dst.y = 0;
	surface->hwdata->dst.w = SCREEN_W;
	surface->hwdata->dst.h = SCREEN_H;
	glGenTextures(1, &surface->hwdata->texture);
	glBindTexture(GL_TEXTURE_2D, surface->hwdata->texture);

	surface->pixels = malloc(surface->w * surface->h * surface->format->BytesPerPixel);
	
	switch(surface->format->BitsPerPixel)
	{
		case 16:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->w, surface->h, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, surface->pixels);
		break;

		case 24:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, surface->w, surface->h, 0, GL_RGB, GL_UNSIGNED_BYTE, surface->pixels);
		break;

		case 32:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->w, surface->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surface->pixels);
		break;

		default:
			SDL_SetError("unsupported BitsPerPixel: %i\n", surface->format->BitsPerPixel);
		return -1;
	}

	surface->pixels = vglGetTexDataPointer(GL_TEXTURE_2D);
	surface->pitch = surface->w * surface->format->BytesPerPixel;
	surface->flags |= SDL_HWSURFACE;

	return(0);
}

static void PSP2_FreeHWSurface(_THIS, SDL_Surface *surface)
{
	if (surface->hwdata != NULL)
	{
		glFinish();
		glDeleteTextures(1, &surface->hwdata->texture);
		free(surface->pixels);
		SDL_free(surface->hwdata);
		surface->hwdata = NULL;
		surface->pixels = NULL;
	}
}

static int PSP2_FlipHWSurface(_THIS, SDL_Surface *surface)
{
	glBindTexture(GL_TEXTURE_2D, surface->hwdata->texture);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	
	glBindFramebuffer(GL_FRAMEBUFFER, cur_fb);
	glClear(GL_COLOR_BUFFER_BIT);
	if (cur_shader != SDL_SHADER_NONE){
		float vertices[] = {
			surface->hwdata->dst.x, surface->hwdata->dst.y, 0, 1,
			surface->hwdata->dst.x + surface->hwdata->dst.w, surface->hwdata->dst.y, 0, 1,
			surface->hwdata->dst.x + surface->hwdata->dst.w, surface->hwdata->dst.y + surface->hwdata->dst.h, 0, 1,
			surface->hwdata->dst.x, surface->hwdata->dst.y + surface->hwdata->dst.h, 0, 1
		};
		
		glUseProgram(shaders[cur_shader]);
		vglVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 4, vertices);
		vglVertexAttribPointerMapped(1, texcoords);
		int i;
		for (i = 0; i < 2; i++){
			glUniform2fv(unif[cur_shader].output_size[i], 1, output_size);
			glUniform2fv(unif[cur_shader].video_size[i], 1, texture_size);
			glUniform2fv(unif[cur_shader].texture_size[i], 1, texture_size);
		}
	}else{
		float vertices[] = {
			surface->hwdata->dst.x, surface->hwdata->dst.y, 0,
			surface->hwdata->dst.x + surface->hwdata->dst.w, surface->hwdata->dst.y, 0,
			surface->hwdata->dst.x + surface->hwdata->dst.w, surface->hwdata->dst.y + surface->hwdata->dst.h, 0,
			surface->hwdata->dst.x, surface->hwdata->dst.y + surface->hwdata->dst.h, 0
		};
		
		vglVertexPointer(3, GL_FLOAT, 0, 4, vertices);
		vglTexCoordPointerMapped(texcoords);
	}
	vglIndexPointerMapped(indices);
	vglDrawObjects(GL_TRIANGLE_FAN, 4, GL_TRUE);
	glUseProgram(0);
    if (callback != NULL) callback();
	vglSwapBuffers(GL_FALSE);
}

// custom psp2 function for centering/scaling main screen surface (texture)
void SDL_SetVideoModeScaling(int x, int y, float w, float h)
{
	SDL_Surface *surface = SDL_VideoSurface;

	if (surface != NULL && surface->hwdata != NULL)
	{
		surface->hwdata->dst.x = x;
		surface->hwdata->dst.y = y;
		surface->hwdata->dst.w = w;
		surface->hwdata->dst.h = h;
		output_size[0] = w;
		output_size[1] = h;
	}
}

// custom psp2 function for setting the texture filter to nearest or bilinear
void SDL_SetVideoModeBilinear(int enable_bilinear)
{
	SDL_Surface *surface = SDL_GetVideoSurface();
	
	if (enable_bilinear) 
	{
		//reduce pixelation by setting bilinear filtering
		//for magnification
		//(first one is minimization filter,
		//second one is magnification filter)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
}	

// custom psp2 function for rendering callback
void SDL_SetVideoCallback(void (*cb)()){
    callback = cb;
}

// custom psp2 function for framebuffers usage
void SDL_SetVideoFrameBuffer(unsigned int fb){
    cur_fb = fb;
}

// custom psp2 function for vsync
void SDL_SetVideoModeSync(int enable_vsync)
{
	vsync = enable_vsync;
	vglWaitVblankStart(vsync);
}

// custom psp2 function for shaders
void SDL_SetVideoShader(SDL_Shader shader)
{
	cur_shader = shader;
}

static int PSP2_LockHWSurface(_THIS, SDL_Surface *surface)
{
	return(0);
}

static void PSP2_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
	return;
}

static void PSP2_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{

}

int PSP2_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	return(1);
}

void PSP2_VideoQuit(_THIS)
{
	if (this->screen->hwdata != NULL)
	{
		PSP2_FreeHWSurface(this, this->screen);
	}
	free(texcoords);
	free(indices);
	vglEnd();
}

VideoBootStrap PSP2_bootstrap = {
	PSP2VID_DRIVER_NAME, "SDL psp2 video driver",
	PSP2_Available, PSP2_CreateDevice
};

