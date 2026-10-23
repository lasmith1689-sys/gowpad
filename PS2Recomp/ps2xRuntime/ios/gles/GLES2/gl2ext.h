// See GLES3/gl3.h. rlgl's ES3 path still spells core ES 3.0 features with the
// ES2 extension names from Khronos' gl2ext.h; iOS ships no such header, so map
// them onto the ES3 core enums/entry points (and give the rest their Khronos values).
#pragma once
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>

#ifndef GL_COLOR_ATTACHMENT0_EXT
#define GL_COLOR_ATTACHMENT0_EXT GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT1_EXT GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT2_EXT GL_COLOR_ATTACHMENT2
#define GL_COLOR_ATTACHMENT3_EXT GL_COLOR_ATTACHMENT3
#define GL_COLOR_ATTACHMENT4_EXT 0x8CE4
#define GL_COLOR_ATTACHMENT5_EXT 0x8CE5
#define GL_COLOR_ATTACHMENT6_EXT 0x8CE6
#define GL_COLOR_ATTACHMENT7_EXT 0x8CE7
#endif
#ifndef glDrawBuffersEXT
#define glDrawBuffersEXT glDrawBuffers
#endif

#ifndef GL_DEPTH_COMPONENT24_OES
#define GL_DEPTH_COMPONENT24_OES 0x81A6
#endif
#ifndef GL_DEPTH_COMPONENT32_OES
#define GL_DEPTH_COMPONENT32_OES 0x81A7
#endif
#ifndef GL_RED_EXT
#define GL_RED_EXT 0x1903
#endif
#ifndef GL_R16F_EXT
#define GL_R16F_EXT 0x822D
#endif
#ifndef GL_R32F_EXT
#define GL_R32F_EXT 0x822E
#endif
#ifndef GL_RGB16F_EXT
#define GL_RGB16F_EXT 0x881B
#endif
#ifndef GL_RGB32F_EXT
#define GL_RGB32F_EXT 0x8815
#endif
#ifndef GL_RGBA16F_EXT
#define GL_RGBA16F_EXT 0x881A
#endif
#ifndef GL_RGBA32F_EXT
#define GL_RGBA32F_EXT 0x8814
#endif
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
#ifndef GL_MIRROR_CLAMP_EXT
#define GL_MIRROR_CLAMP_EXT 0x8742
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG 0x8C00
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG 0x8C01
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG 0x8C03
#endif
