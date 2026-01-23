#if GLMARK2_USE_MACOS

#include "scene.h"
#include "gl-headers.h"
#include "options.h"
#include "log.h"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "mat.h"
#include "stack.h"

namespace {

#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8A11
#endif
#ifndef GL_PROGRAM_SEPARABLE
#define GL_PROGRAM_SEPARABLE 0x8258
#endif
#ifndef GL_VERTEX_SHADER_BIT
#define GL_VERTEX_SHADER_BIT 0x00000001
#endif
#ifndef GL_FRAGMENT_SHADER_BIT
#define GL_FRAGMENT_SHADER_BIT 0x00000002
#endif
#ifndef GL_GEOMETRY_SHADER
#define GL_GEOMETRY_SHADER 0x8DD9
#endif
#ifndef GL_INVALID_INDEX
#define GL_INVALID_INDEX 0xFFFFFFFFu
#endif

#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif

// SceneBlock std140 layout: mat4 (16 floats) + vec4 (4) + vec4 (4).
constexpr size_t kSceneBlockSize = sizeof(float) * 24;
// ModelBlock stores one vec4 per instance; 16x16 grid => 256 entries.
constexpr size_t kModelBlockVec4s = 256;
constexpr size_t kModelBlockSize = kModelBlockVec4s * sizeof(float) * 4;
constexpr unsigned kModelBlockMaxGrid = 16; // sqrt(kModelBlockVec4s)
constexpr size_t kStreamingBufferAlignment = 256;
// Power-of-two alignment is required for efficient GL buffer offset calculations
// and to meet mapping alignment requirements on some drivers.
static_assert((kStreamingBufferAlignment & (kStreamingBufferAlignment - 1u)) == 0,
              "kStreamingBufferAlignment must be a power of two for GL buffer offset alignment");

using PFNGLGENVERTEXARRAYSPROC = void (*)(GLsizei, GLuint*);
using PFNGLBINDVERTEXARRAYPROC = void (*)(GLuint);
using PFNGLDELETEVERTEXARRAYSPROC = void (*)(GLsizei, const GLuint*);

using PFNGLDRAWARRAYSINSTANCEDPROC = void (*)(GLenum, GLint, GLsizei, GLsizei);
using PFNGLVERTEXATTRIBDIVISORPROC = void (*)(GLuint, GLuint);

using PFNGLBINDBUFFERBASEPROC = void (*)(GLenum, GLuint, GLuint);
using PFNGLGETUNIFORMBLOCKINDEXPROC = GLuint (*)(GLuint, const GLchar*);
using PFNGLUNIFORMBLOCKBINDINGPROC = void (*)(GLuint, GLuint, GLuint);

using PFNGLGENPROGRAMPIPELINESPROC = void (*)(GLsizei, GLuint*);
using PFNGLBINDPROGRAMPIPELINEPROC = void (*)(GLuint);
using PFNGLUSEPROGRAMSTAGESPROC = void (*)(GLuint, GLbitfield, GLuint);
using PFNGLDELETEPROGRAMPIPELINESPROC = void (*)(GLsizei, const GLuint*);
using PFNGLPROGRAMPARAMETERIPROC = void (*)(GLuint, GLenum, GLint);
using PFNGLBINDFRAGDATALOCATIONPROC = void (*)(GLuint, GLuint, const GLchar*);

using PFNGLENABLEIPROC = void (*)(GLenum, GLuint);
using PFNGLDISABLEIPROC = void (*)(GLenum, GLuint);
using PFNGLBLENDFUNCIPROC = void (*)(GLuint, GLenum, GLenum);
using PFNGLBLENDEQUATIONIPROC = void (*)(GLuint, GLenum);

using PFNGLBLITFRAMEBUFFERPROC = void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

using PFNGLMAPBUFFERRANGEPROC = void* (*)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
using PFNGLUNMAPBUFFERPROC = GLboolean (*)(GLenum);

using PFNGLFENCESYNCPROC = GLsync (*)(GLenum, GLbitfield);
using PFNGLCLIENTWAITSYNCPROC = GLenum (*)(GLsync, GLbitfield, GLuint64);
using PFNGLDELETESYNCPROC = void (*)(GLsync);

struct GL41Procs {
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays = nullptr;

    PFNGLDRAWARRAYSINSTANCEDPROC DrawArraysInstanced = nullptr;
    PFNGLVERTEXATTRIBDIVISORPROC VertexAttribDivisor = nullptr;

    PFNGLBINDBUFFERBASEPROC BindBufferBase = nullptr;
    PFNGLGETUNIFORMBLOCKINDEXPROC GetUniformBlockIndex = nullptr;
    PFNGLUNIFORMBLOCKBINDINGPROC UniformBlockBinding = nullptr;

    PFNGLGENPROGRAMPIPELINESPROC GenProgramPipelines = nullptr;
    PFNGLBINDPROGRAMPIPELINEPROC BindProgramPipeline = nullptr;
    PFNGLUSEPROGRAMSTAGESPROC UseProgramStages = nullptr;
    PFNGLDELETEPROGRAMPIPELINESPROC DeleteProgramPipelines = nullptr;
    PFNGLPROGRAMPARAMETERIPROC ProgramParameteri = nullptr;
    PFNGLBINDFRAGDATALOCATIONPROC BindFragDataLocation = nullptr;

    PFNGLENABLEIPROC Enablei = nullptr;
    PFNGLDISABLEIPROC Disablei = nullptr;
    PFNGLBLENDFUNCIPROC BlendFunci = nullptr;
    PFNGLBLENDEQUATIONIPROC BlendEquationi = nullptr;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = nullptr;

    PFNGLMAPBUFFERRANGEPROC MapBufferRange = nullptr;
    PFNGLUNMAPBUFFERPROC UnmapBuffer = nullptr;
    PFNGLFENCESYNCPROC FenceSync = nullptr;
    PFNGLCLIENTWAITSYNCPROC ClientWaitSync = nullptr;
    PFNGLDELETESYNCPROC DeleteSync = nullptr;

    bool load(bool show_errors)
    {
        auto sym = [show_errors](const char* name) -> void* {
            dlerror();
            void* ptr = dlsym(RTLD_DEFAULT, name);
            const char* err = dlerror();
            if ((!ptr || err) && show_errors) {
                const std::string err_str = err ? err : "";
                const bool not_found = (err_str.find("symbol not found") != std::string::npos);
                Log::info("gl41: %s %s: %s\n",
                          not_found ? "missing symbol" : "dlsym error for",
                          name,
                          err ? err : "<null symbol>");
            }
            return ptr;
        };

        GenVertexArrays = reinterpret_cast<PFNGLGENVERTEXARRAYSPROC>(sym("glGenVertexArrays"));
        BindVertexArray = reinterpret_cast<PFNGLBINDVERTEXARRAYPROC>(sym("glBindVertexArray"));
        DeleteVertexArrays = reinterpret_cast<PFNGLDELETEVERTEXARRAYSPROC>(sym("glDeleteVertexArrays"));

        DrawArraysInstanced = reinterpret_cast<PFNGLDRAWARRAYSINSTANCEDPROC>(sym("glDrawArraysInstanced"));
        VertexAttribDivisor = reinterpret_cast<PFNGLVERTEXATTRIBDIVISORPROC>(sym("glVertexAttribDivisor"));

        BindBufferBase = reinterpret_cast<PFNGLBINDBUFFERBASEPROC>(sym("glBindBufferBase"));
        GetUniformBlockIndex = reinterpret_cast<PFNGLGETUNIFORMBLOCKINDEXPROC>(sym("glGetUniformBlockIndex"));
        UniformBlockBinding = reinterpret_cast<PFNGLUNIFORMBLOCKBINDINGPROC>(sym("glUniformBlockBinding"));

        GenProgramPipelines = reinterpret_cast<PFNGLGENPROGRAMPIPELINESPROC>(sym("glGenProgramPipelines"));
        BindProgramPipeline = reinterpret_cast<PFNGLBINDPROGRAMPIPELINEPROC>(sym("glBindProgramPipeline"));
        UseProgramStages = reinterpret_cast<PFNGLUSEPROGRAMSTAGESPROC>(sym("glUseProgramStages"));
        DeleteProgramPipelines = reinterpret_cast<PFNGLDELETEPROGRAMPIPELINESPROC>(sym("glDeleteProgramPipelines"));
        ProgramParameteri = reinterpret_cast<PFNGLPROGRAMPARAMETERIPROC>(sym("glProgramParameteri"));
        BindFragDataLocation = reinterpret_cast<PFNGLBINDFRAGDATALOCATIONPROC>(sym("glBindFragDataLocation"));

        Enablei = reinterpret_cast<PFNGLENABLEIPROC>(sym("glEnablei"));
        Disablei = reinterpret_cast<PFNGLDISABLEIPROC>(sym("glDisablei"));
        BlendFunci = reinterpret_cast<PFNGLBLENDFUNCIPROC>(sym("glBlendFunci"));
        BlendEquationi = reinterpret_cast<PFNGLBLENDEQUATIONIPROC>(sym("glBlendEquationi"));

        BlitFramebuffer = reinterpret_cast<PFNGLBLITFRAMEBUFFERPROC>(sym("glBlitFramebuffer"));

        MapBufferRange = reinterpret_cast<PFNGLMAPBUFFERRANGEPROC>(sym("glMapBufferRange"));
        UnmapBuffer = reinterpret_cast<PFNGLUNMAPBUFFERPROC>(sym("glUnmapBuffer"));
        FenceSync = reinterpret_cast<PFNGLFENCESYNCPROC>(sym("glFenceSync"));
        ClientWaitSync = reinterpret_cast<PFNGLCLIENTWAITSYNCPROC>(sym("glClientWaitSync"));
        DeleteSync = reinterpret_cast<PFNGLDELETESYNCPROC>(sym("glDeleteSync"));

        const bool have_vao = (GenVertexArrays && BindVertexArray && DeleteVertexArrays);
        const bool have_instancing = (DrawArraysInstanced && VertexAttribDivisor);
        const bool have_ubo = (BindBufferBase && GetUniformBlockIndex && UniformBlockBinding);

        if (!have_vao || !have_instancing || !have_ubo) {
            if (show_errors) {
                Log::info("Scene requires core features (VAO + instancing + UBO). Missing:%s%s%s\n",
                          have_vao ? "" : " VAO",
                          have_instancing ? "" : " instancing",
                          have_ubo ? "" : " UBO");
            }
            return false;
        }
        return true;
    }
};

GLuint compile_shader(GLenum type, const std::string& src, std::string& err)
{
    GLuint sh = glCreateShader(type);
    if (!sh) {
        err = "glCreateShader failed";
        return 0;
    }

    const char* cstr = src.c_str();
    glShaderSource(sh, 1, &cstr, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log;
        if (len > 0) {
            log.resize(len);
            glGetShaderInfoLog(sh, len, nullptr, log.data());
        }
        err = log.empty() ? "shader compile failed" : log;
        glDeleteShader(sh);
        return 0;
    }

    return sh;
}

GLuint link_program(GLuint vs, GLuint fs, std::string& err)
{
    GLuint prog = glCreateProgram();
    if (!prog) {
        err = "glCreateProgram failed";
        return 0;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log;
        if (len > 0) {
            log.resize(len);
            glGetProgramInfoLog(prog, len, nullptr, log.data());
        }
        err = log.empty() ? "program link failed" : log;
        glDeleteProgram(prog);
        return 0;
    }

    return prog;
}

GLuint link_program(GLuint vs, GLuint gs, GLuint fs, std::string& err)
{
    GLuint prog = glCreateProgram();
    if (!prog) {
        err = "glCreateProgram failed";
        return 0;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, gs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log;
        if (len > 0) {
            log.resize(len);
            glGetProgramInfoLog(prog, len, nullptr, log.data());
        }
        err = log.empty() ? "program link failed" : log;
        glDeleteProgram(prog);
        return 0;
    }

    return prog;
}

std::string get_program_info_log(GLuint prog)
{
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    if (len <= 1)
        return std::string();

    std::string log;
    log.resize(len);
    glGetProgramInfoLog(prog, len, nullptr, log.data());
    return log;
}

} // namespace

struct SceneGL41InstancingPrivate
{
    GL41Procs procs;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo_pos = 0;
    GLuint vbo_offsets = 0;
    GLuint ubo = 0;
    unsigned instances = 0;
};

SceneGL41Instancing::SceneGL41Instancing(Canvas &pCanvas)
    : Scene(pCanvas, "gl41-instancing"), priv_(new SceneGL41InstancingPrivate)
{
    options_["instances"] = Scene::Option("instances", "4096", "Number of instances to draw");
}

SceneGL41Instancing::~SceneGL41Instancing()
{
    unload();
    delete priv_;
}

bool SceneGL41Instancing::supported(bool show_errors)
{
    if (Options::macos_gl_profile == Options::MacOSGLProfileLegacy) {
        if (show_errors)
            Log::info("gl41-instancing: disabled in legacy profile\n");
        return false;
    }
    if (!GLExtensions::is_core_profile()) {
        if (show_errors)
            Log::info("gl41-instancing: requires a core profile context\n");
        return false;
    }

    GL41Procs p;
    return p.load(show_errors);
}

bool SceneGL41Instancing::load()
{
    if (!priv_->procs.load(true))
        return false;

    static const std::string vtx =
        "#version 150\n"
        "in vec2 position;\n"
        "in vec2 instanceOffset;\n"
        "layout(std140) uniform Matrices { mat4 uMVP; };\n"
        "void main() {\n"
        "  vec2 pos = position + instanceOffset;\n"
        "  gl_Position = uMVP * vec4(pos, 0.0, 1.0);\n"
        "}\n";

    static const std::string frg =
        "#version 150\n"
        "out vec4 fragColor;\n"
        "void main() { fragColor = vec4(0.2, 0.8, 1.0, 1.0); }\n";

    std::string err;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vtx, err);
    if (!vs) {
        Log::error("gl41-instancing vertex shader: %s\n", err.c_str());
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frg, err);
    if (!fs) {
        Log::error("gl41-instancing fragment shader: %s\n", err.c_str());
        glDeleteShader(vs);
        return false;
    }

    priv_->program = link_program(vs, fs, err);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!priv_->program) {
        Log::error("gl41-instancing link: %s\n", err.c_str());
        return false;
    }

    priv_->procs.GenVertexArrays(1, &priv_->vao);
    priv_->procs.BindVertexArray(priv_->vao);

    const float quad[] = {
        -0.01f, -0.01f,
         0.01f, -0.01f,
        -0.01f,  0.01f,
        -0.01f,  0.01f,
         0.01f, -0.01f,
         0.01f,  0.01f,
    };

    glGenBuffers(1, &priv_->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    auto cleanup_instancing_load = [this]() {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        priv_->procs.BindVertexArray(0);
        if (priv_->vbo_pos) {
            glDeleteBuffers(1, &priv_->vbo_pos);
            priv_->vbo_pos = 0;
        }
        if (priv_->vbo_offsets) {
            glDeleteBuffers(1, &priv_->vbo_offsets);
            priv_->vbo_offsets = 0;
        }
        if (priv_->ubo) {
            glDeleteBuffers(1, &priv_->ubo);
            priv_->ubo = 0;
        }
        if (priv_->vao) {
            priv_->procs.DeleteVertexArrays(1, &priv_->vao);
            priv_->vao = 0;
        }
        if (priv_->program) {
            glDeleteProgram(priv_->program);
            priv_->program = 0;
        }
    };

    GLint pos_loc = glGetAttribLocation(priv_->program, "position");
    if (pos_loc < 0) {
        Log::error("gl41-instancing: missing vertex attribute 'position'\n");
        cleanup_instancing_load();
        return false;
    }
    glEnableVertexAttribArray(static_cast<GLuint>(pos_loc));
    glVertexAttribPointer(static_cast<GLuint>(pos_loc), 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));

    GLint off_loc = glGetAttribLocation(priv_->program, "instanceOffset");
    if (off_loc < 0) {
        Log::error("gl41-instancing: missing vertex attribute 'instanceOffset'\n");
        cleanup_instancing_load();
        return false;
    }
    glGenBuffers(1, &priv_->vbo_offsets);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo_offsets);
    glEnableVertexAttribArray(static_cast<GLuint>(off_loc));
    glVertexAttribPointer(static_cast<GLuint>(off_loc), 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));
    priv_->procs.VertexAttribDivisor(static_cast<GLuint>(off_loc), 1);

    glGenBuffers(1, &priv_->ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(float) * 16, nullptr, GL_DYNAMIC_DRAW);

    const GLuint block = priv_->procs.GetUniformBlockIndex(priv_->program, "Matrices");
    if (block == GL_INVALID_INDEX) {
        Log::error("gl41-instancing: missing uniform block 'Matrices'\n");
        cleanup_instancing_load();
        return false;
    }
    priv_->procs.UniformBlockBinding(priv_->program, block, 0);
    priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 0, priv_->ubo);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);
    return true;
}

void SceneGL41Instancing::unload()
{
    if (!priv_)
        return;

    if (priv_->vao) {
        priv_->procs.DeleteVertexArrays(1, &priv_->vao);
        priv_->vao = 0;
    }
    if (priv_->vbo_pos) {
        glDeleteBuffers(1, &priv_->vbo_pos);
        priv_->vbo_pos = 0;
    }
    if (priv_->vbo_offsets) {
        glDeleteBuffers(1, &priv_->vbo_offsets);
        priv_->vbo_offsets = 0;
    }
    if (priv_->ubo) {
        glDeleteBuffers(1, &priv_->ubo);
        priv_->ubo = 0;
    }
    if (priv_->program) {
        glDeleteProgram(priv_->program);
        priv_->program = 0;
    }
}

bool SceneGL41Instancing::setup()
{
    unsigned instances = 4096;
    try {
        instances = static_cast<unsigned>(std::stoul(options_["instances"].value));
    } catch (...) {
        instances = 4096;
    }
    priv_->instances = std::max(1u, instances);

    std::vector<float> offsets;
    offsets.reserve(static_cast<size_t>(priv_->instances) * 2);

    const unsigned side = static_cast<unsigned>(std::ceil(std::sqrt(static_cast<double>(priv_->instances))));
    const float span = 1.8f;
    const float step = (side > 1) ? (span / (side - 1)) : 0.0f;

    for (unsigned i = 0; i < priv_->instances; ++i) {
        const unsigned x = i % side;
        const unsigned y = i / side;
        offsets.push_back(-0.9f + step * x);
        offsets.push_back(-0.9f + step * y);
    }

    priv_->procs.BindVertexArray(priv_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo_offsets);
    glBufferData(GL_ARRAY_BUFFER, offsets.size() * sizeof(float), offsets.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);
    return true;
}

void SceneGL41Instancing::draw()
{
    const float t = static_cast<float>(currentFrame_) * 0.01f;
    const float c = std::cos(t);
    const float s = std::sin(t);
    const float mvp[16] = {
        c,  s, 0, 0,
       -s,  c, 0, 0,
        0,  0, 1, 0,
        0,  0, 0, 1,
    };

    glUseProgram(priv_->program);
    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(mvp), mvp);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    priv_->procs.BindVertexArray(priv_->vao);
    priv_->procs.DrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(priv_->instances));
    priv_->procs.BindVertexArray(0);
    glUseProgram(0);
}

struct SceneGL41PipelinePrivate
{
    GL41Procs procs;
    GLuint pipeline = 0;
    GLuint prog_vs = 0;
    GLuint prog_fs = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ubo_scene = 0;
    GLuint ubo_model = 0;
    int attr_position = -1;
    int attr_normal = -1;
    unsigned grid = 10;
    unsigned instance_count = 100;
};

SceneGL41Pipeline::SceneGL41Pipeline(Canvas &pCanvas)
    : Scene(pCanvas, "gl41-pipeline"), priv_(new SceneGL41PipelinePrivate)
{
    options_["grid"] = Scene::Option("grid", "10", "Cube grid size (NxN, clamps to 1..16)");
}

SceneGL41Pipeline::~SceneGL41Pipeline()
{
    unload();
    delete priv_;
}

bool SceneGL41Pipeline::supported(bool show_errors)
{
    if (Options::macos_gl_profile == Options::MacOSGLProfileLegacy) {
        if (show_errors)
            Log::info("gl41-pipeline: disabled in legacy profile\n");
        return false;
    }
    if (!GLExtensions::is_core_profile()) {
        if (show_errors)
            Log::info("gl41-pipeline: requires a core profile context\n");
        return false;
    }

    GL41Procs p;
    if (!p.load(show_errors))
        return false;

    const bool have_pipeline = (p.GenProgramPipelines && p.BindProgramPipeline && p.UseProgramStages && p.DeleteProgramPipelines && p.ProgramParameteri);
    if (!have_pipeline) {
        if (show_errors)
            Log::info("gl41-pipeline: missing program pipeline entry points\n");
        return false;
    }
    return true;
}

bool SceneGL41Pipeline::load()
{
    if (!priv_->procs.load(true))
        return false;
    if (!priv_->procs.ProgramParameteri || !priv_->procs.GenProgramPipelines || !priv_->procs.UseProgramStages ||
        !priv_->procs.BindProgramPipeline || !priv_->procs.DeleteProgramPipelines)
        return false;

    static const std::string vtx =
        "#version 150\n"
        "out gl_PerVertex { vec4 gl_Position; };\n"
        "in vec3 position;\n"
        "in vec3 normal;\n"
        "layout(std140) uniform SceneBlock { mat4 uVP; vec4 uLightDir; vec4 uTime; };\n"
        "layout(std140) uniform ModelBlock { vec4 uPosScale[256]; };\n"
        "out vec3 vN;\n"
        "out vec3 vWPos;\n"
        "void main() {\n"
        "  vec4 ps = uPosScale[gl_InstanceID];\n"
        "  float t = uTime.x;\n"
        "  float a = t * 1.2 + float(gl_InstanceID) * 0.11;\n"
        "  float b = t * 0.7 + float(gl_InstanceID) * 0.07;\n"
        "  float cy = cos(a), sy = sin(a);\n"
        "  float cx = cos(b), sx = sin(b);\n"
        "  mat3 rotY = mat3(cy,0,sy, 0,1,0, -sy,0,cy);\n"
        "  mat3 rotX = mat3(1,0,0, 0,cx,-sx, 0,sx,cx);\n"
        "  mat3 rot = rotY * rotX;\n"
        "  vec3 p = rot * (position * ps.w) + ps.xyz;\n"
        "  vWPos = p;\n"
        "  vN = rot * normal;\n"
        "  gl_Position = uVP * vec4(p, 1.0);\n"
        "}\n";

    static const std::string frg =
        "#version 150\n"
        "in vec3 vN;\n"
        "in vec3 vWPos;\n"
        "layout(std140) uniform SceneBlock { mat4 uVP; vec4 uLightDir; vec4 uTime; };\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  vec3 n = normalize(vN);\n"
        "  vec3 l = normalize(uLightDir.xyz);\n"
        "  float ndotl = max(dot(n, l), 0.0);\n"
        "  vec3 base = 0.35 + 0.35 * sin(vWPos * 2.0);\n"
        "  vec3 col = base * (0.25 + 0.75 * ndotl);\n"
        "  fragColor = vec4(col, 1.0);\n"
        "}\n";

    std::string err;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vtx, err);
    if (!vs) {
        Log::error("gl41-pipeline vertex shader: %s\n", err.c_str());
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frg, err);
    if (!fs) {
        Log::error("gl41-pipeline fragment shader: %s\n", err.c_str());
        glDeleteShader(vs);
        return false;
    }

    priv_->prog_vs = glCreateProgram();
    glAttachShader(priv_->prog_vs, vs);
    priv_->procs.ProgramParameteri(priv_->prog_vs, GL_PROGRAM_SEPARABLE, GL_TRUE);
    glLinkProgram(priv_->prog_vs);

    priv_->prog_fs = glCreateProgram();
    glAttachShader(priv_->prog_fs, fs);
    priv_->procs.ProgramParameteri(priv_->prog_fs, GL_PROGRAM_SEPARABLE, GL_TRUE);
    glLinkProgram(priv_->prog_fs);

    glDeleteShader(vs);
    glDeleteShader(fs);

    auto cleanup_pipeline_load = [this]() {
        if (priv_->procs.BindBufferBase) {
            priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
            priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 1, 0);
        }
        if (priv_->pipeline) {
            if (priv_->procs.DeleteProgramPipelines)
                priv_->procs.DeleteProgramPipelines(1, &priv_->pipeline);
            priv_->pipeline = 0;
        }
        if (priv_->prog_vs) {
            glDeleteProgram(priv_->prog_vs);
            priv_->prog_vs = 0;
        }
        if (priv_->prog_fs) {
            glDeleteProgram(priv_->prog_fs);
            priv_->prog_fs = 0;
        }
        if (priv_->vao) {
            priv_->procs.DeleteVertexArrays(1, &priv_->vao);
            priv_->vao = 0;
        }
        if (priv_->vbo) {
            glDeleteBuffers(1, &priv_->vbo);
            priv_->vbo = 0;
        }
        if (priv_->ubo_scene) {
            glDeleteBuffers(1, &priv_->ubo_scene);
            priv_->ubo_scene = 0;
        }
        if (priv_->ubo_model) {
            glDeleteBuffers(1, &priv_->ubo_model);
            priv_->ubo_model = 0;
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        priv_->procs.BindVertexArray(0);
    };

    GLint ok = 0;
    glGetProgramiv(priv_->prog_vs, GL_LINK_STATUS, &ok);
    if (!ok) {
        const std::string log = get_program_info_log(priv_->prog_vs);
        Log::error("gl41-pipeline: VS program link failed: %s\n", log.empty() ? "<no log>" : log.c_str());
        cleanup_pipeline_load();
        return false;
    }
    glGetProgramiv(priv_->prog_fs, GL_LINK_STATUS, &ok);
    if (!ok) {
        const std::string log = get_program_info_log(priv_->prog_fs);
        Log::error("gl41-pipeline: FS program link failed: %s\n", log.empty() ? "<no log>" : log.c_str());
        cleanup_pipeline_load();
        return false;
    }

    priv_->procs.GenProgramPipelines(1, &priv_->pipeline);
    priv_->procs.BindProgramPipeline(priv_->pipeline);
    priv_->procs.UseProgramStages(priv_->pipeline, GL_VERTEX_SHADER_BIT, priv_->prog_vs);
    priv_->procs.UseProgramStages(priv_->pipeline, GL_FRAGMENT_SHADER_BIT, priv_->prog_fs);
    priv_->procs.BindProgramPipeline(0);

    // UBOs (scene + per-model)
    glGenBuffers(1, &priv_->ubo_scene);
    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo_scene);
    glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(kSceneBlockSize), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glGenBuffers(1, &priv_->ubo_model);
    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo_model);
    glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(kModelBlockSize), nullptr, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // Bind uniform blocks for both stage programs.
    // SceneBlock is shared by VS and FS, so bind it in both; ModelBlock is VS-only.
    const GLuint scene_vs = priv_->procs.GetUniformBlockIndex(priv_->prog_vs, "SceneBlock");
    const GLuint scene_fs = priv_->procs.GetUniformBlockIndex(priv_->prog_fs, "SceneBlock");
    const GLuint model_vs = priv_->procs.GetUniformBlockIndex(priv_->prog_vs, "ModelBlock");
    if (scene_vs == GL_INVALID_INDEX || scene_fs == GL_INVALID_INDEX || model_vs == GL_INVALID_INDEX) {
        Log::error("gl41-pipeline: missing uniform blocks (SceneBlock/ModelBlock)\n");
        cleanup_pipeline_load();
        return false;
    }
    priv_->procs.UniformBlockBinding(priv_->prog_vs, scene_vs, 0);
    priv_->procs.UniformBlockBinding(priv_->prog_fs, scene_fs, 0);
    priv_->procs.UniformBlockBinding(priv_->prog_vs, model_vs, 1);
    priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 0, priv_->ubo_scene);
    priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 1, priv_->ubo_model);

    priv_->procs.GenVertexArrays(1, &priv_->vao);
    priv_->procs.BindVertexArray(priv_->vao);

    // Unit cube (36 verts), interleaved position + normal.
    const float verts[] = {
        // +X
        0.5f,-0.5f,-0.5f,  1,0,0,  0.5f, 0.5f,-0.5f,  1,0,0,  0.5f, 0.5f, 0.5f,  1,0,0,
        0.5f,-0.5f,-0.5f,  1,0,0,  0.5f, 0.5f, 0.5f,  1,0,0,  0.5f,-0.5f, 0.5f,  1,0,0,
        // -X
       -0.5f,-0.5f, 0.5f, -1,0,0, -0.5f, 0.5f, 0.5f, -1,0,0, -0.5f, 0.5f,-0.5f, -1,0,0,
       -0.5f,-0.5f, 0.5f, -1,0,0, -0.5f, 0.5f,-0.5f, -1,0,0, -0.5f,-0.5f,-0.5f, -1,0,0,
        // +Y
       -0.5f, 0.5f,-0.5f,  0,1,0, -0.5f, 0.5f, 0.5f,  0,1,0,  0.5f, 0.5f, 0.5f,  0,1,0,
       -0.5f, 0.5f,-0.5f,  0,1,0,  0.5f, 0.5f, 0.5f,  0,1,0,  0.5f, 0.5f,-0.5f,  0,1,0,
        // -Y
       -0.5f,-0.5f, 0.5f,  0,-1,0, -0.5f,-0.5f,-0.5f,  0,-1,0,  0.5f,-0.5f,-0.5f,  0,-1,0,
       -0.5f,-0.5f, 0.5f,  0,-1,0,  0.5f,-0.5f,-0.5f,  0,-1,0,  0.5f,-0.5f, 0.5f,  0,-1,0,
        // +Z
       -0.5f,-0.5f, 0.5f,  0,0,1,  0.5f,-0.5f, 0.5f,  0,0,1,  0.5f, 0.5f, 0.5f,  0,0,1,
       -0.5f,-0.5f, 0.5f,  0,0,1,  0.5f, 0.5f, 0.5f,  0,0,1, -0.5f, 0.5f, 0.5f,  0,0,1,
        // -Z
        0.5f,-0.5f,-0.5f,  0,0,-1, -0.5f,-0.5f,-0.5f,  0,0,-1, -0.5f, 0.5f,-0.5f,  0,0,-1,
        0.5f,-0.5f,-0.5f,  0,0,-1, -0.5f, 0.5f,-0.5f,  0,0,-1,  0.5f, 0.5f,-0.5f,  0,0,-1,
    };

    glGenBuffers(1, &priv_->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    priv_->attr_position = glGetAttribLocation(priv_->prog_vs, "position");
    priv_->attr_normal = glGetAttribLocation(priv_->prog_vs, "normal");
    if (priv_->attr_position < 0 || priv_->attr_normal < 0) {
        Log::error("gl41-pipeline: missing vertex attributes (position/normal)\n");
        cleanup_pipeline_load();
        return false;
    }

    const GLsizei stride = 6 * sizeof(float);
    glEnableVertexAttribArray(static_cast<GLuint>(priv_->attr_position));
    glVertexAttribPointer(static_cast<GLuint>(priv_->attr_position), 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(static_cast<GLuint>(priv_->attr_normal));
    glVertexAttribPointer(static_cast<GLuint>(priv_->attr_normal), 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);

    return true;
}

bool SceneGL41Pipeline::setup()
{
    unsigned grid = 10;
    try {
        grid = static_cast<unsigned>(std::stoul(options_["grid"].value));
    } catch (...) {
        grid = 10;
    }

    // Clamp to keep UBO array fixed at kModelBlockVec4s entries.
    if (grid < 1)
        grid = 1;
    if (grid > kModelBlockMaxGrid)
        grid = kModelBlockMaxGrid;

    priv_->grid = grid;
    priv_->instance_count = grid * grid;

    const float spacing = 1.6f;
    const float half = (static_cast<float>(grid) - 1.0f) * spacing * 0.5f;
    std::vector<float> pos_scale;
    pos_scale.resize(priv_->instance_count * 4);
    unsigned idx = 0;
    for (unsigned y = 0; y < grid; ++y) {
        for (unsigned x = 0; x < grid; ++x) {
            pos_scale[idx * 4 + 0] = -half + x * spacing;
            pos_scale[idx * 4 + 1] = -half + y * spacing;
            pos_scale[idx * 4 + 2] = 0.0f;
            pos_scale[idx * 4 + 3] = 0.7f;
            ++idx;
        }
    }

    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo_model);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, pos_scale.size() * sizeof(float), pos_scale.data());
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return true;
}

void SceneGL41Pipeline::unload()
{
    if (!priv_)
        return;

    if (priv_->procs.BindBufferBase) {
        priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
        priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 1, 0);
    }
    if (priv_->pipeline) {
        if (priv_->procs.DeleteProgramPipelines)
            priv_->procs.DeleteProgramPipelines(1, &priv_->pipeline);
        priv_->pipeline = 0;
    }
    if (priv_->prog_vs) {
        glDeleteProgram(priv_->prog_vs);
        priv_->prog_vs = 0;
    }
    if (priv_->prog_fs) {
        glDeleteProgram(priv_->prog_fs);
        priv_->prog_fs = 0;
    }
    if (priv_->vao) {
        priv_->procs.DeleteVertexArrays(1, &priv_->vao);
        priv_->vao = 0;
    }
    if (priv_->vbo) {
        glDeleteBuffers(1, &priv_->vbo);
        priv_->vbo = 0;
    }
    if (priv_->ubo_scene) {
        glDeleteBuffers(1, &priv_->ubo_scene);
        priv_->ubo_scene = 0;
    }
    if (priv_->ubo_model) {
        glDeleteBuffers(1, &priv_->ubo_model);
        priv_->ubo_model = 0;
    }
}

void SceneGL41Pipeline::draw()
{
    auto mat4_to_float16 = [](const LibMatrix::mat4& m, float out[16]) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                out[c * 4 + r] = m[r][c];
    };

    // Preserve incoming GL state to avoid leaking changes across scenes.
    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    GLint depth_func = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    const float aspect = static_cast<float>(canvas_.width()) / static_cast<float>(canvas_.height());
    LibMatrix::mat4 proj;
    proj.setIdentity();
    proj *= LibMatrix::Mat4::perspective(60.0f, aspect, 0.1f, 100.0f);

    LibMatrix::Stack4 view;
    const float t = static_cast<float>(realTime_.elapsed());
    view.translate(0.0f, 0.0f, -18.0f);
    view.rotate(t * 15.0f, 0.0f, 1.0f, 0.0f);
    view.rotate(t * 10.0f, 1.0f, 0.0f, 0.0f);

    LibMatrix::mat4 vp(proj);
    vp *= view.getCurrent();

    float scene_block[24] = {};
    mat4_to_float16(vp, scene_block);
    // light dir (w unused)
    scene_block[16] = 0.4f;
    scene_block[17] = 0.8f;
    scene_block[18] = 0.2f;
    scene_block[19] = 0.0f;
    // time (pad to vec4)
    scene_block[20] = t;
    scene_block[21] = 0.0f;
    scene_block[22] = 0.0f;
    scene_block[23] = 0.0f;

    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo_scene);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(scene_block), scene_block);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    priv_->procs.BindProgramPipeline(priv_->pipeline);
    priv_->procs.BindVertexArray(priv_->vao);

    // Single instanced draw for the whole grid.
    priv_->procs.DrawArraysInstanced(GL_TRIANGLES, 0, 36, static_cast<GLsizei>(priv_->instance_count));

    priv_->procs.BindVertexArray(0);
    priv_->procs.BindProgramPipeline(0);

    if (depth_was_enabled)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    glDepthFunc(depth_func);
}

struct SceneGL41MRTPrivate
{
    GL41Procs procs;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint fbo = 0;
    GLuint rbo_depth = 0;
    std::vector<GLuint> color_tex;
    GLint u_time = -1;
    unsigned targets = 4;
    unsigned size = 1024;
};

SceneGL41MRT::SceneGL41MRT(Canvas &pCanvas)
    : Scene(pCanvas, "gl41-mrt"), priv_(new SceneGL41MRTPrivate)
{
    options_["targets"] = Scene::Option("targets", "4", "Number of MRT color attachments (1..4)");
    options_["size"] = Scene::Option("size", "1024", "Offscreen MRT render size (pixels)");
}

SceneGL41MRT::~SceneGL41MRT()
{
    unload();
    delete priv_;
}

bool SceneGL41MRT::supported(bool show_errors)
{
    if (Options::macos_gl_profile == Options::MacOSGLProfileLegacy) {
        if (show_errors)
            Log::info("gl41-mrt: disabled in legacy profile\n");
        return false;
    }
    if (!GLExtensions::is_core_profile()) {
        if (show_errors)
            Log::info("gl41-mrt: requires a core profile context\n");
        return false;
    }

    GL41Procs p;
    if (!p.load(show_errors))
        return false;

    const bool have_blend_i = (p.Enablei && p.Disablei && p.BlendFunci && p.BlendEquationi);
    const bool have_blit = (p.BlitFramebuffer != nullptr);
    if (!have_blend_i || !have_blit) {
        if (show_errors)
            Log::info("gl41-mrt: missing per-target blend or blit entry points\n");
        return false;
    }
    return true;
}

static bool gl41_mrt_recreate_fbo(SceneGL41MRTPrivate* priv, bool show_errors)
{
    if (priv->fbo) {
        GLExtensions::DeleteFramebuffers(1, &priv->fbo);
        priv->fbo = 0;
    }
    if (priv->rbo_depth) {
        GLExtensions::DeleteRenderbuffers(1, &priv->rbo_depth);
        priv->rbo_depth = 0;
    }
    if (!priv->color_tex.empty()) {
        glDeleteTextures(static_cast<GLsizei>(priv->color_tex.size()), priv->color_tex.data());
        priv->color_tex.clear();
    }

    priv->color_tex.resize(priv->targets, 0);
    glGenTextures(static_cast<GLsizei>(priv->targets), priv->color_tex.data());
    for (unsigned i = 0; i < priv->targets; ++i) {
        glBindTexture(GL_TEXTURE_2D, priv->color_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Use RGBA8 for broad compatibility and predictable bandwidth.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(priv->size), static_cast<GLsizei>(priv->size), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    GLExtensions::GenRenderbuffers(1, &priv->rbo_depth);
    GLExtensions::BindRenderbuffer(GL_RENDERBUFFER, priv->rbo_depth);
    GLExtensions::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(priv->size), static_cast<GLsizei>(priv->size));
    GLExtensions::BindRenderbuffer(GL_RENDERBUFFER, 0);

    GLExtensions::GenFramebuffers(1, &priv->fbo);
    GLExtensions::BindFramebuffer(GL_FRAMEBUFFER, priv->fbo);
    for (unsigned i = 0; i < priv->targets; ++i)
        GLExtensions::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, priv->color_tex[i], 0);
    GLExtensions::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, priv->rbo_depth);

    std::vector<GLenum> bufs;
    bufs.reserve(priv->targets);
    for (unsigned i = 0; i < priv->targets; ++i)
        bufs.push_back(GL_COLOR_ATTACHMENT0 + i);
    glDrawBuffers(static_cast<GLsizei>(bufs.size()), bufs.data());

    const GLenum status = GLExtensions::CheckFramebufferStatus(GL_FRAMEBUFFER);
    GLExtensions::BindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (show_errors)
            Log::error("gl41-mrt: FBO incomplete (0x%x)\n", status);
        return false;
    }
    return true;
}

bool SceneGL41MRT::load()
{
    if (!priv_->procs.load(true))
        return false;
    if (!priv_->procs.Enablei || !priv_->procs.Disablei || !priv_->procs.BlendFunci ||
        !priv_->procs.BlendEquationi || !priv_->procs.BlitFramebuffer) {
        Log::error("gl41-mrt: missing required GL 4.1 entry points\n");
        return false;
    }

    static const std::string vtx =
        "#version 150\n"
        "in vec2 position;\n"
        "out vec2 vUV;\n"
        "void main() {\n"
        "  vUV = position * 0.5 + 0.5;\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "}\n";

    static const std::string frg =
        "#version 150\n"
        "in vec2 vUV;\n"
        "uniform float uTime;\n"
        "out vec4 o0;\n"
        "out vec4 o1;\n"
        "out vec4 o2;\n"
        "out vec4 o3;\n"
        "void main() {\n"
        "  float w = 0.5 + 0.5 * sin(uTime + vUV.x * 10.0 + vUV.y * 7.0);\n"
        "  vec4 base = vec4(vUV, w, 0.6);\n"
        "  o0 = vec4(1.0, 0.2, 0.2, 0.20) + 0.80 * base;\n"
        "  o1 = vec4(0.2, 1.0, 0.2, 0.30) + 0.70 * base.zyxw;\n"
        "  o2 = vec4(0.2, 0.2, 1.0, 0.40) + 0.60 * base.yxzw;\n"
        "  o3 = vec4(1.0, 1.0, 0.2, 0.50) + 0.50 * base;\n"
        "}\n";

    std::string err;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vtx, err);
    if (!vs) {
        Log::error("gl41-mrt vertex shader: %s\n", err.c_str());
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frg, err);
    if (!fs) {
        Log::error("gl41-mrt fragment shader: %s\n", err.c_str());
        glDeleteShader(vs);
        return false;
    }

    priv_->program = glCreateProgram();
    glAttachShader(priv_->program, vs);
    glAttachShader(priv_->program, fs);
    auto cleanup_mrt_load = [this]() {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        priv_->procs.BindVertexArray(0);
        if (priv_->fbo) {
            GLExtensions::DeleteFramebuffers(1, &priv_->fbo);
            priv_->fbo = 0;
        }
        if (priv_->rbo_depth) {
            GLExtensions::DeleteRenderbuffers(1, &priv_->rbo_depth);
            priv_->rbo_depth = 0;
        }
        if (!priv_->color_tex.empty()) {
            glDeleteTextures(static_cast<GLsizei>(priv_->color_tex.size()), priv_->color_tex.data());
            priv_->color_tex.clear();
        }
        if (priv_->vbo) {
            glDeleteBuffers(1, &priv_->vbo);
            priv_->vbo = 0;
        }
        if (priv_->vao) {
            priv_->procs.DeleteVertexArrays(1, &priv_->vao);
            priv_->vao = 0;
        }
        if (priv_->program) {
            glDeleteProgram(priv_->program);
            priv_->program = 0;
        }
    };
    if (!priv_->procs.BindFragDataLocation) {
        Log::error("gl41-mrt: missing glBindFragDataLocation entry point\n");
        glDeleteShader(vs);
        glDeleteShader(fs);
        cleanup_mrt_load();
        return false;
    }
    priv_->procs.BindFragDataLocation(priv_->program, 0, "o0");
    priv_->procs.BindFragDataLocation(priv_->program, 1, "o1");
    priv_->procs.BindFragDataLocation(priv_->program, 2, "o2");
    priv_->procs.BindFragDataLocation(priv_->program, 3, "o3");
    glLinkProgram(priv_->program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(priv_->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        const std::string log = get_program_info_log(priv_->program);
        Log::error("gl41-mrt link: %s\n", log.empty() ? "<no log>" : log.c_str());
        cleanup_mrt_load();
        return false;
    }

    priv_->u_time = glGetUniformLocation(priv_->program, "uTime");

    priv_->procs.GenVertexArrays(1, &priv_->vao);
    priv_->procs.BindVertexArray(priv_->vao);

    const float tri[] = {
        -1.0f, -1.0f,
         3.0f, -1.0f,
        -1.0f,  3.0f,
    };

    glGenBuffers(1, &priv_->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);

    const GLint pos_loc = glGetAttribLocation(priv_->program, "position");
    if (pos_loc < 0) {
        Log::error("gl41-mrt: missing vertex attribute 'position'\n");
        cleanup_mrt_load();
        return false;
    }
    glEnableVertexAttribArray(static_cast<GLuint>(pos_loc));
    glVertexAttribPointer(static_cast<GLuint>(pos_loc), 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);

    if (!gl41_mrt_recreate_fbo(priv_, true)) {
        cleanup_mrt_load();
        return false;
    }

    return true;
}

bool SceneGL41MRT::setup()
{
    unsigned targets = 4;
    unsigned size = 1024;
    try {
        targets = static_cast<unsigned>(std::stoul(options_["targets"].value));
    } catch (...) {
        targets = 4;
    }
    try {
        size = static_cast<unsigned>(std::stoul(options_["size"].value));
    } catch (...) {
        size = 1024;
    }

    if (targets < 1)
        targets = 1;
    if (targets > 4)
        targets = 4;
    if (size < 64)
        size = 64;
    if (size > 4096)
        size = 4096;

    const bool changed = (targets != priv_->targets) || (size != priv_->size);
    priv_->targets = targets;
    priv_->size = size;

    if (changed)
        return gl41_mrt_recreate_fbo(priv_, true);
    return true;
}

void SceneGL41MRT::unload()
{
    if (!priv_)
        return;

    if (priv_->fbo) {
        GLExtensions::DeleteFramebuffers(1, &priv_->fbo);
        priv_->fbo = 0;
    }
    if (priv_->rbo_depth) {
        GLExtensions::DeleteRenderbuffers(1, &priv_->rbo_depth);
        priv_->rbo_depth = 0;
    }
    if (!priv_->color_tex.empty()) {
        glDeleteTextures(static_cast<GLsizei>(priv_->color_tex.size()), priv_->color_tex.data());
        priv_->color_tex.clear();
    }
    if (priv_->vao) {
        priv_->procs.DeleteVertexArrays(1, &priv_->vao);
        priv_->vao = 0;
    }
    if (priv_->vbo) {
        glDeleteBuffers(1, &priv_->vbo);
        priv_->vbo = 0;
    }
    if (priv_->program) {
        glDeleteProgram(priv_->program);
        priv_->program = 0;
    }
}

void SceneGL41MRT::draw()
{
    if (!priv_->procs.Enablei || !priv_->procs.Disablei || !priv_->procs.BlendFunci ||
        !priv_->procs.BlendEquationi || !priv_->procs.BlitFramebuffer) {
        return;
    }

    GLint prev_read_fbo = 0;
    GLint prev_draw_fbo = 0;
    GLint prev_read_buffer = GL_BACK;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
    glGetIntegerv(GL_READ_BUFFER, &prev_read_buffer);

    GLint prev_viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    GLint depth_func = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    const float t = static_cast<float>(realTime_.elapsed());
    const GLsizei w = static_cast<GLsizei>(priv_->size);
    const GLsizei h = static_cast<GLsizei>(priv_->size);

    GLExtensions::BindFramebuffer(GL_FRAMEBUFFER, priv_->fbo);
    glViewport(0, 0, w, h);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Per-target blending (different state per render target).
    for (unsigned i = 0; i < priv_->targets; ++i) {
        priv_->procs.Enablei(GL_BLEND, i);
        priv_->procs.BlendEquationi(i, GL_FUNC_ADD);
        switch (i % 4) {
        case 0:
            priv_->procs.BlendFunci(i, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case 1:
            priv_->procs.BlendFunci(i, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case 2:
            priv_->procs.BlendFunci(i, GL_SRC_ALPHA, GL_ONE);
            break;
        default:
            priv_->procs.BlendFunci(i, GL_ONE, GL_ONE);
            break;
        }
    }

    glUseProgram(priv_->program);
    if (priv_->u_time >= 0)
        glUniform1f(priv_->u_time, t);
    priv_->procs.BindVertexArray(priv_->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    priv_->procs.BindVertexArray(0);
    glUseProgram(0);

    for (unsigned i = 0; i < priv_->targets; ++i)
        priv_->procs.Disablei(GL_BLEND, i);

    // Blit attachment 0 to the default framebuffer for a visible result.
    GLExtensions::BindFramebuffer(GL_READ_FRAMEBUFFER, priv_->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLExtensions::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    priv_->procs.BlitFramebuffer(0, 0, w, h,
                                 0, 0, static_cast<GLint>(canvas_.width()), static_cast<GLint>(canvas_.height()),
                                 GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glReadBuffer(prev_read_buffer);
    GLExtensions::BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
    GLExtensions::BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

    if (depth_was_enabled)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    glDepthFunc(depth_func);
}

struct SceneGL41TexArrayPrivate
{
    GL41Procs procs;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo_pos = 0;
    GLuint vbo_instance = 0;
    GLuint ubo = 0;
    GLuint tex = 0;
    GLint u_bias = -1;
    unsigned instances = 4096;
    unsigned layers = 64;
    unsigned tex_size = 64;
};

SceneGL41TexArray::SceneGL41TexArray(Canvas &pCanvas)
    : Scene(pCanvas, "gl41-texarray"), priv_(new SceneGL41TexArrayPrivate)
{
    options_["instances"] = Scene::Option("instances", "4096", "Number of instances");
    options_["layers"] = Scene::Option("layers", "64", "Texture array layers (1..256)");
    options_["tex-size"] = Scene::Option("tex-size", "64", "Per-layer texture size (pixels)");
    options_["bias"] = Scene::Option("bias", "0.75", "Texture sampling LOD bias");
}

SceneGL41TexArray::~SceneGL41TexArray()
{
    unload();
    delete priv_;
}

bool SceneGL41TexArray::supported(bool show_errors)
{
    if (Options::macos_gl_profile == Options::MacOSGLProfileLegacy) {
        if (show_errors)
            Log::info("gl41-texarray: disabled in legacy profile\n");
        return false;
    }
    if (!GLExtensions::is_core_profile()) {
        if (show_errors)
            Log::info("gl41-texarray: requires a core profile context\n");
        return false;
    }

    GL41Procs p;
    if (!p.load(show_errors))
        return false;
    if (!GLExtensions::GenerateMipmap) {
        if (show_errors)
            Log::info("gl41-texarray: missing glGenerateMipmap entry point\n");
        return false;
    }
    return true;
}

static void gl41_texarray_upload(SceneGL41TexArrayPrivate* priv)
{
    const unsigned w = priv->tex_size;
    const unsigned h = priv->tex_size;
    const unsigned layers = priv->layers;

    glBindTexture(GL_TEXTURE_2D_ARRAY, priv->tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                 static_cast<GLsizei>(w), static_cast<GLsizei>(h), static_cast<GLsizei>(layers),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    std::vector<unsigned char> rgba;
    rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (unsigned layer = 0; layer < layers; ++layer) {
        const unsigned char lr = static_cast<unsigned char>((layer * 7) & 0xFF);
        const unsigned char lg = static_cast<unsigned char>((layer * 13) & 0xFF);
        const unsigned char lb = static_cast<unsigned char>((layer * 23) & 0xFF);
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const bool checker = (((x >> 3) ^ (y >> 3)) & 1) != 0;
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                rgba[idx + 0] = static_cast<unsigned char>(checker ? lr : (255 - lr));
                rgba[idx + 1] = static_cast<unsigned char>(checker ? lg : (255 - lg));
                rgba[idx + 2] = static_cast<unsigned char>(checker ? lb : (255 - lb));
                rgba[idx + 3] = 255;
            }
        }

        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                        0, 0, static_cast<GLint>(layer),
                        static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    GLExtensions::GenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

bool SceneGL41TexArray::load()
{
    if (!priv_->procs.load(true))
        return false;
    if (!GLExtensions::GenerateMipmap) {
        Log::error("gl41-texarray: missing glGenerateMipmap entry point\n");
        return false;
    }

    static const std::string vtx =
        "#version 150\n"
        "in vec2 position;\n"
        "in vec3 instanceData;\n"
        "out vec2 vUV;\n"
        "flat out float vLayer;\n"
        "layout(std140) uniform Matrices { mat4 uMVP; };\n"
        "void main() {\n"
        "  vUV = position * 0.5 + 0.5;\n"
        "  vLayer = instanceData.z;\n"
        "  vec2 pos = position * 0.04 + instanceData.xy;\n"
        "  gl_Position = uMVP * vec4(pos, 0.0, 1.0);\n"
        "}\n";

    static const std::string frg =
        "#version 150\n"
        "in vec2 vUV;\n"
        "flat in float vLayer;\n"
        "uniform sampler2DArray uTex;\n"
        "uniform float uBias;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  vec4 texel = texture(uTex, vec3(vUV, vLayer), uBias);\n"
        "  fragColor = texel;\n"
        "}\n";

    std::string err;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vtx, err);
    if (!vs) {
        Log::error("gl41-texarray vertex shader: %s\n", err.c_str());
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frg, err);
    if (!fs) {
        Log::error("gl41-texarray fragment shader: %s\n", err.c_str());
        glDeleteShader(vs);
        return false;
    }

    priv_->program = link_program(vs, fs, err);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!priv_->program) {
        Log::error("gl41-texarray link: %s\n", err.c_str());
        return false;
    }

    auto cleanup_texarray_load = [this]() {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        priv_->procs.BindVertexArray(0);
        if (priv_->tex) {
            glDeleteTextures(1, &priv_->tex);
            priv_->tex = 0;
        }
        if (priv_->ubo) {
            glDeleteBuffers(1, &priv_->ubo);
            priv_->ubo = 0;
        }
        if (priv_->vbo_instance) {
            glDeleteBuffers(1, &priv_->vbo_instance);
            priv_->vbo_instance = 0;
        }
        if (priv_->vbo_pos) {
            glDeleteBuffers(1, &priv_->vbo_pos);
            priv_->vbo_pos = 0;
        }
        if (priv_->vao) {
            priv_->procs.DeleteVertexArrays(1, &priv_->vao);
            priv_->vao = 0;
        }
        if (priv_->program) {
            glDeleteProgram(priv_->program);
            priv_->program = 0;
        }
    };

    glUseProgram(priv_->program);
    const GLint loc_tex = glGetUniformLocation(priv_->program, "uTex");
    priv_->u_bias = glGetUniformLocation(priv_->program, "uBias");
    if (loc_tex >= 0)
        glUniform1i(loc_tex, 0);
    glUseProgram(0);

    priv_->procs.GenVertexArrays(1, &priv_->vao);
    priv_->procs.BindVertexArray(priv_->vao);

    const float quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
    };

    glGenBuffers(1, &priv_->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    const GLint pos_loc = glGetAttribLocation(priv_->program, "position");
    if (pos_loc < 0) {
        Log::error("gl41-texarray: missing vertex attribute 'position'\n");
        cleanup_texarray_load();
        return false;
    }
    glEnableVertexAttribArray(static_cast<GLuint>(pos_loc));
    glVertexAttribPointer(static_cast<GLuint>(pos_loc), 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));

    glGenBuffers(1, &priv_->vbo_instance);
    const GLint inst_loc = glGetAttribLocation(priv_->program, "instanceData");
    if (inst_loc < 0) {
        Log::error("gl41-texarray: missing vertex attribute 'instanceData'\n");
        cleanup_texarray_load();
        return false;
    }
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo_instance);
    glEnableVertexAttribArray(static_cast<GLuint>(inst_loc));
    glVertexAttribPointer(static_cast<GLuint>(inst_loc), 3, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));
    priv_->procs.VertexAttribDivisor(static_cast<GLuint>(inst_loc), 1);

    glGenBuffers(1, &priv_->ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(float) * 16, nullptr, GL_DYNAMIC_DRAW);

    const GLuint block = priv_->procs.GetUniformBlockIndex(priv_->program, "Matrices");
    if (block == GL_INVALID_INDEX) {
        Log::error("gl41-texarray: missing uniform block 'Matrices'\n");
        cleanup_texarray_load();
        return false;
    }
    priv_->procs.UniformBlockBinding(priv_->program, block, 0);
    priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 0, priv_->ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);

    glGenTextures(1, &priv_->tex);
    gl41_texarray_upload(priv_);

    return true;
}

bool SceneGL41TexArray::setup()
{
    unsigned instances = 4096;
    unsigned layers = 64;
    unsigned tex_size = 64;
    float bias = 0.75f;

    try {
        instances = static_cast<unsigned>(std::stoul(options_["instances"].value));
    } catch (...) {
        instances = 4096;
    }
    try {
        layers = static_cast<unsigned>(std::stoul(options_["layers"].value));
    } catch (...) {
        layers = 64;
    }
    try {
        tex_size = static_cast<unsigned>(std::stoul(options_["tex-size"].value));
    } catch (...) {
        tex_size = 64;
    }
    try {
        bias = std::stof(options_["bias"].value);
    } catch (...) {
        bias = 0.75f;
    }

    if (instances < 1)
        instances = 1;
    if (instances > 16384)
        instances = 16384;
    if (layers < 1)
        layers = 1;
    if (layers > 256)
        layers = 256;
    if (tex_size < 8)
        tex_size = 8;
    if (tex_size > 512)
        tex_size = 512;
    if (bias < -4.0f)
        bias = -4.0f;
    if (bias > 4.0f)
        bias = 4.0f;

    const bool tex_changed = (layers != priv_->layers) || (tex_size != priv_->tex_size);
    priv_->instances = instances;
    priv_->layers = layers;
    priv_->tex_size = tex_size;

    // Instance data: x, y, layer
    std::vector<float> inst;
    inst.reserve(static_cast<size_t>(instances) * 3);
    const unsigned side = static_cast<unsigned>(std::ceil(std::sqrt(static_cast<double>(instances))));
    const float span = 1.8f;
    const float step = (side > 1) ? (span / (side - 1)) : 0.0f;
    for (unsigned i = 0; i < instances; ++i) {
        const unsigned x = i % side;
        const unsigned y = i / side;
        inst.push_back(-0.9f + step * x);
        inst.push_back(-0.9f + step * y);
        inst.push_back(static_cast<float>(i % layers));
    }

    priv_->procs.BindVertexArray(priv_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo_instance);
    glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(float), inst.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);

    if (tex_changed)
        gl41_texarray_upload(priv_);

    glUseProgram(priv_->program);
    if (priv_->u_bias >= 0)
        glUniform1f(priv_->u_bias, bias);
    glUseProgram(0);
    return true;
}

void SceneGL41TexArray::unload()
{
    if (!priv_)
        return;

    if (priv_->procs.BindBufferBase)
        priv_->procs.BindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
    if (priv_->vao) {
        priv_->procs.DeleteVertexArrays(1, &priv_->vao);
        priv_->vao = 0;
    }
    if (priv_->vbo_pos) {
        glDeleteBuffers(1, &priv_->vbo_pos);
        priv_->vbo_pos = 0;
    }
    if (priv_->vbo_instance) {
        glDeleteBuffers(1, &priv_->vbo_instance);
        priv_->vbo_instance = 0;
    }
    if (priv_->ubo) {
        glDeleteBuffers(1, &priv_->ubo);
        priv_->ubo = 0;
    }
    if (priv_->tex) {
        glDeleteTextures(1, &priv_->tex);
        priv_->tex = 0;
    }
    if (priv_->program) {
        glDeleteProgram(priv_->program);
        priv_->program = 0;
    }
}

void SceneGL41TexArray::draw()
{
    const float t = static_cast<float>(currentFrame_) * 0.01f;
    const float c = std::cos(t);
    const float s = std::sin(t);
    const float mvp[16] = {
        c,  s, 0, 0,
       -s,  c, 0, 0,
        0,  0, 1, 0,
        0,  0, 0, 1,
    };

    GLint prev_active_texture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_texture);

    glUseProgram(priv_->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, priv_->tex);

    glBindBuffer(GL_UNIFORM_BUFFER, priv_->ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(mvp), mvp);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    priv_->procs.BindVertexArray(priv_->vao);
    priv_->procs.DrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(priv_->instances));
    priv_->procs.BindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glUseProgram(0);

    glActiveTexture(static_cast<GLenum>(prev_active_texture));
}

struct SceneGL41StreamingSyncPrivate
{
    GL41Procs procs;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo[3] = {0, 0, 0};
    GLsync fence[3] = {nullptr, nullptr, nullptr};
    unsigned buffer_index = 0;
    size_t bytes = 4u * 1024u * 1024u;
    GLsizei vertex_count = 0;
    std::vector<float> base_xy;
    GLint attr_position = -1;
    GLint u_mvp = -1;
    bool warned_map_failure = false;
    bool warned_sync_timeout = false;
    bool warned_alignment = false;
    bool warned_unmap_failure = false;
};

SceneGL41StreamingSync::SceneGL41StreamingSync(Canvas &pCanvas)
    : Scene(pCanvas, "gl41-streaming-sync"), priv_(new SceneGL41StreamingSyncPrivate)
{
    options_["bytes"] = Scene::Option("bytes", "4194304", "Streaming buffer size in bytes");
}

SceneGL41StreamingSync::~SceneGL41StreamingSync()
{
    unload();
    delete priv_;
}

bool SceneGL41StreamingSync::supported(bool show_errors)
{
    if (Options::macos_gl_profile == Options::MacOSGLProfileLegacy) {
        if (show_errors)
            Log::info("gl41-streaming-sync: disabled in legacy profile\n");
        return false;
    }
    if (!GLExtensions::is_core_profile()) {
        if (show_errors)
            Log::info("gl41-streaming-sync: requires a core profile context\n");
        return false;
    }

    GL41Procs p;
    if (!p.load(show_errors))
        return false;

    const bool have_map = (p.MapBufferRange && p.UnmapBuffer);
    const bool have_sync = (p.FenceSync && p.ClientWaitSync && p.DeleteSync);
    if (!have_map || !have_sync) {
        if (show_errors)
            Log::info("gl41-streaming-sync: missing MapBufferRange and/or sync object entry points\n");
        return false;
    }
    return true;
}

static void gl41_streaming_realloc(SceneGL41StreamingSyncPrivate* priv)
{
    for (int i = 0; i < 3; ++i) {
        if (priv->fence[i]) {
            priv->procs.DeleteSync(priv->fence[i]);
            priv->fence[i] = nullptr;
        }
        if (priv->vbo[i]) {
            glDeleteBuffers(1, &priv->vbo[i]);
            priv->vbo[i] = 0;
        }
    }

    glGenBuffers(3, priv->vbo);
    for (int i = 0; i < 3; ++i) {
        glBindBuffer(GL_ARRAY_BUFFER, priv->vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(priv->bytes), nullptr, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // vec3 position per vertex, rounded down to a multiple of 3 vertices.
    const size_t floats = priv->bytes / sizeof(float);
    priv->vertex_count = static_cast<GLsizei>((floats / 3) - ((floats / 3) % 3));

    const size_t tris = static_cast<size_t>(priv->vertex_count) / 3;
    const unsigned side = static_cast<unsigned>(std::sqrt(static_cast<double>(tris))) + 1;
    priv->base_xy.resize(tris * 2);
    for (size_t i = 0; i < tris; ++i) {
        const unsigned x = static_cast<unsigned>(i % side);
        const unsigned y = static_cast<unsigned>(i / side);
        const float fx = (side > 1) ? (-1.2f + 2.4f * (static_cast<float>(x) / (side - 1))) : 0.0f;
        const float fy = (side > 1) ? (-1.2f + 2.4f * (static_cast<float>(y) / (side - 1))) : 0.0f;
        priv->base_xy[i * 2 + 0] = fx;
        priv->base_xy[i * 2 + 1] = fy;
    }
}

bool SceneGL41StreamingSync::load()
{
    if (!priv_->procs.load(true))
        return false;

    if (!priv_->procs.MapBufferRange || !priv_->procs.UnmapBuffer || !priv_->procs.FenceSync || !priv_->procs.ClientWaitSync || !priv_->procs.DeleteSync)
        return false;

    static const std::string vtx =
        "#version 150\n"
        "in vec3 position;\n"
        "uniform mat4 uMVP;\n"
        "out vec3 vP;\n"
        "void main() {\n"
        "  vP = position;\n"
        "  gl_Position = uMVP * vec4(position, 1.0);\n"
        "}\n";

    static const std::string frg =
        "#version 150\n"
        "in vec3 vP;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  vec3 c = 0.5 + 0.5 * normalize(vP + vec3(0.2, 0.1, 0.3));\n"
        "  fragColor = vec4(c, 1.0);\n"
        "}\n";

    std::string err;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vtx, err);
    if (!vs) {
        Log::error("gl41-streaming-sync vertex shader: %s\n", err.c_str());
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frg, err);
    if (!fs) {
        Log::error("gl41-streaming-sync fragment shader: %s\n", err.c_str());
        glDeleteShader(vs);
        return false;
    }

    priv_->program = link_program(vs, fs, err);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!priv_->program) {
        Log::error("gl41-streaming-sync link: %s\n", err.c_str());
        return false;
    }

    priv_->u_mvp = glGetUniformLocation(priv_->program, "uMVP");

    priv_->procs.GenVertexArrays(1, &priv_->vao);
    priv_->procs.BindVertexArray(priv_->vao);

    gl41_streaming_realloc(priv_);

    auto cleanup_streaming_load = [this]() {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        priv_->procs.BindVertexArray(0);
        for (int i = 0; i < 3; ++i) {
            if (priv_->vbo[i]) {
                glDeleteBuffers(1, &priv_->vbo[i]);
                priv_->vbo[i] = 0;
            }
        }
        if (priv_->vao) {
            priv_->procs.DeleteVertexArrays(1, &priv_->vao);
            priv_->vao = 0;
        }
        if (priv_->program) {
            glDeleteProgram(priv_->program);
            priv_->program = 0;
        }
    };

    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo[0]);
    priv_->attr_position = glGetAttribLocation(priv_->program, "position");
    if (priv_->attr_position < 0) {
        Log::error("gl41-streaming-sync: missing vertex attribute 'position'\n");
        cleanup_streaming_load();
        return false;
    }
    glEnableVertexAttribArray(static_cast<GLuint>(priv_->attr_position));
    glVertexAttribPointer(static_cast<GLuint>(priv_->attr_position), 3, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    priv_->procs.BindVertexArray(0);
    return true;
}

bool SceneGL41StreamingSync::setup()
{
    size_t bytes = 4u * 1024u * 1024u;
    try {
        bytes = static_cast<size_t>(std::stoull(options_["bytes"].value));
    } catch (...) {
        bytes = 4u * 1024u * 1024u;
    }
    if (bytes < 256 * 1024)
        bytes = 256 * 1024;
    if (bytes > 64u * 1024u * 1024u)
        bytes = 64u * 1024u * 1024u;
    // Keep alignment friendly.
    const size_t requested_bytes = bytes;
    bytes &= ~static_cast<size_t>(kStreamingBufferAlignment - 1u);
    if (bytes != requested_bytes && !priv_->warned_alignment) {
        Log::info("gl41-streaming-sync: bytes aligned down from %zu to %zu\n",
                  requested_bytes, bytes);
        priv_->warned_alignment = true;
    }

    const bool changed = (bytes != priv_->bytes);
    priv_->bytes = bytes;
    if (changed)
        gl41_streaming_realloc(priv_);
    return true;
}

void SceneGL41StreamingSync::unload()
{
    if (!priv_)
        return;

    for (int i = 0; i < 3; ++i) {
        if (priv_->fence[i]) {
            priv_->procs.DeleteSync(priv_->fence[i]);
            priv_->fence[i] = nullptr;
        }
        if (priv_->vbo[i]) {
            glDeleteBuffers(1, &priv_->vbo[i]);
            priv_->vbo[i] = 0;
        }
    }
    if (priv_->vao) {
        priv_->procs.DeleteVertexArrays(1, &priv_->vao);
        priv_->vao = 0;
    }
    if (priv_->program) {
        glDeleteProgram(priv_->program);
        priv_->program = 0;
    }
}

void SceneGL41StreamingSync::draw()
{
    const float t = static_cast<float>(realTime_.elapsed());
    const unsigned idx = priv_->buffer_index % 3;
    priv_->buffer_index++;

    if (priv_->fence[idx]) {
        // Fast-path wait to keep benchmark responsiveness; short fallback for correctness.
        const GLenum wait_res = priv_->procs.ClientWaitSync(priv_->fence[idx], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000ull);
        if (wait_res == GL_TIMEOUT_EXPIRED || wait_res == GL_WAIT_FAILED) {
            if (!priv_->warned_sync_timeout) {
                Log::info("gl41-streaming-sync: ClientWaitSync returned %s\n",
                          wait_res == GL_TIMEOUT_EXPIRED ? "GL_TIMEOUT_EXPIRED" : "GL_WAIT_FAILED");
                priv_->warned_sync_timeout = true;
            }
            const GLenum wait_res2 = priv_->procs.ClientWaitSync(priv_->fence[idx], GL_SYNC_FLUSH_COMMANDS_BIT, 5000000ull);
            if (wait_res2 == GL_TIMEOUT_EXPIRED || wait_res2 == GL_WAIT_FAILED) {
                Log::info("gl41-streaming-sync: sync wait exceeded, forcing glFinish\n");
                glFinish();
            }
        }
        priv_->procs.DeleteSync(priv_->fence[idx]);
        priv_->fence[idx] = nullptr;
    }

    priv_->procs.BindVertexArray(priv_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo[idx]);
    // Rebind attribute pointer to the current streaming buffer.
    glVertexAttribPointer(static_cast<GLuint>(priv_->attr_position), 3, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));

    // Use GL_MAP_INVALIDATE_BUFFER_BIT to discard old contents. We intentionally do NOT use
    // GL_MAP_UNSYNCHRONIZED_BIT here; instead we rely on explicit fence sync objects to ensure
    // the GPU is done with this buffer before mapping. This is a deliberate safety/perf trade-off.
    void* ptr = priv_->procs.MapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(priv_->bytes),
                                           GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    const size_t verts = static_cast<size_t>(priv_->vertex_count);
    const size_t tris = verts / 3;
    const float jitter = 0.02f * std::sin(t * 1.3f);

    const float* base_xy = priv_->base_xy.data();
    auto fill_vertices = [base_xy, tris, t, jitter](float* dst) {
        for (size_t i = 0; i < tris; ++i) {
            const float fx = base_xy[i * 2 + 0];
            const float fy = base_xy[i * 2 + 1];
            const float z = 0.4f * std::sin(t + fx * 2.1f) + 0.4f * std::cos(t * 0.7f + fy * 2.3f);
            const float s = 0.03f;

            const size_t tri_base = i * 3;
            dst[(tri_base + 0) * 3 + 0] = fx - s + jitter;
            dst[(tri_base + 0) * 3 + 1] = fy - s + jitter;
            dst[(tri_base + 0) * 3 + 2] = z;
            dst[(tri_base + 1) * 3 + 0] = fx + s - jitter;
            dst[(tri_base + 1) * 3 + 1] = fy - s + jitter;
            dst[(tri_base + 1) * 3 + 2] = z + 0.02f;
            dst[(tri_base + 2) * 3 + 0] = fx + jitter;
            dst[(tri_base + 2) * 3 + 1] = fy + s - jitter;
            dst[(tri_base + 2) * 3 + 2] = z + 0.04f;
        }
    };

    if (ptr) {
        fill_vertices(reinterpret_cast<float*>(ptr));
        if (!priv_->procs.UnmapBuffer(GL_ARRAY_BUFFER) && !priv_->warned_unmap_failure) {
            Log::info("gl41-streaming-sync: UnmapBuffer reported corruption\n");
            priv_->warned_unmap_failure = true;
        }
    } else {
        if (!priv_->warned_map_failure) {
            Log::info("gl41-streaming-sync: MapBufferRange failed, falling back to glBufferSubData\n");
            priv_->warned_map_failure = true;
        }
        // Orphan the buffer to avoid pipeline stalls on update.
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(priv_->bytes), nullptr, GL_STREAM_DRAW);
        std::vector<float> temp;
        temp.resize(verts * 3);
        fill_vertices(temp.data());
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(temp.size() * sizeof(float)), temp.data());
    }

    // Capture current framebuffer state before modifying it.
    GLint prev_read_fbo = 0;
    GLint prev_draw_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean stencil_was_enabled = glIsEnabled(GL_STENCIL_TEST);
    GLboolean color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);

    GLExtensions::BindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(canvas_.width()), static_cast<GLsizei>(canvas_.height()));
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(priv_->program);
    if (priv_->u_mvp >= 0) {
        const float aspect = static_cast<float>(canvas_.width()) / static_cast<float>(canvas_.height());
        LibMatrix::mat4 proj;
        proj.setIdentity();
        proj *= LibMatrix::Mat4::perspective(60.0f, aspect, 0.1f, 10.0f);

        LibMatrix::Stack4 view;
        view.translate(0.0f, 0.0f, -3.2f);
        view.rotate(t * 20.0f, 0.0f, 1.0f, 0.0f);
        view.rotate(t * 13.0f, 1.0f, 0.0f, 0.0f);

        LibMatrix::mat4 mvp(proj);
        mvp *= view.getCurrent();

        float mvp_f[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                mvp_f[c * 4 + r] = mvp[r][c];
        glUniformMatrix4fv(priv_->u_mvp, 1, GL_FALSE, mvp_f);
    }
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, priv_->vertex_count);
    glUseProgram(0);

    if (depth_was_enabled)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (cull_was_enabled)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (blend_was_enabled)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (scissor_was_enabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    if (stencil_was_enabled)
        glEnable(GL_STENCIL_TEST);
    else
        glDisable(GL_STENCIL_TEST);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    GLExtensions::BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
    GLExtensions::BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));

    priv_->fence[idx] = priv_->procs.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);
}

struct SceneGL41GeometryPrivate
{
    GL41Procs procs;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLint u_size = -1;
    GLint u_time = -1;
    unsigned points = 65536;
};

SceneGL41Geometry::SceneGL41Geometry(Canvas &pCanvas)
    : Scene(pCanvas, "gl41-geometry"), priv_(new SceneGL41GeometryPrivate)
{
    options_["points"] = Scene::Option("points", "65536", "Number of input points");
    options_["size"] = Scene::Option("size", "0.012", "Quad half-size in clip space");
}

SceneGL41Geometry::~SceneGL41Geometry()
{
    unload();
    delete priv_;
}

bool SceneGL41Geometry::supported(bool show_errors)
{
    if (Options::macos_gl_profile == Options::MacOSGLProfileLegacy) {
        if (show_errors)
            Log::info("gl41-geometry: disabled in legacy profile\n");
        return false;
    }
    if (!GLExtensions::is_core_profile()) {
        if (show_errors)
            Log::info("gl41-geometry: requires a core profile context\n");
        return false;
    }

    GL41Procs p;
    return p.load(show_errors);
}

bool SceneGL41Geometry::load()
{
    if (!priv_->procs.load(true))
        return false;

    static const std::string vtx =
        "#version 150\n"
        "in vec2 position;\n"
        "out float vId;\n"
        "void main() {\n"
        "  vId = position.x * 0.5 + position.y * 0.5;\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "}\n";

    static const std::string geo =
        "#version 150\n"
        "layout(points) in;\n"
        "layout(triangle_strip, max_vertices=4) out;\n"
        "uniform vec2 uSize;\n"
        "uniform float uTime;\n"
        "out vec2 gUV;\n"
        "void emit(vec2 off, vec2 uv) {\n"
        "  gl_Position = gl_in[0].gl_Position + vec4(off, 0.0, 0.0);\n"
        "  gUV = uv;\n"
        "  EmitVertex();\n"
        "}\n"
        "void main() {\n"
        "  vec2 s = uSize * (0.6 + 0.4 * abs(sin(uTime)));\n"
        "  emit(vec2(-s.x, -s.y), vec2(0.0, 0.0));\n"
        "  emit(vec2( s.x, -s.y), vec2(1.0, 0.0));\n"
        "  emit(vec2(-s.x,  s.y), vec2(0.0, 1.0));\n"
        "  emit(vec2( s.x,  s.y), vec2(1.0, 1.0));\n"
        "  EndPrimitive();\n"
        "}\n";

    static const std::string frg =
        "#version 150\n"
        "in vec2 gUV;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  float c = smoothstep(0.0, 0.15, min(min(gUV.x, 1.0-gUV.x), min(gUV.y, 1.0-gUV.y)));\n"
        "  fragColor = vec4(0.1 + gUV.x, 0.2 + gUV.y, c, 1.0);\n"
        "}\n";

    std::string err;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vtx, err);
    if (!vs) {
        Log::error("gl41-geometry vertex shader: %s\n", err.c_str());
        return false;
    }
    GLuint gs = compile_shader(GL_GEOMETRY_SHADER, geo, err);
    if (!gs) {
        Log::error("gl41-geometry geometry shader: %s\n", err.c_str());
        glDeleteShader(vs);
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frg, err);
    if (!fs) {
        Log::error("gl41-geometry fragment shader: %s\n", err.c_str());
        glDeleteShader(vs);
        glDeleteShader(gs);
        return false;
    }

    priv_->program = link_program(vs, gs, fs, err);
    glDeleteShader(vs);
    glDeleteShader(gs);
    glDeleteShader(fs);
    if (!priv_->program) {
        Log::error("gl41-geometry link: %s\n", err.c_str());
        return false;
    }

    priv_->u_size = glGetUniformLocation(priv_->program, "uSize");
    priv_->u_time = glGetUniformLocation(priv_->program, "uTime");

    priv_->procs.GenVertexArrays(1, &priv_->vao);
    priv_->procs.BindVertexArray(priv_->vao);

    glGenBuffers(1, &priv_->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(priv_->points) * 2 * static_cast<GLsizeiptr>(sizeof(float)), nullptr, GL_STATIC_DRAW);

    auto cleanup_geometry_load = [this]() {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        priv_->procs.BindVertexArray(0);
        if (priv_->vbo) {
            glDeleteBuffers(1, &priv_->vbo);
            priv_->vbo = 0;
        }
        if (priv_->vao) {
            priv_->procs.DeleteVertexArrays(1, &priv_->vao);
            priv_->vao = 0;
        }
        if (priv_->program) {
            glDeleteProgram(priv_->program);
            priv_->program = 0;
        }
    };

    const GLint pos_loc = glGetAttribLocation(priv_->program, "position");
    if (pos_loc < 0) {
        Log::error("gl41-geometry: missing vertex attribute 'position'\n");
        cleanup_geometry_load();
        return false;
    }
    glEnableVertexAttribArray(static_cast<GLuint>(pos_loc));
    glVertexAttribPointer(static_cast<GLuint>(pos_loc), 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<void*>(0));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    priv_->procs.BindVertexArray(0);
    return true;
}

bool SceneGL41Geometry::setup()
{
    unsigned points = 65536;
    float size = 0.012f;
    try {
        points = static_cast<unsigned>(std::stoul(options_["points"].value));
    } catch (...) {
        points = 65536;
    }
    try {
        size = std::stof(options_["size"].value);
    } catch (...) {
        size = 0.012f;
    }

    if (points < 1024)
        points = 1024;
    if (points > 262144)
        points = 262144;
    if (size < 0.001f)
        size = 0.001f;
    if (size > 0.05f)
        size = 0.05f;

    priv_->points = points;

    // Rebuild point buffer contents.
    std::vector<float> pts;
    pts.reserve(static_cast<size_t>(points) * 2);
    const unsigned side = static_cast<unsigned>(std::ceil(std::sqrt(static_cast<double>(points))));
    for (unsigned i = 0; i < points; ++i) {
        const unsigned x = i % side;
        const unsigned y = i / side;
        const float fx = (side > 1) ? (-1.0f + 2.0f * (static_cast<float>(x) / (side - 1))) : 0.0f;
        const float fy = (side > 1) ? (-1.0f + 2.0f * (static_cast<float>(y) / (side - 1))) : 0.0f;
        pts.push_back(fx);
        pts.push_back(fy);
    }

    glBindBuffer(GL_ARRAY_BUFFER, priv_->vbo);
    glBufferData(GL_ARRAY_BUFFER, pts.size() * sizeof(float), pts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glUseProgram(priv_->program);
    if (priv_->u_size >= 0)
        glUniform2f(priv_->u_size, size, size);
    glUseProgram(0);
    return true;
}

void SceneGL41Geometry::unload()
{
    if (!priv_)
        return;

    if (priv_->vao) {
        priv_->procs.DeleteVertexArrays(1, &priv_->vao);
        priv_->vao = 0;
    }
    if (priv_->vbo) {
        glDeleteBuffers(1, &priv_->vbo);
        priv_->vbo = 0;
    }
    if (priv_->program) {
        glDeleteProgram(priv_->program);
        priv_->program = 0;
    }
}

void SceneGL41Geometry::draw()
{
    const float t = static_cast<float>(realTime_.elapsed());
    glUseProgram(priv_->program);
    if (priv_->u_time >= 0)
        glUniform1f(priv_->u_time, t);
    priv_->procs.BindVertexArray(priv_->vao);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(priv_->points));
    priv_->procs.BindVertexArray(0);
    glUseProgram(0);
}

#endif // GLMARK2_USE_MACOS
