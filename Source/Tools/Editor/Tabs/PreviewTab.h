//
// Copyright (c) 2018 Rokas Kupstys
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#pragma once


#include <Urho3D/Core/Object.h>
#include <Urho3D/Scene/Scene.h>
#include "Tabs/Scene/SceneTab.h"


namespace Urho3D
{

enum SceneSimulationStatus
{
    SCENE_SIMULATION_STOPPED,
    SCENE_SIMULATION_RUNNING,
    SCENE_SIMULATION_PAUSED,
};

class PreviewTab : public Tab
{
    URHO3D_OBJECT(PreviewTab, Tab)
public:
    explicit PreviewTab(Context* context);

    bool RenderWindowContent() override;
    /// Set color of view texture to black.
    void Clear();

    /// Render play/pause/restore/step/store buttons.
    void RenderButtons();

    /// Start playing a scene. If scene is already playing this does nothing.
    void Play();
    /// Pause playing a scene. If scene is stopped or paused this does nothing.
    void Pause();
    /// Toggle between play/pause states.
    void Toggle();
    /// Simulate single frame. If scene is not paused this does nothing.
    void Step(float timeStep);
    /// Stop scene simulation. If scene is already stopped this does nothing.
    void Stop();
    /// Take a snapshot of current scene state and use it as "master" state. Stopping simulation will revert to this new state. Clears all scene undo actions!
    void Snapshot();
    /// Returns true when scene is playing or paysed.
    bool IsScenePlaying() const { return simulationStatus_ != SCENE_SIMULATION_STOPPED; }
    /// Returns current scene simulation status.
    SceneSimulationStatus GetSceneSimulationStatus() const { return simulationStatus_; }

protected:
    ///
    IntRect UpdateViewRect() override;
    ///
    void UpdateViewports();
    ///
    void OnComponentUpdated(Component* component);

    /// Last view rectangle.
    IntRect viewRect_{};
    /// Texture used to display preview.
    SharedPtr<Texture2D> view_{};

    /// Scene which can be simulated.
    WeakPtr<SceneTab> sceneTab_;
    /// Flag controlling scene updates in the viewport.
    SceneSimulationStatus simulationStatus_ = SCENE_SIMULATION_STOPPED;
    /// Temporary storage of scene data used in play/pause functionality.
    VectorBuffer sceneState_;
    /// Temporary storage of scene data used when plugins are being reloaded.
    VectorBuffer sceneReloadState_;
    /// Time since ESC was last pressed. Used for double-press ESC to exit scene simulation.
    unsigned lastEscPressTime_ = 0;
    ///
    bool sceneMouseVisible_ = true;
    ///
    MouseMode sceneMouseMode_ = MM_FREE;};

}