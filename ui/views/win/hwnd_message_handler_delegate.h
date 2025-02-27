// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_VIEWS_WIN_HWND_MESSAGE_HANDLER_DELEGATE_H_
#define UI_VIEWS_WIN_HWND_MESSAGE_HANDLER_DELEGATE_H_

#include "ui/views/views_export.h"

namespace gfx {
class Canvas;
class Insets;
class Path;
class Point;
class Size;
}

namespace ui {
class Accelerator;
class KeyEvent;
class MouseEvent;
class TouchEvent;
}

namespace views {

class InputMethod;

// Implemented by the object that uses the HWNDMessageHandler to handle
// notifications from the underlying HWND and service requests for data.
class VIEWS_EXPORT HWNDMessageHandlerDelegate {
 public:
  virtual bool IsWidgetWindow() const = 0;

  // TODO(beng): resolve this more satisfactorily vis-a-vis ShouldUseNativeFrame
  //             to avoid confusion.
  virtual bool IsUsingCustomFrame() const = 0;

  virtual void SchedulePaint() = 0;
  virtual void EnableInactiveRendering() = 0;
  virtual bool IsInactiveRenderingDisabled() = 0;

  virtual bool CanResize() const = 0;
  virtual bool CanMaximize() const = 0;
  virtual bool CanMinimize() const = 0;
  virtual bool CanActivate() const = 0;

  virtual bool WidgetSizeIsClientSize() const = 0;

  // Returns true if the delegate represents a modal window.
  virtual bool IsModal() const = 0;

  // Returns the show state that should be used for the application's first
  // window.
  virtual int GetInitialShowState() const = 0;

  virtual bool WillProcessWorkAreaChange() const = 0;

  virtual int GetNonClientComponent(const gfx::Point& point) const = 0;
  virtual void GetWindowMask(const gfx::Size& size, gfx::Path* mask) = 0;

  // Returns true if the delegate modifies |insets| to define a custom client
  // area for the window, false if the default client area should be used. If
  // false is returned, |insets| is not modified.
  virtual bool GetClientAreaInsets(gfx::Insets* insets) const = 0;

  // Returns the minimum and maximum size the window can be resized to by the
  // user.
  virtual void GetMinMaxSize(gfx::Size* min_size,
                             gfx::Size* max_size) const = 0;

  // Returns the current size of the RootView.
  virtual gfx::Size GetRootViewSize() const = 0;

  virtual void ResetWindowControls() = 0;

  virtual gfx::NativeViewAccessible GetNativeViewAccessible() = 0;

  // Returns true if the window should handle standard system commands, such as
  // close, minimize, maximize.
  // TODO(benwells): Remove this once bubbles don't have two widgets
  // implementing them on non-aura windows. http://crbug.com/189112.
  virtual bool ShouldHandleSystemCommands() const = 0;

  // TODO(beng): Investigate migrating these methods to On* prefixes once
  // HWNDMessageHandler is the WindowImpl.

  // Called when another app was activated.
  virtual void HandleAppDeactivated() = 0;

  // Called when the window was activated or deactivated. |active| reflects the
  // new state.
  virtual void HandleActivationChanged(bool active) = 0;

  // Called when a well known "app command" from the system was performed.
  // Returns true if the command was handled.
  virtual bool HandleAppCommand(short command) = 0;

  // Called from WM_CANCELMODE.
  virtual void HandleCancelMode() = 0;

  // Called when the window has lost mouse capture.
  virtual void HandleCaptureLost() = 0;

  // Called when the user tried to close the window.
  virtual void HandleClose() = 0;

  // Called when a command defined by the application was performed. Returns
  // true if the command was handled.
  virtual bool HandleCommand(int command) = 0;

  // Called when an accelerator is invoked.
  virtual void HandleAccelerator(const ui::Accelerator& accelerator) = 0;

  // Called when the HWND is created.
  virtual void HandleCreate() = 0;

  // Called when the HWND is being destroyed, before any child HWNDs are
  // destroyed.
  virtual void HandleDestroying() = 0;

  // Called after the HWND is destroyed, after all child HWNDs have been
  // destroyed.
  virtual void HandleDestroyed() = 0;

  // Called when the HWND is to be focused for the first time. This is called
  // when the window is shown for the first time. Returns true if the delegate
  // set focus and no default processing should be done by the message handler.
  virtual bool HandleInitialFocus(ui::WindowShowState show_state) = 0;

  // Called when display settings are adjusted on the system.
  virtual void HandleDisplayChange() = 0;

  // Called when the user begins or ends a size/move operation using the window
  // manager.
  virtual void HandleBeginWMSizeMove() = 0;
  virtual void HandleEndWMSizeMove() = 0;

  // Called when the window's position changed.
  virtual void HandleMove() = 0;

  // Called when the system's work area has changed.
  virtual void HandleWorkAreaChanged() = 0;

  // Called when the window's visibility is changing. |visible| holds the new
  // state.
  virtual void HandleVisibilityChanging(bool visible) = 0;

  // Called when the window's visibility changed. |visible| holds the new state.
  virtual void HandleVisibilityChanged(bool visible) = 0;

  // Called when the "calculated" window's visibility changed.
  // OS doesn't mark windows hidden on screen lock or when other opaque windows
  // cover them.
  // |visible| holds the new state.
  virtual void HandleSoftVisibilityChanged(bool visible) = 0;

  // Called when the window's client size changed. |new_size| holds the new
  // size.
  virtual void HandleClientSizeChanged(const gfx::Size& new_size) = 0;

  // Called when the window's frame has changed.
  virtual void HandleFrameChanged() = 0;

  // Called when focus shifted to this HWND from |last_focused_window|.
  virtual void HandleNativeFocus(HWND last_focused_window) = 0;

  // Called when focus shifted from the HWND to a different window.
  virtual void HandleNativeBlur(HWND focused_window) = 0;

  // Called when a mouse event is received. Returns true if the event was
  // handled by the delegate.
  virtual bool HandleMouseEvent(const ui::MouseEvent& event) = 0;

  // Called when an untranslated key event is received (i.e. pre-IME
  // translation).
  virtual void HandleKeyEvent(ui::KeyEvent* event) = 0;

  // Called when a touch event is received.
  virtual void HandleTouchEvent(const ui::TouchEvent& event) = 0;

  // Called when an IME message needs to be processed by the delegate. Returns
  // true if the event was handled and no default processing should be
  // performed.
  virtual bool HandleIMEMessage(UINT message,
                                WPARAM w_param,
                                LPARAM l_param,
                                LRESULT* result) = 0;

  // Called when the system input language changes.
  virtual void HandleInputLanguageChange(DWORD character_set,
                                         HKL input_language_id) = 0;

  // Called to compel the delegate to paint |invalid_rect| accelerated.
  virtual void HandlePaintAccelerated(const gfx::Rect& invalid_rect) = 0;

  // Called to forward a WM_NOTIFY message to the tooltip manager.
  virtual bool HandleTooltipNotify(int w_param,
                                   NMHDR* l_param,
                                   LRESULT* l_result) = 0;

  // Invoked on entering/exiting a menu loop.
  virtual void HandleMenuLoop(bool in_menu_loop) = 0;

  // Catch-all message handling and filtering. Called before
  // HWNDMessageHandler's built-in handling, which may pre-empt some
  // expectations in Views/Aura if messages are consumed. Returns true if the
  // message was consumed by the delegate and should not be processed further
  // by the HWNDMessageHandler. In this case, |result| is returned. |result| is
  // not modified otherwise.
  virtual bool PreHandleMSG(UINT message,
                            WPARAM w_param,
                            LPARAM l_param,
                            LRESULT* result) = 0;

  // Like PreHandleMSG, but called after HWNDMessageHandler's built-in handling
  // has run and after DefWindowProc.
  virtual void PostHandleMSG(UINT message,
                             WPARAM w_param,
                             LPARAM l_param) = 0;

  // Called when a scroll event is received. Returns true if the event was
  // handled by the delegate.
  virtual bool HandleScrollEvent(const ui::ScrollEvent& event) = 0;

  // Called when the window size is about to change.
  virtual void HandleWindowSizeChanging() = 0;

  // Called when the window size has finished changing.
  virtual void HandleWindowSizeChanged() = 0;

 protected:
  virtual ~HWNDMessageHandlerDelegate() {}
};

}  // namespace views

#endif  // UI_VIEWS_WIN_HWND_MESSAGE_HANDLER_DELEGATE_H_
