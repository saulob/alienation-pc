// Stands in for <GLES3/gl3.h> in the one file that includes it: imgui's backend calls OpenGL ES by name, and these
// names go through pointers SDL resolves from ANGLE, so the executable links no OpenGL ES library.
#pragma once

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>

// Desktop features the ES3 build must not see: the backend tests these macros, not its target.
#undef GL_CLIP_ORIGIN
#undef GL_POLYGON_MODE

#define ALN_IMGUI_GLES3_FUNCTIONS(X)                                                               \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                       \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                           \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                                         \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                 \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                                                     \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                                     \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                             \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                           \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                                     \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                     \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                     \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                       \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                     \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                     \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                                                   \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                       \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                                           \
    X(PFNGLDETACHSHADERPROC, glDetachShader)                                                       \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)                               \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                 \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                           \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers)                                                         \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                 \
    X(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation)                                             \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                       \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                               \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                         \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                                           \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                           \
    X(PFNGLGETVERTEXATTRIBIVPROC, glGetVertexAttribiv)                                             \
    X(PFNGLGETVERTEXATTRIBPOINTERVPROC, glGetVertexAttribPointerv)                                 \
    X(PFNGLISPROGRAMPROC, glIsProgram)                                                             \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                         \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                                             \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                       \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                             \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                               \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                           \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define ALN_IMGUI_GLES3_CORE_FUNCTIONS(X)                                                          \
    X(glActiveTexture)                                                                             \
    X(glBindTexture)                                                                               \
    X(glDeleteTextures)                                                                            \
    X(glDisable)                                                                                   \
    X(glDrawElements)                                                                              \
    X(glEnable)                                                                                    \
    X(glGenTextures)                                                                               \
    X(glGetError)                                                                                  \
    X(glGetIntegerv)                                                                               \
    X(glGetString)                                                                                 \
    X(glIsEnabled)                                                                                 \
    X(glPixelStorei)                                                                               \
    X(glScissor)                                                                                   \
    X(glTexImage2D)                                                                                \
    X(glTexParameteri)                                                                             \
    X(glTexSubImage2D)                                                                             \
    X(glViewport)

#define ALN_DECLARE(type, name) static type aln_##name;
ALN_IMGUI_GLES3_FUNCTIONS(ALN_DECLARE)
#undef ALN_DECLARE
#define ALN_DECLARE(name) static decltype(&::name) aln_##name;
ALN_IMGUI_GLES3_CORE_FUNCTIONS(ALN_DECLARE)
#undef ALN_DECLARE

static bool LoadGles3ForImGui() {
#define ALN_RESOLVE(type, name)                                                                    \
    aln_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                             \
    if (!aln_##name) {                                                                             \
        return false;                                                                              \
    }
#define ALN_RESOLVE_CORE(name)                                                                     \
    aln_##name = reinterpret_cast<decltype(aln_##name)>(SDL_GL_GetProcAddress(#name));             \
    if (!aln_##name) {                                                                             \
        return false;                                                                              \
    }
    ALN_IMGUI_GLES3_FUNCTIONS(ALN_RESOLVE)
    ALN_IMGUI_GLES3_CORE_FUNCTIONS(ALN_RESOLVE_CORE)
#undef ALN_RESOLVE
#undef ALN_RESOLVE_CORE
    return true;
}

#define glActiveTexture aln_glActiveTexture
#define glAttachShader aln_glAttachShader
#define glBindBuffer aln_glBindBuffer
#define glBindSampler aln_glBindSampler
#define glBindTexture aln_glBindTexture
#define glBindVertexArray aln_glBindVertexArray
#define glBlendEquation aln_glBlendEquation
#define glBlendEquationSeparate aln_glBlendEquationSeparate
#define glBlendFuncSeparate aln_glBlendFuncSeparate
#define glBufferData aln_glBufferData
#define glBufferSubData aln_glBufferSubData
#define glCompileShader aln_glCompileShader
#define glCreateProgram aln_glCreateProgram
#define glCreateShader aln_glCreateShader
#define glDeleteBuffers aln_glDeleteBuffers
#define glDeleteProgram aln_glDeleteProgram
#define glDeleteSamplers aln_glDeleteSamplers
#define glDeleteShader aln_glDeleteShader
#define glDeleteTextures aln_glDeleteTextures
#define glDeleteVertexArrays aln_glDeleteVertexArrays
#define glDetachShader aln_glDetachShader
#define glDisable aln_glDisable
#define glDisableVertexAttribArray aln_glDisableVertexAttribArray
#define glDrawElements aln_glDrawElements
#define glEnable aln_glEnable
#define glEnableVertexAttribArray aln_glEnableVertexAttribArray
#define glGenBuffers aln_glGenBuffers
#define glGenSamplers aln_glGenSamplers
#define glGenTextures aln_glGenTextures
#define glGenVertexArrays aln_glGenVertexArrays
#define glGetAttribLocation aln_glGetAttribLocation
#define glGetError aln_glGetError
#define glGetIntegerv aln_glGetIntegerv
#define glGetProgramInfoLog aln_glGetProgramInfoLog
#define glGetProgramiv aln_glGetProgramiv
#define glGetShaderInfoLog aln_glGetShaderInfoLog
#define glGetShaderiv aln_glGetShaderiv
#define glGetString aln_glGetString
#define glGetStringi aln_glGetStringi
#define glGetUniformLocation aln_glGetUniformLocation
#define glGetVertexAttribPointerv aln_glGetVertexAttribPointerv
#define glGetVertexAttribiv aln_glGetVertexAttribiv
#define glIsEnabled aln_glIsEnabled
#define glIsProgram aln_glIsProgram
#define glLinkProgram aln_glLinkProgram
#define glPixelStorei aln_glPixelStorei
#define glSamplerParameteri aln_glSamplerParameteri
#define glScissor aln_glScissor
#define glShaderSource aln_glShaderSource
#define glTexImage2D aln_glTexImage2D
#define glTexParameteri aln_glTexParameteri
#define glTexSubImage2D aln_glTexSubImage2D
#define glUniform1i aln_glUniform1i
#define glUniformMatrix4fv aln_glUniformMatrix4fv
#define glUseProgram aln_glUseProgram
#define glVertexAttribPointer aln_glVertexAttribPointer
#define glViewport aln_glViewport
