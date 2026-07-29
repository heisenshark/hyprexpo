#include "ScrollOverview.hpp"
#include "HyprexpoLogic.hpp"
#include <any>
#include <vector>
#include <unordered_map>
#include <map>
#include <string>
#include <algorithm>
#include <memory>
#include <limits>
#include <cmath>
#include <GLES3/gl32.h>

#include "HyprlandConfigCompat.hpp"
#define private public
#define protected public
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#undef private
#undef protected
#include "OverviewPassElement.hpp"

static void clearWithColor(const CHyprColor& color) {
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void damageMonitor(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    if (g_pOverview) g_pOverview->damage();
}

static void removeOverview(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    g_pOverview.reset();
}

CScrollOverview::~CScrollOverview() {
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    images.clear(); // otherwise we get a vram leak
    Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    resetSubmapIfNeeded();
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_) : startedOn(startedOn_), swipe(swipe_) {
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)CompatHyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();

    const auto          PMONITOR = Desktop::focusState()->monitor();
    pMonitor                     = PMONITOR;

    for (const auto& w_weak : State::workspaceState()->workspaces()) {
        PHLWORKSPACE w = w_weak.lock();
        if (w && w->m_monitor == pMonitor && !w->m_isSpecialWorkspace) {
            auto img = makeShared<SWorkspaceImage>();
            img->pWorkspace = w;
            Animation::mgr()->createAnimation(0.F, img->hScrollX, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
            img->hScrollX->setUpdateCallback(damageMonitor);
            images.emplace_back(img);
        }
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

    Animation::mgr()->createAnimation(1.F, scale, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(Vector2D{}, viewOffset, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);

    scale->setUpdateCallback(damageMonitor);
    viewOffset->setUpdateCallback(damageMonitor);

    if (!swipe)
        *scale = std::clamp(**PDEFAULTZOOM, 0.1F, 0.9F);

    lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

    for (const auto& w : Desktop::windowState()->windows()) {
        if (w && Desktop::View::validMapped(w)) {
            if (Desktop::focusState()->window() == w && !w->m_realBorderColor.m_colors.empty())
                activeBorderGradient = w->m_realBorderColor;
            else if (!w->m_realBorderColor.m_colors.empty())
                inactiveBorderGradient = w->m_realBorderColor;
        }
    }
    if (activeBorderGradient.m_colors.empty())
        activeBorderGradient = Config::CGradientValueData(CHyprColor{0.2f, 0.6f, 1.0f, 1.0f});
    if (inactiveBorderGradient.m_colors.empty())
        inactiveBorderGradient = Config::CGradientValueData(CHyprColor{0.3f, 0.3f, 0.3f, 0.5f});

    auto onCursorMove = [this](Event::SCallbackInfo& info) {
        if (closing)
            return;
        Vector2D currentMousePos = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
        Vector2D delta = currentMousePos - lastMousePosLocal;
        if (delta.x * delta.x + delta.y * delta.y < 1.0)
            return;
        lastMousePosLocal = currentMousePos;
        updateMouseHover();
    };

    auto onCursorSelect = [this](Event::SCallbackInfo& info) {
        if (closing)
            return;
        selectHoveredWorkspace();
        close();
    };

    auto onMouseAxis = [this](const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (closing)
            return;

        static auto* const* PZOOM = (Hyprlang::INT* const*)CompatHyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:scroll_moves_up_down")->getDataStaticPtr();

        if (!**PZOOM) {
            const auto VAL = std::clamp(sc<float>(scale->value() + e.delta / -500.F), 0.05F, 0.95F);
            *scale         = VAL;
        } else
            moveViewportWorkspace(e.delta > 0);
    };

    auto onWindowOpen = [this](std::any) {
        if (closing)
            return;
        redrawAll();
    };

    mouseMoveHook   = Event::bus()->m_events.input.mouse.move.listen([onCursorMove](const Vector2D&, Event::SCallbackInfo& info) { onCursorMove(info); });
    touchMoveHook   = Event::bus()->m_events.input.touch.motion.listen([onCursorMove](const ITouch::SMotionEvent&, Event::SCallbackInfo& info) { onCursorMove(info); });
    mouseAxisHook   = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);

    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen([onCursorSelect](const IPointer::SButtonEvent&, Event::SCallbackInfo& info) { onCursorSelect(info); });
    touchDownHook   = Event::bus()->m_events.input.touch.down.listen([onCursorSelect](const ITouch::SDownEvent&, Event::SCallbackInfo& info) { onCursorSelect(info); });

    windowOpenHook  = Event::bus()->m_events.window.open.listen(onWindowOpen);

    Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);

    redrawAll();

    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    viewportCurrentWorkspace = activeIdx;

    const auto focused = Desktop::focusState()->window();
    if (focused && Desktop::View::validMapped(focused) && !focused->m_isFloating && focused->m_workspace == startedOn) {
        closeOnWindow = focused;
    } else if (viewportCurrentWorkspace < images.size()) {
        for (const auto& img : images[viewportCurrentWorkspace]->windowImages) {
            if (img->pWindow && Desktop::View::validMapped(img->pWindow.lock()) && !img->pWindow->m_isFloating) {
                closeOnWindow = img->pWindow;
                break;
            }
        }
    }

    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock()) && !closeOnWindow->m_isFloating) {
        PHLWINDOW win = closeOnWindow.lock();
        preferredKbCenterX = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle().x;
        if (viewportCurrentWorkspace < images.size())
            images[viewportCurrentWorkspace]->lastFocusedWindow = closeOnWindow;
        Desktop::focusState()->fullWindowFocus(win, Desktop::eFocusReason::FOCUS_REASON_UNKNOWN);
    }

    updateViewportOffset();
    enterSubmapIfEnabled();
}

PHLWINDOW CScrollOverview::getWindowAtPoint(const Vector2D& pointLocal) {
    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    const auto VIEWPORT_CENTER = CBox{{}, pMonitor->m_size}.middle();
    float yoff = -(float)activeIdx * pMonitor->m_size.y * scale->value();

    for (size_t i = 0; i < images.size(); ++i) {
        const auto& wimg = images[i];
        float wsXOff = wimg->hScrollX ? wimg->hScrollX->value() : 0.F;
        Vector2D wsOffset = Vector2D{-wsXOff * scale->value(), yoff - viewOffset->value().y * scale->value()};

        for (const auto& img : wimg->windowImages) {
            if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()) || img->pWindow->m_isFloating)
                continue;
            CBox texbox = {img->lastWindowPosition - pMonitor->m_position, img->lastWindowSize};
            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(wsOffset);

            if (texbox.containsPoint(pointLocal))
                return img->pWindow.lock();
        }
        yoff += pMonitor->m_size.y * scale->value();
    }
    return nullptr;
}

void CScrollOverview::redrawWindowFor(PHLWINDOW win) {
    if (!win)
        return;
    for (const auto& wimg : images) {
        for (const auto& img : wimg->windowImages) {
            if (img->pWindow && img->pWindow.lock() == win) {
                redrawWindowImage(img);
                return;
            }
        }
    }
}

void CScrollOverview::updateMouseHover() {
    if (closing)
        return;
    PHLWINDOW hovered = getWindowAtPoint(lastMousePosLocal);
    if (hovered && hovered != closeOnWindow.lock()) {
        PHLWINDOW oldWin = closeOnWindow ? closeOnWindow.lock() : nullptr;
        mouseHoveredWindow = hovered;
        closeOnWindow = hovered;
        if (hovered->m_workspace && hovered->m_workspace != pMonitor->m_activeWorkspace) {
            (void)Config::Actions::changeWorkspace(std::to_string(hovered->m_workspace->m_id));
        }
        Desktop::focusState()->fullWindowFocus(hovered, Desktop::eFocusReason::FOCUS_REASON_UNKNOWN);
        if (oldWin && oldWin != hovered)
            redrawWindowFor(oldWin);
        redrawWindowFor(hovered);
        damage();
    }
}

void CScrollOverview::selectHoveredWorkspace() {
    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    const auto VIEWPORT_CENTER = CBox{{}, pMonitor->m_size}.middle();

    float yoff = -(float)activeIdx * pMonitor->m_size.y * scale->value();
    bool foundWin = false;

    for (size_t i = 0; i < images.size(); ++i) {
        const auto& wimg = images[i];
        float wsXOff = wimg->hScrollX ? wimg->hScrollX->value() : 0.F;
        Vector2D wsOffset = Vector2D{-wsXOff * scale->value(), yoff - viewOffset->value().y * scale->value()};

        for (const auto& img : wimg->windowImages) {
            if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()) || img->pWindow->m_isFloating)
                continue;
            CBox texbox = {img->pWindow->m_realPosition->value() - pMonitor->m_position, img->pWindow->m_realSize->value()};
            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(wsOffset);

            if (texbox.containsPoint(lastMousePosLocal)) {
                closeOnWindow = img->pWindow;
                foundWin = true;
                break;
            }
        }
        if (foundWin)
            break;
        yoff += pMonitor->m_size.y * scale->value();
    }

    if (!foundWin) {
        yoff = -(float)activeIdx * pMonitor->m_size.y * scale->value();
        for (size_t i = 0; i < images.size(); ++i) {
            float wsXOff = images[i]->hScrollX ? images[i]->hScrollX->value() : 0.F;
            Vector2D wsOffset = Vector2D{-wsXOff * scale->value(), yoff - viewOffset->value().y * scale->value()};

            CBox wsBox = CBox{{}, pMonitor->m_size};
            wsBox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(wsOffset);

            if (wsBox.containsPoint(lastMousePosLocal)) {
                viewportCurrentWorkspace = i;
                closeOnWindow = nullptr;
                break;
            }
            yoff += pMonitor->m_size.y * scale->value();
        }
    }

    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock()) && !closeOnWindow->m_isFloating) {
        PHLWINDOW win = closeOnWindow.lock();
        preferredKbCenterX = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle().x;
        if (viewportCurrentWorkspace < images.size())
            images[viewportCurrentWorkspace]->lastFocusedWindow = closeOnWindow;
    }

    updateViewportOffset();
}

void CScrollOverview::updateViewportOffset() {
    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    float targetY = (sc<long>(viewportCurrentWorkspace) - sc<long>(activeIdx)) * pMonitor->m_size.y;
    *viewOffset = Vector2D{0.F, targetY};

    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock())) {
        PHLWINDOW win = closeOnWindow.lock();
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i]->pWorkspace == win->m_workspace) {
                Vector2D winLocalCenter = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
                Vector2D monitorCenter = pMonitor->m_size / 2.0;
                float targetX = winLocalCenter.x - monitorCenter.x;

                if (images[i]->hScrollX)
                    *images[i]->hScrollX = targetX;
                break;
            }
        }
    }
}

void CScrollOverview::moveViewportWorkspace(bool up) {
    if (viewportCurrentWorkspace == 0 && !up)
        return;
    if (viewportCurrentWorkspace == images.size() - 1 && up)
        return;

    if (up)
        viewportCurrentWorkspace++;
    else
        viewportCurrentWorkspace--;

    closeOnWindow = nullptr;
    if (viewportCurrentWorkspace < images.size()) {
        for (const auto& img : images[viewportCurrentWorkspace]->windowImages) {
            if (img->pWindow && Desktop::View::validMapped(img->pWindow.lock())) {
                closeOnWindow = img->pWindow;
                break;
            }
        }
    }

    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock())) {
        PHLWINDOW win = closeOnWindow.lock();
        Desktop::focusState()->fullWindowFocus(win, Desktop::eFocusReason::FOCUS_REASON_UNKNOWN);
    }

    updateViewportOffset();
}

void CScrollOverview::highlightHoverDebug() {
    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    const auto VIEWPORT_CENTER = CBox{{}, pMonitor->m_size}.middle();

    float      yoff = -(float)activeIdx * pMonitor->m_size.y * scale->value();
    for (const auto& wimg : images) {
        for (const auto& img : wimg->windowImages) {
            CBox texbox = {img->pWindow->m_realPosition->value() - pMonitor->m_position, img->pWindow->m_realSize->value()};

            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
            texbox.translate({0.F, yoff});

            if (texbox.containsPoint(lastMousePosLocal)) {
                img->highlight = true;
                continue;
            }

            img->highlight = false;
        }
        yoff += pMonitor->m_size.y * scale->value();
    }
}

SP<CScrollOverview::SWorkspaceImage> CScrollOverview::imageForWorkspace(PHLWORKSPACE w) {
    for (const auto& i : images) {
        if (i->pWorkspace == w)
            return i;
    }
    return nullptr;
}

void CScrollOverview::redrawWorkspace(PHLWORKSPACE workspace, bool forcelowres) {
    if (pMonitor->m_activeWorkspace != startedOn && !closing) {
        onWorkspaceChange();
    }

    blockOverviewRendering = true;

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    auto image = imageForWorkspace(workspace);

    if (!image)
        return;

    std::vector<PHLWINDOW> windows;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!Desktop::View::valid(w) || w->m_isFloating || w->m_workspace != workspace)
            continue;
        windows.emplace_back(w);
    }

    for (const auto& w : windows) {
        auto img     = image->windowImages.emplace_back(makeShared<SWindowImage>());
        img->pWindow = w;
        img->fb = g_pHyprRenderer->createFB("hyprexpo-window");
        img->fb->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y, pMonitor->m_output->state->state().drmFormat);
        if (!w->m_isX11 && w->wlSurface()) {
            img->windowCommit = w->wlSurface()->resource()->m_events.commit.listen([wk = WP<SWindowImage>{img}] {
                if (wk.expired())
                    return;
                ((CScrollOverview*)g_pOverview.get())->redrawWindowImage(wk.lock());
                g_pOverview->damage();
            });
        }

        redrawWindowImage(img);
    }

    blockOverviewRendering = false;
}

void CScrollOverview::redrawWindowImage(SP<SWindowImage> img) {
    if (!img->pWindow)
        return;

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(pMonitor.lock(), fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, img->fb);

    clearWithColor(CHyprColor{0, 0, 0, 0});

    g_pHyprRenderer->renderWindow(img->pWindow.lock(), pMonitor.lock(), Time::steadyNow(), true, Render::RENDER_PASS_ALL, true, true);

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    img->lastWindowPosition = img->pWindow->m_realPosition->value();
    img->lastWindowSize     = img->pWindow->m_realSize->value();
}

void CScrollOverview::redrawAll(bool forcelowres) {

    for (const auto& img : images) {
        redrawWorkspace(img->pWorkspace);
    }

    if (!backgroundFb)
        backgroundFb = g_pHyprRenderer->createFB("hyprexpo-bg");
    if (!floatingFb)
        floatingFb = g_pHyprRenderer->createFB("hyprexpo-float");

    if (backgroundFb->m_size != pMonitor->m_pixelSize) {
        backgroundFb->release();
        backgroundFb->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y, pMonitor->m_output->state->state().drmFormat);
        floatingFb->release();
        floatingFb->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y, pMonitor->m_output->state->state().drmFormat);
    }

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(pMonitor.lock(), fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, backgroundFb);

    clearWithColor(CHyprColor{0, 0, 0, 1.0});

    g_pHyprRenderer->renderAllClientsForWorkspace(pMonitor.lock(), nullptr, Time::steadyNow());

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    g_pHyprRenderer->beginRender(pMonitor.lock(), fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, floatingFb);

    clearWithColor(CHyprColor{0, 0, 0, 0});

    for (const auto& w : Desktop::windowState()->windows()) {
        if (!Desktop::View::validMapped(w) || !w->m_isFloating || w->m_workspace != startedOn)
            continue;

        g_pHyprRenderer->renderWindow(w, pMonitor.lock(), Time::steadyNow(), false, Render::RENDER_PASS_ALL);
    }

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
}

void CScrollOverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void CScrollOverview::onDamageReported() {}

void CScrollOverview::close(bool switchToSelection) {
    if (closing)
        return;

    closing = true;
    resetSubmapIfNeeded();

    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    if (!switchToSelection) {
        if (startedOn && startedOn != pMonitor->m_activeWorkspace) {
            (void)Config::Actions::changeWorkspace(std::to_string(startedOn->m_id));
        }
        *viewOffset = Vector2D{};
    } else if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock())) {
        PHLWINDOW win = closeOnWindow.lock();
        if (win->m_workspace && win->m_workspace != pMonitor->m_activeWorkspace) {
            (void)Config::Actions::changeWorkspace(std::to_string(win->m_workspace->m_id));
        }
        Desktop::focusState()->fullWindowFocus(win, Desktop::eFocusReason::FOCUS_REASON_UNKNOWN);

        float yoff = -(float)activeIdx * pMonitor->m_size.y * scale->value();
        bool found = false;
        for (const auto& wimg : images) {
            for (const auto& img : wimg->windowImages) {
                if (img->pWindow && img->pWindow.lock() == win) {
                    Vector2D middleOfWindow = CBox{img->pWindow->m_realPosition->value(), img->pWindow->m_realSize->value()}.translate({0.F, yoff / scale->value()}).middle() -
                        CBox{pMonitor->m_position, pMonitor->m_size}.middle();

                    *viewOffset = middleOfWindow +
                        (CBox{pMonitor->m_position, pMonitor->m_size}.middle() - CBox{img->pWindow->m_realPosition->value(), img->pWindow->m_realSize->value()}.middle());
                    found = true;
                    break;
                }
            }
            if (found)
                break;
            yoff += pMonitor->m_size.y * scale->value();
        }
    } else {
        if (viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace]->pWorkspace) {
            PHLWORKSPACE targetWs = images[viewportCurrentWorkspace]->pWorkspace;
            if (targetWs != pMonitor->m_activeWorkspace) {
                (void)Config::Actions::changeWorkspace(std::to_string(targetWs->m_id));
            }
            *viewOffset = {viewOffset->value().x, (sc<long>(viewportCurrentWorkspace) - sc<long>(activeIdx)) * pMonitor->m_size.y};
        } else {
            *viewOffset = Vector2D{};
        }
    }

    *scale = 1.F;
    scale->setCallbackOnEnd(removeOverview);
}

void CScrollOverview::onPreRender() {
}

void CScrollOverview::onWorkspaceChange() {
}

void CScrollOverview::render() {
    bool needsDamage = false;
    for (const auto& img : images) {
        for (const auto& i : img->windowImages) {
            if (!i->pWindow)
                continue;

            if (i->lastWindowSize != i->pWindow->m_realSize->value() || i->lastWindowPosition != i->pWindow->m_realPosition->value()) {
                needsDamage           = true;
                i->lastWindowPosition = i->pWindow->m_realPosition->value();
                i->lastWindowSize     = i->pWindow->m_realSize->value();
            }
        }
    }

    if (needsDamage)
        damage();

    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>());
}

void CScrollOverview::fullRender() {

    clearWithColor(CHyprColor{0, 0, 0, 1.0});

    CBox texbox = {{}, pMonitor->m_size};
    texbox.scale(pMonitor->m_scale);
    texbox.round();
    CRegion damage{0, 0, INT16_MAX, INT16_MAX};
    Render::GL::g_pHyprOpenGL->renderTextureInternal(backgroundFb->getTexture(), texbox, Render::GL::CHyprOpenGLImpl::STextureRenderData{.damage = &damage, .a = 1.0f});

    const auto VIEWPORT_CENTER = CBox{{}, pMonitor->m_size}.middle();

    size_t     activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    float yoff = -(float)activeIdx * pMonitor->m_size.y * scale->value();
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& wimg = images[i];
        bool dirty = false;

        float wsXOff = wimg->hScrollX ? wimg->hScrollX->value() : 0.F;
        Vector2D wsOffset = Vector2D{-wsXOff * scale->value(), yoff - viewOffset->value().y * scale->value()};

        for (const auto& img : wimg->windowImages) {
            if (!img->pWindow) {
                dirty = true;
                continue;
            }

            CBox texbox = CBox{img->pWindow->m_realPosition->value() - pMonitor->m_position, pMonitor->m_size};

            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(wsOffset);

            texbox.scale(pMonitor->m_scale).round();
            CRegion damage{0, 0, INT16_MAX, INT16_MAX};
            Render::GL::g_pHyprOpenGL->renderTextureInternal(img->fb->getTexture(), texbox, Render::GL::CHyprOpenGLImpl::STextureRenderData{.damage = &damage, .a = 1.0f * img->pWindow->m_alpha.value()});

            PHLWINDOW win = img->pWindow.lock();
            if (win) {
                CBox winBox = CBox{img->lastWindowPosition - pMonitor->m_position, img->lastWindowSize};
                winBox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(wsOffset);
                winBox.scale(pMonitor->m_scale).round();

                int bSize = win->getRealBorderSize();
                if (bSize <= 0)
                    bSize = 2;

                const int scaledBSize = std::max(1, (int)std::lround(bSize * scale->value() * pMonitor->m_scale));
                const int roundPx     = std::max(0, (int)std::lround(win->rounding() * scale->value() * pMonitor->m_scale));

                bool isFocused = (closeOnWindow && closeOnWindow.lock() == win);
                Config::CGradientValueData grad = isFocused ? activeBorderGradient : inactiveBorderGradient;

                Render::GL::g_pHyprOpenGL->renderBorder(winBox, grad, Render::GL::CHyprOpenGLImpl::SBorderRenderData{
                    .round = roundPx,
                    .roundingPower = win->roundingPower(),
                    .borderSize = scaledBSize,
                    .a = 1.0f * win->m_alpha.value()
                });
            }

            if (img->highlight) {
                CBox texbox2 = CBox{img->pWindow->m_realPosition->value() - pMonitor->m_position, img->pWindow->m_realSize->value()};
                texbox2.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(wsOffset);
                texbox2.scale(pMonitor->m_scale).round();
                Render::GL::g_pHyprOpenGL->renderRect(texbox2, CHyprColor{0.5, 0.0, 0.0, 0.5}, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 5});
            }
        }
        yoff += pMonitor->m_size.y * scale->value();

        if (dirty)
            std::erase_if(wimg->windowImages, [](const auto& e) { return !e->pWindow; });
    }

    if (!closeOnWindow && viewportCurrentWorkspace < images.size()) {
        float yoff = (sc<float>(viewportCurrentWorkspace) - sc<float>(activeIdx)) * pMonitor->m_size.y * scale->value();
        float wsXOff = images[viewportCurrentWorkspace]->hScrollX ? images[viewportCurrentWorkspace]->hScrollX->value() : 0.F;
        Vector2D wsOffset = Vector2D{-wsXOff * scale->value(), yoff - viewOffset->value().y * scale->value()};

        CBox wsBox = CBox{{}, pMonitor->m_size};
        wsBox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(wsOffset);
        wsBox.scale(pMonitor->m_scale).round();

        const int roundPx = std::max(0, (int)std::lround(8.0 * pMonitor->m_scale));
        Render::GL::g_pHyprOpenGL->renderRect(wsBox, CHyprColor{0.2f, 0.6f, 1.0f, 0.20f}, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = roundPx});
    }
}

static float hyprlerp(const float& from, const float& to, const float perc) {
    return (to - from) * perc + from;
}

static Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return Vector2D{hyprlerp(from.x, to.x, perc), hyprlerp(from.y, to.y, perc)};
}

void CScrollOverview::setClosing(bool closing_) {
    closing = closing_;
}

void CScrollOverview::resetSwipe() {
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)CompatHyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();

    if (closing) {
        close();
        return;
    }

    (*scale)    = **PDEFAULTZOOM;
    m_isSwiping = false;
}

void CScrollOverview::onSwipeUpdate(double delta) {
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)CompatHyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();
    static auto* const* PDISTANCE    = (Hyprlang::INT* const*)CompatHyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:gesture_distance")->getDataStaticPtr();

    m_isSwiping = true;

    const float PERC = closing ? std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0) : 1.0 - std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0);

    scale->setValueAndWarp(hyprlerp(1.F, **PDEFAULTZOOM, PERC));
}

void CScrollOverview::onSwipeEnd() {
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)CompatHyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();

    if (closing) {
        close();
        return;
    }

    (*scale)    = **PDEFAULTZOOM;
    m_isSwiping = false;
}

PHLWINDOW CScrollOverview::getFocusedOrFirstWindow() {
    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock()))
        return closeOnWindow.lock();

    const auto focused = Desktop::focusState()->window();
    if (focused && Desktop::View::validMapped(focused)) {
        if (viewportCurrentWorkspace < images.size() && focused->m_workspace == images[viewportCurrentWorkspace]->pWorkspace) {
            closeOnWindow = focused;
            return focused;
        }
    }

    if (viewportCurrentWorkspace < images.size()) {
        for (const auto& img : images[viewportCurrentWorkspace]->windowImages) {
            if (img->pWindow && Desktop::View::validMapped(img->pWindow.lock())) {
                closeOnWindow = img->pWindow;
                return img->pWindow.lock();
            }
        }
    }

    return nullptr;
}

int64_t CScrollOverview::selectedWorkspaceID() const {
    if (closeOnWindow && closeOnWindow->m_workspace)
        return closeOnWindow->m_workspace->m_id;

    if (viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace]->pWorkspace)
        return images[viewportCurrentWorkspace]->pWorkspace->m_id;

    return startedOn ? startedOn->m_id : WORKSPACE_INVALID;
}

bool CScrollOverview::selectVisibleIndex(size_t index) {
    if (index >= images.size() || !images[index]->pWorkspace)
        return false;

    viewportCurrentWorkspace = index;
    closeOnWindow = nullptr;
    preferredKbCenterX.reset();

    if (images[index]->lastFocusedWindow && Desktop::View::validMapped(images[index]->lastFocusedWindow.lock())) {
        closeOnWindow = images[index]->lastFocusedWindow;
    } else {
        for (const auto& img : images[index]->windowImages) {
            if (img->pWindow && Desktop::View::validMapped(img->pWindow.lock())) {
                closeOnWindow = img->pWindow;
                images[index]->lastFocusedWindow = closeOnWindow;
                break;
            }
        }
    }

    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock())) {
        PHLWINDOW win = closeOnWindow.lock();
        preferredKbCenterX = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle().x;
        Desktop::focusState()->fullWindowFocus(win, Desktop::eFocusReason::FOCUS_REASON_UNKNOWN);
    }

    updateViewportOffset();
    damage();
    return true;
}

bool CScrollOverview::selectWorkspaceByID(int64_t workspaceID) {
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace->m_id == workspaceID)
            return selectVisibleIndex(i);
    }
    return false;
}

void CScrollOverview::onKbConfirm() {
    if (closing)
        return;
    (void)getFocusedOrFirstWindow();
    close();
}

void CScrollOverview::onKbSelectNumber(int num) {
    if (closing)
        return;
    if (num == 0)
        num = 10;
    if (selectVisibleIndex(num - 1))
        close();
}

void CScrollOverview::onKbSelectToken(int visibleIdx) {
    if (closing)
        return;
    if (selectVisibleIndex(visibleIdx))
        close();
}

bool CScrollOverview::selectVisibleToken(const std::string& token) {
    const int idx = Hyprexpo::fallbackTokenToVisibleIndex(token);
    if (idx < 0)
        return false;
    return selectVisibleIndex(idx);
}

void CScrollOverview::onKbMoveFocus(const std::string& dir) {
    if (closing || images.empty())
        return;

    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    size_t currentWsIdx = viewportCurrentWorkspace;
    if (currentWsIdx >= images.size())
        currentWsIdx = activeIdx;

    PHLWINDOW currentWin = nullptr;
    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock())) {
        PHLWINDOW w = closeOnWindow.lock();
        if (w->m_workspace == images[currentWsIdx]->pWorkspace)
            currentWin = w;
    }

    if (!currentWin && currentWsIdx < images.size()) {
        if (images[currentWsIdx]->lastFocusedWindow && Desktop::View::validMapped(images[currentWsIdx]->lastFocusedWindow.lock())) {
            currentWin = images[currentWsIdx]->lastFocusedWindow.lock();
        } else {
            const auto focused = Desktop::focusState()->window();
            if (focused && Desktop::View::validMapped(focused) && focused->m_workspace == images[currentWsIdx]->pWorkspace) {
                currentWin = focused;
            } else {
                for (const auto& img : images[currentWsIdx]->windowImages) {
                    if (img->pWindow && Desktop::View::validMapped(img->pWindow.lock())) {
                        currentWin = img->pWindow.lock();
                        break;
                    }
                }
            }
        }
    }

    Vector2D currentLocalCenter = currentWin ? CBox{currentWin->m_realPosition->value() - pMonitor->m_position, currentWin->m_realSize->value()}.middle() : (pMonitor->m_size / 2.0);

    if (currentWin && !preferredKbCenterX.has_value()) {
        preferredKbCenterX = currentLocalCenter.x;
    }
    double targetRefX = preferredKbCenterX.value_or(currentLocalCenter.x);

    if (dir == "down") {
        PHLWINDOW bestWin = nullptr;
        if (currentWin) {
            double bestScore = std::numeric_limits<double>::max();
            for (const auto& img : images[currentWsIdx]->windowImages) {
                if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                    continue;
                PHLWINDOW win = img->pWindow.lock();
                if (win == currentWin)
                    continue;

                Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
                double dy = center.y - currentLocalCenter.y;
                double dx = center.x - targetRefX;
                if (dy > 20.0) {
                    double score = dy + 1.5 * std::abs(dx);
                    if (score < bestScore) {
                        bestScore = score;
                        bestWin = win;
                    }
                }
            }
        }

        if (bestWin) {
            closeOnWindow = bestWin;
            images[currentWsIdx]->lastFocusedWindow = bestWin;
            Vector2D center = CBox{bestWin->m_realPosition->value() - pMonitor->m_position, bestWin->m_realSize->value()}.middle();
            preferredKbCenterX = center.x;
        } else if (currentWsIdx + 1 < images.size()) {
            viewportCurrentWorkspace = currentWsIdx + 1;

            PHLWINDOW nextWin = nullptr;
            if (images[viewportCurrentWorkspace]->lastFocusedWindow && Desktop::View::validMapped(images[viewportCurrentWorkspace]->lastFocusedWindow.lock())) {
                nextWin = images[viewportCurrentWorkspace]->lastFocusedWindow.lock();
            } else {
                double bestDx = std::numeric_limits<double>::max();
                for (const auto& img : images[viewportCurrentWorkspace]->windowImages) {
                    if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                        continue;
                    PHLWINDOW win = img->pWindow.lock();
                    Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
                    double dx = std::abs(center.x - targetRefX);
                    if (dx < bestDx) {
                        bestDx = dx;
                        nextWin = win;
                    }
                }
                if (nextWin)
                    images[viewportCurrentWorkspace]->lastFocusedWindow = nextWin;
            }
            closeOnWindow = nextWin;
            if (nextWin) {
                Vector2D center = CBox{nextWin->m_realPosition->value() - pMonitor->m_position, nextWin->m_realSize->value()}.middle();
                preferredKbCenterX = center.x;
            }
        }
        updateViewportOffset();
        damage();
    } else if (dir == "up") {
        PHLWINDOW bestWin = nullptr;
        if (currentWin) {
            double bestScore = std::numeric_limits<double>::max();
            for (const auto& img : images[currentWsIdx]->windowImages) {
                if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                    continue;
                PHLWINDOW win = img->pWindow.lock();
                if (win == currentWin)
                    continue;

                Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
                double dy = currentLocalCenter.y - center.y;
                double dx = center.x - targetRefX;
                if (dy > 20.0) {
                    double score = dy + 1.5 * std::abs(dx);
                    if (score < bestScore) {
                        bestScore = score;
                        bestWin = win;
                    }
                }
            }
        }

        if (bestWin) {
            closeOnWindow = bestWin;
            images[currentWsIdx]->lastFocusedWindow = bestWin;
            Vector2D center = CBox{bestWin->m_realPosition->value() - pMonitor->m_position, bestWin->m_realSize->value()}.middle();
            preferredKbCenterX = center.x;
        } else if (currentWsIdx > 0) {
            viewportCurrentWorkspace = currentWsIdx - 1;

            PHLWINDOW prevWin = nullptr;
            if (images[viewportCurrentWorkspace]->lastFocusedWindow && Desktop::View::validMapped(images[viewportCurrentWorkspace]->lastFocusedWindow.lock())) {
                prevWin = images[viewportCurrentWorkspace]->lastFocusedWindow.lock();
            } else {
                double bestDx = std::numeric_limits<double>::max();
                for (const auto& img : images[viewportCurrentWorkspace]->windowImages) {
                    if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                        continue;
                    PHLWINDOW win = img->pWindow.lock();
                    Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
                    double dx = std::abs(center.x - targetRefX);
                    if (dx < bestDx) {
                        bestDx = dx;
                        prevWin = win;
                    }
                }
                if (prevWin)
                    images[viewportCurrentWorkspace]->lastFocusedWindow = prevWin;
            }
            closeOnWindow = prevWin;
            if (prevWin) {
                Vector2D center = CBox{prevWin->m_realPosition->value() - pMonitor->m_position, prevWin->m_realSize->value()}.middle();
                preferredKbCenterX = center.x;
            }
        }
        updateViewportOffset();
        damage();
    } else if (dir == "left") {
        if (!currentWin) {
            onKbMoveFocus("up");
            return;
        }
        PHLWINDOW bestWin = nullptr;
        double bestScore = std::numeric_limits<double>::max();
        for (const auto& img : images[currentWsIdx]->windowImages) {
            if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                continue;
            PHLWINDOW win = img->pWindow.lock();
            if (win == currentWin)
                continue;

            Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
            double dx = currentLocalCenter.x - center.x;
            double dy = center.y - currentLocalCenter.y;
            if (dx > 20.0) {
                double score = dx + 1.5 * std::abs(dy);
                if (score < bestScore) {
                    bestScore = score;
                    bestWin = win;
                }
            }
        }
        if (!bestWin) {
            double maxX = -1.0;
            for (const auto& img : images[currentWsIdx]->windowImages) {
                if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                    continue;
                PHLWINDOW win = img->pWindow.lock();
                if (win == currentWin)
                    continue;
                Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
                if (center.x > maxX) {
                    maxX = center.x;
                    bestWin = win;
                }
            }
        }
        if (bestWin) {
            closeOnWindow = bestWin;
            images[currentWsIdx]->lastFocusedWindow = bestWin;
            Vector2D center = CBox{bestWin->m_realPosition->value() - pMonitor->m_position, bestWin->m_realSize->value()}.middle();
            preferredKbCenterX = center.x;
        }
        updateViewportOffset();
        damage();
    } else if (dir == "right") {
        if (!currentWin) {
            onKbMoveFocus("down");
            return;
        }
        PHLWINDOW bestWin = nullptr;
        double bestScore = std::numeric_limits<double>::max();
        for (const auto& img : images[currentWsIdx]->windowImages) {
            if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                continue;
            PHLWINDOW win = img->pWindow.lock();
            if (win == currentWin)
                continue;

            Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
            double dx = center.x - currentLocalCenter.x;
            double dy = center.y - currentLocalCenter.y;
            if (dx > 20.0) {
                double score = dx + 1.5 * std::abs(dy);
                if (score < bestScore) {
                    bestScore = score;
                    bestWin = win;
                }
            }
        }
        if (!bestWin) {
            double minX = std::numeric_limits<double>::max();
            for (const auto& img : images[currentWsIdx]->windowImages) {
                if (!img->pWindow || !Desktop::View::validMapped(img->pWindow.lock()))
                    continue;
                PHLWINDOW win = img->pWindow.lock();
                if (win == currentWin)
                    continue;
                Vector2D center = CBox{win->m_realPosition->value() - pMonitor->m_position, win->m_realSize->value()}.middle();
                if (center.x < minX) {
                    minX = center.x;
                    bestWin = win;
                }
            }
        }
        if (bestWin) {
            closeOnWindow = bestWin;
            images[currentWsIdx]->lastFocusedWindow = bestWin;
            Vector2D center = CBox{bestWin->m_realPosition->value() - pMonitor->m_position, bestWin->m_realSize->value()}.middle();
            preferredKbCenterX = center.x;
        }
        updateViewportOffset();
        damage();
    }

    if (closeOnWindow && Desktop::View::validMapped(closeOnWindow.lock())) {
        PHLWINDOW win = closeOnWindow.lock();
        PHLWINDOW oldWin = mouseHoveredWindow ? mouseHoveredWindow.lock() : nullptr;
        mouseHoveredWindow = nullptr;
        if (win->m_workspace && win->m_workspace != pMonitor->m_activeWorkspace) {
            (void)Config::Actions::changeWorkspace(std::to_string(win->m_workspace->m_id));
        }
        Desktop::focusState()->fullWindowFocus(win, Desktop::eFocusReason::FOCUS_REASON_KEYBIND);
        if (oldWin && oldWin != win)
            redrawWindowFor(oldWin);
        redrawWindowFor(win);
    }
    damage();
}
