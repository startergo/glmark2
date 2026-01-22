#ifndef GLMARK2_NATIVE_STATE_MACOS_H_
#define GLMARK2_NATIVE_STATE_MACOS_H_

#include "native-state.h"

class NativeStateMacOS : public NativeState
{
public:
    NativeStateMacOS();
    ~NativeStateMacOS();

    bool init_display() override;
    void* display() override;
    bool create_window(WindowProperties const& properties) override;
    void* window(WindowProperties& properties) override;
    void visible(bool v) override;
    bool should_quit() override;
    void flip() override;

private:
    struct Impl;
    Impl* impl_;
};

#endif /* GLMARK2_NATIVE_STATE_MACOS_H_ */
