#pragma once
#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include "IOverview.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <vector>

class CMonitor;

class CScrollOverview : public IOverview {
  public:
    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false);
    virtual ~CScrollOverview();

    void render() override;
    void damage() override;
    void onDamageReported() override;
    void onPreRender() override;

    void setClosing(bool closing) override;
    bool closeCommitted() const override { return closing; }
    bool shouldRenderOverviewForMonitor(const PHLMONITOR& monitor) const override { return pMonitor == monitor; }
    void onWindowMoveToWorkspace(const PHLWINDOW& window, const PHLWORKSPACE& workspace) override {}

    void resetSwipe() override;
    void onSwipeUpdate(double delta) override;
    void onSwipeEnd() override;

    void close(bool switchToSelection = true) override;
    void selectHoveredWorkspace() override;

    void onKbMoveFocus(const std::string& dir) override {}
    void onKbConfirm() override {}
    void onKbSelectNumber(int num) override {}
    void onKbSelectToken(int visibleIdx) override {}
    bool selectVisibleToken(const std::string& token) override { return false; }
    int64_t selectedWorkspaceID() const override { return startedOn ? startedOn->m_id : -1; }
    bool selectWorkspaceByID(int64_t workspaceID) override { return false; }
    bool selectVisibleIndex(size_t index) override { return false; }
    bool moveWindowBetweenVisibleIndices(size_t sourceIndex, size_t targetIndex, const PHLWINDOW& window = nullptr) override { return false; }

    void fullRender() override;

  private:
    void   redrawWorkspace(PHLWORKSPACE w, bool forcelowres = false);
    void   redrawAll(bool forcelowres = false);
    void   onWorkspaceChange();
    void   highlightHoverDebug();
    void   moveViewportWorkspace(bool up);

    bool   damageDirty              = false;
    size_t viewportCurrentWorkspace = 0;

    struct SWindowImage {
        PHLWINDOWREF            pWindow;
        SP<Render::IFramebuffer> fb;
        bool                    highlight = false;
        CHyprSignalListener windowCommit;
        Vector2D                lastWindowPosition, lastWindowSize;
    };

    void redrawWindowImage(SP<SWindowImage>);

    struct SWorkspaceImage {
        PHLWORKSPACE                  pWorkspace;
        CBox                          box;
        std::vector<SP<SWindowImage>> windowImages;
    };

    SP<Render::IFramebuffer>         backgroundFb;
    SP<Render::IFramebuffer>         floatingFb;

    Vector2D                         lastMousePosLocal = Vector2D{};

    PHLWINDOWREF                     closeOnWindow;

    std::vector<SP<SWorkspaceImage>> images;
    SP<SWorkspaceImage>              imageForWorkspace(PHLWORKSPACE w);

    PHLWORKSPACE                     startedOn;

    PHLANIMVAR<float>                scale;
    PHLANIMVAR<Vector2D>             viewOffset;

    bool                             closing = false;

    CHyprSignalListener              mouseMoveHook;
    CHyprSignalListener              mouseButtonHook;
    CHyprSignalListener              touchMoveHook;
    CHyprSignalListener              touchDownHook;
    CHyprSignalListener              mouseAxisHook;
    CHyprSignalListener              windowOpenHook;

    bool                             swipe             = false;
    bool                             swipeWasCommenced = false;

    friend class COverviewPassElement;
};
