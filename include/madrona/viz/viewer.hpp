#pragma once

#include <madrona/types.hpp>
#include <madrona/render/mw.hpp>
#include <madrona/importer.hpp>
#include <madrona/exec_mode.hpp>
#include <memory>

namespace madrona::viz {

class Viewer {
public:
    struct Config {
        int gpuID;
        uint32_t renderWidth;
        uint32_t renderHeight;
        uint32_t numWorlds;
        uint32_t maxViewsPerWorld;
        uint32_t maxInstancesPerWorld;
        uint32_t defaultSimTickRate;
        ExecMode execMode;
    };

    enum class KeyboardKey : uint32_t {
        W,
        A,
        S,
        D,
        Q,
        E,
        R,
        X,
        Z,
        C,
        NumKeys = C + 1,
    };

    class UserInput {
    public:
        inline UserInput(bool *keys_state);

        inline bool keyPressed(KeyboardKey key) const;

    private:
        bool *keys_state_;
    };

    Viewer(const Config &cfg);
    Viewer(Viewer &&o);

    ~Viewer();

    CountT loadObjects(Span<const imp::SourceObject> objs, Span<const imp::SourceMaterial> mats);

    const render::RendererBridge * rendererBridge() const;

    template <typename InputFn, typename StepFn>
    void loop(InputFn &&input_fn, StepFn &&step_fn);

private:
    void loop(void (*input_fn)(void *, CountT, CountT, const UserInput &),
              void *input_data, void (*step_fn)(void *), void *step_data);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#include "viewer.inl"
