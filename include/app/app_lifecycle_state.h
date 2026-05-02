#pragma once

#include <3ds.h>

// AppLifecycleState holds minimal lifecycle and APT hook state.
// All members are private with inline getters/setters.
// Designed to be header‑only and trivially copyable.

struct AppLifecycleState {
private:
    bool applet_suspended_ = false;
    bool applet_resume_pending_ = false;
    bool applet_suspend_handled_ = false;
    aptHookCookie apt_hook_cookie_; // initialized via InstallHook
    bool apt_hook_installed_ = false;
    bool shutdown_prepared_ = false;
    bool is_new_3ds_ = false;
    bool is_homebrew_ = false;

public:
    // Default constructor – zero‑initialises all bools.
    AppLifecycleState() = default;

    // Simple getters
    bool IsSuspended() const { return applet_suspended_; }
    bool IsResumePending() const { return applet_resume_pending_; }
    bool IsSuspendHandled() const { return applet_suspend_handled_; }
    bool IsShutdownPrepared() const { return shutdown_prepared_; }
    bool IsNew3DS() const { return is_new_3ds_; }
    bool IsHomebrew() const { return is_homebrew_; }

    // Abort work if suspended or quitting (mode passed as AppMode enum).
    bool ShouldAbortWork(u8 mode) const {
        // Quit mode value is 7 (AppMode::Quit).
        return applet_suspended_ || (mode == 7u);
    }

    // Mutators
    void SetSuspended(bool v) { applet_suspended_ = v; }
    void SetResumePending(bool v) { applet_resume_pending_ = v; }
    void SetSuspendHandled(bool v) { applet_suspend_handled_ = v; }
    void MarkShutdownPrepared() { shutdown_prepared_ = true; }
    void SetNew3DS(bool v) { is_new_3ds_ = v; }
    void SetHomebrew(bool v) { is_homebrew_ = v; }

    // APT hook management
    void InstallHook(aptHookFn callback, void* param) {
        aptHook(&apt_hook_cookie_, callback, param);
        apt_hook_installed_ = true;
    }
    void UninstallHook() {
        if (apt_hook_installed_) {
            aptUnhook(&apt_hook_cookie_);
            apt_hook_installed_ = false;
        }
    }
};
