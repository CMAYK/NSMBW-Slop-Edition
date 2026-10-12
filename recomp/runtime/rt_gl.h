/* rt_gl.h - the OpenGL 3.3 core subset used by the GL renderer, loaded at runtime
 * (no system GL headers needed; works with WGL on Windows and EGL on Linux). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat, GLclampf;
typedef double GLdouble;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
typedef void GLvoid;

enum {
    GL_FALSE = 0, GL_TRUE = 1,
    GL_DEPTH_BUFFER_BIT = 0x100, GL_COLOR_BUFFER_BIT = 0x4000,
    GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5, GL_TRIANGLE_FAN = 6, GL_LINES = 1, GL_LINE_STRIP = 3, GL_POINTS = 0,
    GL_NEVER = 0x200, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS,
    GL_ZERO = 0, GL_ONE = 1, GL_SRC_COLOR = 0x300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
    GL_CONSTANT_ALPHA = 0x8003, GL_FUNC_ADD = 0x8006, GL_FUNC_REVERSE_SUBTRACT = 0x800B,
    GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408, GL_CW = 0x900, GL_CCW = 0x901,
    GL_CULL_FACE = 0xB44, GL_DEPTH_TEST = 0xB71, GL_BLEND = 0xBE2, GL_SCISSOR_TEST = 0xC11, GL_DEPTH_CLAMP = 0x864F,
    GL_TEXTURE_2D = 0xDE1, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_UNSIGNED_INT = 0x1405,
    GL_RGBA = 0x1908, GL_BGRA = 0x80E1, GL_RGBA8 = 0x8058, GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367,
    GL_DEPTH_COMPONENT24 = 0x81A6, GL_DEPTH_COMPONENT = 0x1902,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_LINEAR_MIPMAP_LINEAR = 0x2703, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_TEXTURE_MAX_LEVEL = 0x813D, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F, GL_MIRRORED_REPEAT = 0x8370,
    GL_TEXTURE0 = 0x84C0, GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_STREAM_DRAW = 0x88E0,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84,
    GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_RENDERBUFFER = 0x8D41,
    GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_PACK_ALIGNMENT = 0xD05, GL_UNPACK_ALIGNMENT = 0xCF5, GL_UNPACK_ROW_LENGTH = 0xCF2,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02,
};

#define RT_GL_FUNCS(X) \
    X(void, glClear, (GLbitfield m)) \
    X(void, glClearColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a)) \
    X(void, glClearDepth, (GLdouble d)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glEnable, (GLenum c)) \
    X(void, glDisable, (GLenum c)) \
    X(void, glBlendFuncSeparate, (GLenum sr, GLenum dr, GLenum sa, GLenum da)) \
    X(void, glBlendEquation, (GLenum m)) \
    X(void, glBlendColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a)) \
    X(void, glDepthFunc, (GLenum f)) \
    X(void, glDepthMask, (GLboolean f)) \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
    X(void, glCullFace, (GLenum m)) \
    X(void, glFrontFace, (GLenum m)) \
    X(const GLubyte *, glGetString, (GLenum n)) \
    X(GLenum, glGetError, (void)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glPixelStorei, (GLenum p, GLint v)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *d)) \
    X(void, glGenTextures, (GLsizei n, GLuint *t)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *t)) \
    X(void, glBindTexture, (GLenum t, GLuint id)) \
    X(void, glActiveTexture, (GLenum u)) \
    X(void, glTexImage2D, (GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d)) \
    X(void, glTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *d)) \
    X(void, glTexParameteri, (GLenum t, GLenum p, GLint v)) \
    X(void, glGenSamplers, (GLsizei n, GLuint *s)) \
    X(void, glBindSampler, (GLuint unit, GLuint s)) \
    X(void, glSamplerParameteri, (GLuint s, GLenum p, GLint v)) \
    X(void, glDrawArrays, (GLenum m, GLint first, GLsizei n)) \
    X(void, glDrawElements, (GLenum m, GLsizei n, GLenum t, const void *idx)) \
    X(void, glGenBuffers, (GLsizei n, GLuint *b)) \
    X(void, glBindBuffer, (GLenum t, GLuint b)) \
    X(void, glBufferData, (GLenum t, GLsizeiptr s, const void *d, GLenum u)) \
    X(void, glBufferSubData, (GLenum t, GLintptr o, GLsizeiptr s, const void *d)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *a)) \
    X(void, glBindVertexArray, (GLuint a)) \
    X(void, glEnableVertexAttribArray, (GLuint i)) \
    X(void, glDisableVertexAttribArray, (GLuint i)) \
    X(void, glVertexAttribPointer, (GLuint i, GLint n, GLenum t, GLboolean norm, GLsizei stride, const void *p)) \
    X(void, glVertexAttrib4f, (GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)) \
    X(GLuint, glCreateShader, (GLenum t)) \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len)) \
    X(void, glCompileShader, (GLuint s)) \
    X(void, glGetShaderiv, (GLuint s, GLenum p, GLint *v)) \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei n, GLsizei *l, GLchar *log)) \
    X(void, glDeleteShader, (GLuint s)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint p, GLuint s)) \
    X(void, glLinkProgram, (GLuint p)) \
    X(void, glGetProgramiv, (GLuint p, GLenum n, GLint *v)) \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei n, GLsizei *l, GLchar *log)) \
    X(void, glUseProgram, (GLuint p)) \
    X(void, glBindAttribLocation, (GLuint p, GLuint i, const GLchar *n)) \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar *n)) \
    X(void, glUniform1i, (GLint l, GLint v)) \
    X(void, glUniform4iv, (GLint l, GLsizei n, const GLint *v)) \
    X(void, glUniform2iv, (GLint l, GLsizei n, const GLint *v)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *f)) \
    X(void, glCompressedTexImage2D, (GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void *d)) \
    X(void, glGenerateMipmap, (GLenum t)) \
    X(void, glDeleteProgram, (GLuint p)) \
    X(void, glSamplerParameterf, (GLuint s, GLenum p, GLfloat v)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *f)) \
    X(void, glBindFramebuffer, (GLenum t, GLuint f)) \
    X(void, glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)) \
    X(void, glFramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint rb)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum t)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *r)) \
    X(void, glBindRenderbuffer, (GLenum t, GLuint r)) \
    X(void, glRenderbufferStorage, (GLenum t, GLenum f, GLsizei w, GLsizei h)) \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield m, GLenum f))

#define RT_GL_DECL(ret, name, args) typedef ret (GLAPIENTRY *PFN_##name) args; extern PFN_##name name;
RT_GL_FUNCS(RT_GL_DECL)
#undef RT_GL_DECL

/* platform (rt_plat_*.c): create a current GL 3.3 core context; 0 on failure */
int plat_gl_create(void);
void *plat_gl_proc(const char *name);
