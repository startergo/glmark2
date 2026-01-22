#ifndef GLMARK2_GL_STATE_MACOS_H_
#define GLMARK2_GL_STATE_MACOS_H_

#include "gl-state.h"
#include "gl-visual-config.h"
#include "gl-headers.h"
#include "shared-library.h"

#include <vector>

class GLStateMacOS : public GLState
{
public:
    GLStateMacOS();
    ~GLStateMacOS() override;

    bool init_display(void* native_display, GLVisualConfig& config_pref) override;
    bool init_surface(void* native_window) override;
    bool init_gl_extensions() override;
    bool valid() override;
    bool reset() override;
    void swap() override;
    bool gotNativeConfig(intptr_t& vid, std::vector<uint64_t>& mods) override;
    void getVisualConfig(GLVisualConfig& vc) override;
    bool supports_sync() override;
    std::unique_ptr<GLStateSync> sync() override;

private:
    static GLADapiproc load_proc(void* userptr, const char* name);
    bool ensure_context();
    void update_visual_config();

    void* view_;
    GLVisualConfig requested_visual_config_;
    GLVisualConfig active_visual_config_;
    SharedLibrary lib_;

    struct Impl;
    Impl* impl_;
};

#endif /* GLMARK2_GL_STATE_MACOS_H_ */
