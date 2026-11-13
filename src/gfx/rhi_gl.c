/* src/gfx/rhi.h over OpenGL 3.3 core: each call the GL calls the renderer made before it had an RHI, in the same
   order, so the picture is the same to the bit. */
#include "gfx/rhi_impl.h"

#include "gfx/gl.h"

#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_VENDOR
#define GL_VENDOR 0x1F00
#endif

#ifndef GL_EQUAL
#define GL_EQUAL 0x0202
#endif
#ifndef GL_GREATER
#define GL_GREATER 0x0204
#endif
#ifndef GL_NOTEQUAL
#define GL_NOTEQUAL 0x0205
#endif
#ifndef GL_GEQUAL
#define GL_GEQUAL 0x0206
#endif
#ifndef GL_REPLACE
#define GL_REPLACE 0x1E01
#endif

/* A unit no program reads: textures are bound there to be made or updated. */
#define NW_GL_SCRATCH_UNIT 15

struct NwRhiBuffer {
    NwRhiBufferKind kind;
    NwRhiFormat texels;
    GLuint name, tex;
};

struct NwRhiTexture {
    NwRhiFormat format;
    int w, h, levels, samples;
    unsigned flags;
    GLuint name; /* a texture, or a renderbuffer when samples > 1 */
    GLuint fbo;  /* a colour target's framebuffer: it, and the depth last set with it */
};

struct NwRhiProgram {
    GLuint name;
};

struct NwRhiVertexArray {
    GLuint vao;
    NwRhiBuffer *ib;
};

static struct {
    SDL_Window *window;
    SDL_GLContext gl;
    NwRhiCaps caps;
    char description[256];
    NwRhiTexture *target; /* NULL: the window */
    GLuint level_fbo;     /* reads a level other than 0 */
    int viewport[4];
} nw_gl;

static SDL_WindowFlags nw_gl_prepare_window(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    return SDL_WINDOW_OPENGL;
}

static const char *nw_gl_string(GLenum name)
{
    const GLubyte *s = glGetString(name);
    return s ? (const char *)s : "?";
}

static bool nw_gl_init(SDL_Window *window)
{
    nw_gl.window = window;
    nw_gl.gl = SDL_GL_CreateContext(window);
    if (!nw_gl.gl) {
        return SDL_SetError("OpenGL 3.3 is required: %s", SDL_GetError());
    }
    if (!nw_gl_load()) {
        SDL_GL_DestroyContext(nw_gl.gl);
        nw_gl.gl = NULL;
        return SDL_SetError("Could not load the OpenGL functions");
    }
    SDL_GL_SetSwapInterval(1);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &nw_gl.caps.max_texture);
    glGetIntegerv(GL_MAX_SAMPLES, &nw_gl.caps.max_samples);
    SDL_snprintf(nw_gl.description, sizeof(nw_gl.description), "OpenGL %s, %s, %s", nw_gl_string(GL_VERSION),
                 nw_gl_string(GL_RENDERER), nw_gl_string(GL_VENDOR));
    SDL_Log("OpenGL %s (%s)", nw_gl_string(GL_VERSION), nw_gl_string(GL_RENDERER));
    return true;
}

static void nw_gl_shutdown(void)
{
    if (nw_gl.level_fbo) {
        glDeleteFramebuffers(1, &nw_gl.level_fbo);
    }
    if (nw_gl.gl) {
        SDL_GL_DestroyContext(nw_gl.gl);
    }
    SDL_zero(nw_gl);
}

static void *nw_gl_native(int which)
{
    return which == 0 ? (void *)nw_gl.gl : NULL;
}

static const char *nw_gl_description(void)
{
    return nw_gl.description;
}

static const NwRhiCaps *nw_gl_caps(void)
{
    return &nw_gl.caps;
}

static bool nw_gl_set_vsync(int interval)
{
    return SDL_GL_SetSwapInterval(interval);
}

static int nw_gl_vsync(void)
{
    int interval = 1;
    SDL_GL_GetSwapInterval(&interval);
    return interval;
}

static void nw_gl_present(void)
{
    SDL_GL_SwapWindow(nw_gl.window);
}

/* ---- buffers ---- */

static NwRhiBuffer *nw_gl_buffer_create(NwRhiBufferKind kind, NwRhiFormat texels)
{
    NwRhiBuffer *b = SDL_calloc(1, sizeof(*b));
    if (b) {
        b->kind = kind;
        b->texels = texels;
        glGenBuffers(1, &b->name);
    }
    return b;
}

static void nw_gl_buffer_data(NwRhiBuffer *b, size_t size, const void *data, bool stream)
{
    GLenum usage = stream ? GL_STREAM_DRAW : GL_STATIC_DRAW;
    if (b->kind == NW_RHI_TEXELS) {
        glBindBuffer(GL_TEXTURE_BUFFER, b->name);
        glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)size, data, usage);
        if (!b->tex) {
            glGenTextures(1, &b->tex);
        }
        glActiveTexture(GL_TEXTURE0 + NW_GL_SCRATCH_UNIT);
        glBindTexture(GL_TEXTURE_BUFFER, b->tex);
        glTexBuffer(GL_TEXTURE_BUFFER, b->texels == NW_RHI_R32F ? GL_R32F : GL_RGBA32F, b->name);
        glActiveTexture(GL_TEXTURE0);
        return;
    }
    /* Not GL_ELEMENT_ARRAY_BUFFER, which would change the bound vertex array's. */
    glBindBuffer(GL_COPY_WRITE_BUFFER, b->name);
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, data, usage);
}

static void nw_gl_buffer_destroy(NwRhiBuffer *b)
{
    if (b) {
        glDeleteBuffers(1, &b->name);
        glDeleteTextures(1, &b->tex);
        SDL_free(b);
    }
}

/* ---- textures ---- */

static void nw_gl_layout(NwRhiFormat f, GLint *internal, GLenum *layout, GLenum *type)
{
    *type = GL_UNSIGNED_BYTE;
    switch (f) {
    case NW_RHI_RGBA8:
        *internal = GL_RGBA8;
        *layout = GL_RGBA;
        break;
    case NW_RHI_D24S8:
        *internal = GL_DEPTH24_STENCIL8;
        *layout = GL_DEPTH_STENCIL;
        *type = GL_UNSIGNED_INT_24_8;
        break;
    case NW_RHI_RGBA8UI:
        *internal = GL_RGBA8UI;
        *layout = GL_RGBA_INTEGER;
        break;
    case NW_RHI_R8UI:
        *internal = GL_R8UI;
        *layout = GL_RED_INTEGER;
        break;
    default:
        *internal = GL_RGBA32F;
        *layout = GL_RGBA;
        *type = GL_FLOAT;
        break;
    }
}

static NwRhiTexture *nw_gl_texture_create(NwRhiFormat format, int width, int height, int levels, int samples,
                                          unsigned flags, const void *const *data)
{
    NwRhiTexture *t = SDL_calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    *t = (NwRhiTexture){ .format = format, .w = width, .h = height, .levels = levels, .samples = samples,
                         .flags = flags };
    GLint internal;
    GLenum layout, type;
    nw_gl_layout(format, &internal, &layout, &type);
    if (samples > 1) {
        glGenRenderbuffers(1, &t->name);
        glBindRenderbuffer(GL_RENDERBUFFER, t->name);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, (GLenum)internal, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        return t;
    }
    glGenTextures(1, &t->name);
    glActiveTexture(GL_TEXTURE0 + NW_GL_SCRATCH_UNIT);
    glBindTexture(GL_TEXTURE_2D, t->name);
    if (flags & NW_RHI_TEX_TARGET) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else if (flags & NW_RHI_TEX_LINEAR) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        /* Integer textures are only read by texelFetch; a mip chain needs a mipmap MIN_FILTER to be complete. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, format == NW_RHI_R8UI ? 1 : 4);
    for (int i = 0; i < levels; i++) {
        glTexImage2D(GL_TEXTURE_2D, i, internal, SDL_max(width >> i, 1), SDL_max(height >> i, 1), 0, layout, type,
                     data ? data[i] : NULL);
    }
    glActiveTexture(GL_TEXTURE0);
    return t;
}

static void nw_gl_texture_update(NwRhiTexture *t, int level, int x, int y, int width, int height, const void *data)
{
    GLint internal;
    GLenum layout, type;
    nw_gl_layout(t->format, &internal, &layout, &type);
    glActiveTexture(GL_TEXTURE0 + NW_GL_SCRATCH_UNIT);
    glBindTexture(GL_TEXTURE_2D, t->name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, t->format == NW_RHI_R8UI ? 1 : 4);
    glTexSubImage2D(GL_TEXTURE_2D, level, x, y, width, height, layout, type, data);
    glActiveTexture(GL_TEXTURE0);
}

static void nw_gl_texture_destroy(NwRhiTexture *t)
{
    if (!t) {
        return;
    }
    if (nw_gl.target == t) {
        nw_gl.target = NULL;
    }
    if (t->fbo) {
        glDeleteFramebuffers(1, &t->fbo);
    }
    if (t->samples > 1) {
        glDeleteRenderbuffers(1, &t->name);
    } else {
        glDeleteTextures(1, &t->name);
    }
    SDL_free(t);
}

static void nw_gl_texture_size(const NwRhiTexture *t, int *width, int *height)
{
    *width = t ? t->w : 0;
    *height = t ? t->h : 0;
}

static uint64_t nw_gl_texture_ui(NwRhiTexture *t)
{
    return t ? t->name : 0;
}

static void nw_gl_generate_mips(NwRhiTexture *t)
{
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, t->name);
    glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
}

static void nw_gl_attach(GLenum target, GLenum attachment, NwRhiTexture *t)
{
    if (t && t->samples > 1) {
        glFramebufferRenderbuffer(target, attachment, GL_RENDERBUFFER, t->name);
    } else {
        glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, t ? t->name : 0, 0);
    }
}

/* The colour target's framebuffer bound to `target`, with depth attached when it is not NULL. */
static GLuint nw_gl_fbo(GLenum target, NwRhiTexture *colour, NwRhiTexture *depth)
{
    if (!colour) {
        glBindFramebuffer(target, 0);
        return 0;
    }
    if (!colour->fbo) {
        glGenFramebuffers(1, &colour->fbo);
        glBindFramebuffer(target, colour->fbo);
        nw_gl_attach(target, GL_COLOR_ATTACHMENT0, colour);
    } else {
        glBindFramebuffer(target, colour->fbo);
    }
    if (depth) { /* every time: a freed and remade texture can come back at the same address or name */
        nw_gl_attach(target, GL_DEPTH_STENCIL_ATTACHMENT, depth);
    }
    return colour->fbo;
}

static bool nw_gl_texture_read(NwRhiTexture *t, int x, int y, int width, int height, void *rgba, int pitch)
{
    nw_gl_fbo(GL_READ_FRAMEBUFFER, t, NULL);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int i = 0; i < height; i++) {
        glReadPixels(x, y + i, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t *)rgba + (size_t)i * (size_t)pitch);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, nw_gl.target ? nw_gl.target->fbo : 0);
    return true;
}

/* ---- programs ---- */

static GLuint nw_gl_shader(GLenum type, const NwRhiProgramDesc *d, const char *src, size_t size)
{
    static const char head[] = "#version 330 core\n";
    static const char tail[] = "#define NW_BINDING(n)\n";
    const GLchar *parts[4] = { head, d->defines ? d->defines : "", tail, src };
    const GLint lengths[4] = { -1, -1, -1, (GLint)size };
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 4, parts, lengths);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        SDL_SetError("%s: shader: %s", d->set, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static NwRhiProgram *nw_gl_program_create(const NwRhiProgramDesc *d)
{
    GLuint vs = nw_gl_shader(GL_VERTEX_SHADER, d, d->vs, d->vs_size);
    GLuint fs = vs ? nw_gl_shader(GL_FRAGMENT_SHADER, d, d->fs, d->fs_size) : 0;
    if (!fs) {
        glDeleteShader(vs);
        return NULL;
    }
    GLint ok = 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        SDL_SetError("%s: program: %s", d->set, log);
        glDeleteProgram(p);
        return NULL;
    }
    NwRhiProgram *prog = SDL_calloc(1, sizeof(*prog));
    if (!prog) {
        glDeleteProgram(p);
        return NULL;
    }
    prog->name = p;
    glUseProgram(p);
    for (int i = 0; d->samplers && d->samplers[i]; i++) {
        glUniform1i(glGetUniformLocation(p, d->samplers[i]), i);
    }
    return prog;
}

static void nw_gl_program_destroy(NwRhiProgram *p)
{
    if (p) {
        glDeleteProgram(p->name);
        SDL_free(p);
    }
}

static int nw_gl_uniform(NwRhiProgram *p, const char *name)
{
    return glGetUniformLocation(p->name, name);
}

static void nw_gl_use_program(NwRhiProgram *p)
{
    glUseProgram(p ? p->name : 0);
}

/* Takes the program in use, as glUniform does: the caller has just used it. */
static void nw_gl_set_uniform(NwRhiProgram *p, int u, NwRhiUniformType type, int components, int count,
                              const void *v)
{
    (void)p;
    if (u < 0) {
        return;
    }
    switch (type) {
    case NW_RHI_MAT3:
        glUniformMatrix3fv(u, count, GL_FALSE, v);
        break;
    case NW_RHI_MAT4:
        glUniformMatrix4fv(u, count, GL_FALSE, v);
        break;
    case NW_RHI_FLOAT:
        switch (components) {
        case 1: glUniform1fv(u, count, v); break;
        case 2: glUniform2fv(u, count, v); break;
        case 3: glUniform3fv(u, count, v); break;
        default: glUniform4fv(u, count, v); break;
        }
        break;
    case NW_RHI_INT:
        switch (components) {
        case 1: glUniform1iv(u, count, v); break;
        case 2: glUniform2iv(u, count, v); break;
        case 3: glUniform3iv(u, count, v); break;
        default: glUniform4iv(u, count, v); break;
        }
        break;
    case NW_RHI_UINT:
        switch (components) {
        case 1: glUniform1uiv(u, count, v); break;
        case 2: glUniform2uiv(u, count, v); break;
        case 3: glUniform3uiv(u, count, v); break;
        default: glUniform4uiv(u, count, v); break;
        }
        break;
    }
}

/* ---- vertex arrays ---- */

static NwRhiVertexArray *nw_gl_vertex_array_create(void)
{
    NwRhiVertexArray *va = SDL_calloc(1, sizeof(*va));
    if (va) {
        glGenVertexArrays(1, &va->vao);
    }
    return va;
}

static void nw_gl_vertex_array_set(NwRhiVertexArray *va, const NwRhiVertexLayout *l, NwRhiBuffer *vertices,
                                   size_t offset, NwRhiBuffer *indices)
{
    static const GLenum types[] = { GL_FLOAT, GL_SHORT, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT,
                                    GL_UNSIGNED_INT };
    glBindVertexArray(va->vao);
    if (indices) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices->name);
        va->ib = indices;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vertices ? vertices->name : 0);
    for (int i = 0; i < l->count; i++) {
        const NwRhiAttrib *a = &l->attribs[i];
        const void *at = (const void *)(offset + a->offset);
        glEnableVertexAttribArray(a->location);
        if (a->type >= NW_RHI_U8_INT) {
            glVertexAttribIPointer(a->location, a->components, types[a->type], l->stride, at);
        } else {
            glVertexAttribPointer(a->location, a->components, types[a->type], GL_FALSE, l->stride, at);
        }
        if (l->instanced) {
            glVertexAttribDivisor(a->location, 1);
        }
    }
}

static void nw_gl_vertex_array_destroy(NwRhiVertexArray *va)
{
    if (va) {
        glDeleteVertexArrays(1, &va->vao);
        SDL_free(va);
    }
}

/* ---- state ---- */

static void nw_gl_set_target(NwRhiTexture *colour, NwRhiTexture *depth)
{
    nw_gl_fbo(GL_FRAMEBUFFER, colour, depth);
    nw_gl.target = colour;
}

static void nw_gl_viewport(int x, int y, int width, int height)
{
    glViewport(x, y, width, height);
    nw_gl.viewport[0] = x;
    nw_gl.viewport[1] = y;
    nw_gl.viewport[2] = width;
    nw_gl.viewport[3] = height;
}

static void nw_gl_viewport_get(int out[4])
{
    glGetIntegerv(GL_VIEWPORT, out);
}

static const GLenum nw_gl_ops[] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT };
static const GLenum nw_gl_factors[] = { GL_ZERO,      GL_ONE,       GL_SRC_ALPHA,          GL_ONE_MINUS_SRC_ALPHA,
                                        GL_DST_COLOR, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
static const GLenum nw_gl_compare[] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL,
                                        GL_ALWAYS };
static const GLenum nw_gl_stencil_ops[] = { GL_KEEP, GL_INCR_WRAP, GL_DECR_WRAP, GL_REPLACE };

static void nw_gl_blend(bool on, NwRhiBlendOp op, NwRhiBlendFactor src, NwRhiBlendFactor dst, NwRhiBlendOp aop,
                        NwRhiBlendFactor asrc, NwRhiBlendFactor adst)
{
    if (!on) {
        glDisable(GL_BLEND);
        return;
    }
    glEnable(GL_BLEND);
    glBlendEquationSeparate(nw_gl_ops[op], nw_gl_ops[aop]);
    glBlendFuncSeparate(nw_gl_factors[src], nw_gl_factors[dst], nw_gl_factors[asrc], nw_gl_factors[adst]);
}

static void nw_gl_colour_mask(bool r, bool g, bool b, bool a)
{
    glColorMask(r, g, b, a);
}

static void nw_gl_depth(bool test, NwRhiCompare func)
{
    if (test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthFunc(nw_gl_compare[func]);
}

static void nw_gl_depth_write(bool on)
{
    glDepthMask(on);
}

static void nw_gl_stencil(bool test, NwRhiCompare func, uint8_t ref, uint8_t mask, NwRhiStencilOp fail,
                          NwRhiStencilOp zfail, NwRhiStencilOp pass)
{
    if (!test) {
        glDisable(GL_STENCIL_TEST);
        return;
    }
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(nw_gl_compare[func], ref, mask);
    glStencilOp(nw_gl_stencil_ops[fail], nw_gl_stencil_ops[zfail], nw_gl_stencil_ops[pass]);
}

static void nw_gl_stencil_write_mask(uint8_t mask)
{
    glStencilMask(mask);
}

static void nw_gl_wireframe(bool on)
{
    glPolygonMode(GL_FRONT_AND_BACK, on ? GL_LINE : GL_FILL);
}

static void nw_gl_bind_texture(int unit, NwRhiTexture *t)
{
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, t ? t->name : 0);
    glActiveTexture(GL_TEXTURE0);
}

static void nw_gl_bind_texels(int unit, NwRhiBuffer *b)
{
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_BUFFER, b ? b->tex : 0);
    glActiveTexture(GL_TEXTURE0);
}

/* ---- work ---- */

static void nw_gl_draw(NwRhiVertexArray *va, uint32_t first, uint32_t count)
{
    glBindVertexArray(va->vao);
    glDrawArrays(GL_TRIANGLES, (GLint)first, (GLsizei)count);
}

static void nw_gl_draw_indexed(NwRhiVertexArray *va, NwRhiBuffer *indices, uint32_t first, uint32_t count)
{
    glBindVertexArray(va->vao);
    if (indices && indices != va->ib) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices->name);
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_INT, (const void *)((uintptr_t)first * 4));
    if (indices && indices != va->ib) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, va->ib ? va->ib->name : 0);
    }
}

static void nw_gl_draw_instanced(NwRhiVertexArray *va, uint32_t first, uint32_t count, uint32_t instances)
{
    glBindVertexArray(va->vao);
    glDrawArraysInstanced(GL_TRIANGLES, (GLint)first, (GLsizei)count, (GLsizei)instances);
}

static void nw_gl_clear(const NwRhiClear *c)
{
    if (c->scissor) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(c->scissor[0], c->scissor[1], c->scissor[2], c->scissor[3]);
    }
    GLbitfield bits = 0;
    if (c->colour) {
        glColorMask(c->mask[0], c->mask[1], c->mask[2], c->mask[3]);
        glClearColor(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (c->depth) {
        glDepthMask(c->depth_write);
        glClearDepth(c->z);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (c->stencil) {
        glStencilMask(c->stencil_write);
        glClearStencil(c->s);
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    glClear(bits);
    if (c->scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
}

static void nw_gl_blit(const NwRhiBlit *b)
{
    if (b->src_level) {
        if (!nw_gl.level_fbo) {
            glGenFramebuffers(1, &nw_gl.level_fbo);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, nw_gl.level_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->src_colour->name,
                               b->src_level);
    } else {
        nw_gl_fbo(GL_READ_FRAMEBUFFER, b->src_colour, b->src_depth);
    }
    nw_gl_fbo(GL_DRAW_FRAMEBUFFER, b->dst_colour, b->dst_depth);
    GLbitfield mask = (b->colour ? GL_COLOR_BUFFER_BIT : 0) | (b->depth ? GL_DEPTH_BUFFER_BIT : 0) |
                      (b->stencil ? GL_STENCIL_BUFFER_BIT : 0);
    glBlitFramebuffer(b->src[0], b->src[1], b->src[2], b->src[3], b->dst[0], b->dst[1], b->dst[2], b->dst[3], mask,
                      b->linear ? GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, nw_gl.target ? nw_gl.target->fbo : 0);
}

#define NW_GL_ENTRY_X(name, ret, params, args) .name = nw_gl_##name,
#define NW_GL_ENTRY_V(name, params, args) .name = nw_gl_##name,
const NwRhiBackend nw_rhi_gl = { .prepare_window = nw_gl_prepare_window,
                                 NW_RHI_CALLS(NW_GL_ENTRY_X, NW_GL_ENTRY_V) };
