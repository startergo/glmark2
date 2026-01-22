/*
 * Copyright © 2010-2011 Linaro Limited
 *
 * This file is part of the glmark2 OpenGL (ES) 2.0 benchmark.
 *
 * glmark2 is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * glmark2 is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * glmark2.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *  Alexandros Frantzis (glmark2)
 */
#include "gl-headers.h"

void* (GLAD_API_PTR *GLExtensions::MapBuffer) (GLenum target, GLenum access) = 0;
GLboolean (GLAD_API_PTR *GLExtensions::UnmapBuffer) (GLenum target) = 0;

void (GLAD_API_PTR *GLExtensions::GenFramebuffers)(GLsizei n, GLuint *framebuffers) = 0;
void (GLAD_API_PTR *GLExtensions::DeleteFramebuffers)(GLsizei n, const GLuint * framebuffers) = 0;
void (GLAD_API_PTR *GLExtensions::BindFramebuffer)(GLenum target, GLuint framebuffer) = 0;
void (GLAD_API_PTR *GLExtensions::FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) = 0;
void (GLAD_API_PTR *GLExtensions::FramebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) = 0;
GLenum (GLAD_API_PTR *GLExtensions::CheckFramebufferStatus)(GLenum target) = 0;

void (GLAD_API_PTR *GLExtensions::GenRenderbuffers)(GLsizei n, GLuint * renderbuffers) = 0;
void (GLAD_API_PTR *GLExtensions::DeleteRenderbuffers)(GLsizei n, const GLuint * renderbuffers) = 0;
void (GLAD_API_PTR *GLExtensions::BindRenderbuffer)(GLenum target, GLuint renderbuffer) = 0;
void (GLAD_API_PTR *GLExtensions::RenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) = 0;

void (GLAD_API_PTR *GLExtensions::GenerateMipmap)(GLenum target) = 0;

bool
GLExtensions::support(const std::string &ext)
{
    // Some features that were historically exposed via extensions are core in
    // modern desktop OpenGL, and may not appear in the extension string list
    // in core profile contexts.
    if (ext == "GL_ARB_depth_texture") {
#if GLMARK2_USE_GL
        if (GLExtensions::is_core_profile())
            return true;
#endif
    }

    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (exts && *exts) {
        const std::string ext_string(exts);
        size_t pos = 0;
        while ((pos = ext_string.find(ext, pos)) != std::string::npos) {
            const bool ok_before = (pos == 0) || (ext_string[pos - 1] == ' ');
            const size_t after = pos + ext.size();
            const bool ok_after = (after == ext_string.size()) || (ext_string[after] == ' ');
            if (ok_before && ok_after)
                return true;
            pos = after;
        }
        return false;
    }

#ifdef GL_NUM_EXTENSIONS
    GLint num_ext = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &num_ext);
    if (glGetError() == GL_NO_ERROR && num_ext > 0) {
        for (GLint i = 0; i < num_ext; ++i) {
            const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
            if (e && ext == e)
                return true;
        }
    }
#endif

    return false;
}

bool
GLExtensions::is_core_profile()
{
#if GLMARK2_USE_GL
#ifdef GL_CONTEXT_PROFILE_MASK
    GLint mask = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
    if (glGetError() == GL_NO_ERROR && mask != 0) {
#ifdef GL_CONTEXT_CORE_PROFILE_BIT
        return (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
#else
        /* If we can query the mask but don't have the bit constant, be conservative. */
        return false;
#endif
    }
#endif

    /* Fallback: in desktop GL core profile, GL_EXTENSIONS via glGetString is invalid. */
    const GLubyte* exts = glGetString(GL_EXTENSIONS);
    if (!exts)
        return true;

    const GLubyte* ver = glGetString(GL_VERSION);
    if (ver && std::string(reinterpret_cast<const char*>(ver)).find("OpenGL ES") != std::string::npos)
        return false;

    return false;
#else
    return false;
#endif
}
