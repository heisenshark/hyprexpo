#pragma once
#define WLR_USE_UNSTABLE

#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <string>
#include <memory>

class IOverview {
  public:
    IOverview()          = default;
    virtual ~IOverview();

    virtual void  render()           = 0;
    virtual void  damage()           = 0;
    virtual void  onDamageReported() = 0;
    virtual void  onPreRender()      = 0;

    virtual void  setClosing(bool closing) = 0;
    virtual bool  closeCommitted() const = 0;
    virtual bool  shouldRenderOverviewForMonitor(const PHLMONITOR& monitor) const = 0;
    virtual void  onWindowMoveToWorkspace(const PHLWINDOW& window, const PHLWORKSPACE& workspace) = 0;

    virtual void  resetSwipe()                = 0;
    virtual void  onSwipeUpdate(double delta) = 0;
    virtual void  onSwipeEnd()                = 0;

    virtual void  fullRender() = 0;
    virtual void  close(bool switchToSelection = true) = 0;
    virtual void  selectHoveredWorkspace() = 0;

    // keyboard navigation interface
    virtual void  onKbMoveFocus(const std::string& dir) = 0;
    virtual void  onKbConfirm() = 0;
    virtual void  onKbSelectNumber(int num) = 0;
    virtual void  onKbSelectToken(int visibleIdx) = 0;
    virtual bool  selectVisibleToken(const std::string& token) = 0;
    virtual int64_t selectedWorkspaceID() const = 0;
    virtual bool  selectWorkspaceByID(int64_t workspaceID) = 0;
    virtual bool  selectVisibleIndex(size_t index) = 0;
    virtual bool  moveWindowBetweenVisibleIndices(size_t sourceIndex, size_t targetIndex, const PHLWINDOW& window = nullptr) = 0;

    void          enterSubmapIfEnabled();
    void          resetSubmapIfNeeded();

    bool          blockOverviewRendering = false;
    bool          blockDamageReporting   = false;

    PHLMONITORREF pMonitor;
    bool          m_isSwiping = false;
    bool          submapActive = false;
};

inline std::unique_ptr<IOverview> g_pOverview;
