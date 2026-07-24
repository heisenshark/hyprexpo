#include "ScrollOverview.hpp"
#include <any>
#include <vector>
#include <unordered_map>
#include <map>
#include <string>
#include <algorithm>
#include <memory>
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

    auto onCursorMove = [this](Event::SCallbackInfo& info) {
        if (closing)
            return;
        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
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
    enterSubmapIfEnabled();
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

    float      yoff  = -(float)activeIdx * pMonitor->m_size.y * scale->value();
    bool       found = false;
    for (const auto& wimg : images) {
        for (const auto& img : wimg->windowImages) {
            CBox texbox = {img->pWindow->m_realPosition->value() - pMonitor->m_position, img->pWindow->m_realSize->value()};

            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
            texbox.translate({0.F, yoff});

            if (texbox.containsPoint(lastMousePosLocal)) {
                closeOnWindow = img->pWindow;
                found = true;
                break;
            }
        }
        if (found)
            break;
        yoff += pMonitor->m_size.y * scale->value();
    }
}

void CScrollOverview::moveViewportWorkspace(bool up) {
    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    if (viewportCurrentWorkspace == 0 && !up)
        return;
    if (viewportCurrentWorkspace == images.size() - 1 && up)
        return;

    if (up)
        viewportCurrentWorkspace++;
    else
        viewportCurrentWorkspace--;

    *viewOffset = {viewOffset->value().x, (sc<long>(viewportCurrentWorkspace) - sc<long>(activeIdx)) * pMonitor->m_size.y};
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

void CScrollOverview::onDamageReported() {
}

void CScrollOverview::close(bool switchToSelection) {
    if (closing)
        return;

    closing = true;
    resetSubmapIfNeeded();

    if (!closeOnWindow)
        closeOnWindow = Desktop::focusState()->window();

    if (closeOnWindow == Desktop::focusState()->window())
        *viewOffset = Vector2D{};
    else {

        if (closeOnWindow->m_workspace != pMonitor->m_activeWorkspace) {
        (void)Config::Actions::changeWorkspace(std::to_string(closeOnWindow->m_workspace->m_id));
        }

        Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::eFocusReason::FOCUS_REASON_UNKNOWN);

        size_t activeIdx = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
                activeIdx = i;
                break;
            }
        }

        float yoff  = -(float)activeIdx * pMonitor->m_size.y * scale->value();
        bool  found = false;
        for (const auto& wimg : images) {
            for (const auto& img : wimg->windowImages) {
                if (img->pWindow == closeOnWindow && closeOnWindow) {
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
    for (const auto& wimg : images) {
        bool dirty = false;

        for (const auto& img : wimg->windowImages) {
            if (!img->pWindow) {
                dirty = true;
                continue;
            }

            CBox texbox = CBox{img->pWindow->m_realPosition->value() - pMonitor->m_position, pMonitor->m_size};

            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
            texbox.translate({0.F, yoff});

            texbox.scale(pMonitor->m_scale).round();
            CRegion damage{0, 0, INT16_MAX, INT16_MAX};
            Render::GL::g_pHyprOpenGL->renderTextureInternal(img->fb->getTexture(), texbox, Render::GL::CHyprOpenGLImpl::STextureRenderData{.damage = &damage, .a = 1.0f * img->pWindow->m_alpha.value()});

            if (img->highlight) {
                CBox texbox2 = CBox{img->pWindow->m_realPosition->value(), img->pWindow->m_realSize->value()}
                                   .translate(-VIEWPORT_CENTER)
                                   .scale(scale->value())
                                   .translate(VIEWPORT_CENTER)
                                   .translate({0.F, yoff});
                Render::GL::g_pHyprOpenGL->renderRect(texbox2, CHyprColor{0.5, 0.0, 0.0, 0.5}, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 5});
            }
        }
        CBox floatbox = CBox{pMonitor->m_position + Vector2D{0.F, yoff / scale->value()}, pMonitor->m_size};
        floatbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
        floatbox.translate({0.F, yoff});
        floatbox.scale(pMonitor->m_scale).round();
        Render::GL::g_pHyprOpenGL->renderTextureInternal(floatingFb->getTexture(), floatbox, Render::GL::CHyprOpenGLImpl::STextureRenderData{.damage = &damage, .a = 1.0f});

        yoff += pMonitor->m_size.y * scale->value();

        if (dirty)
            std::erase_if(wimg->windowImages, [](const auto& e) { return !e->pWindow; });
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
