#pragma once

#include <3ds.h>
#include <atomic>

// AppLifecycleState holds minimal lifecycle and APT hook state.
// All members are private with inline getters/setters.
// Must never be copied or assigned — access via App::lifecycle_state_ only.

struct AppLifecycleState {
private:
    std::atomic<bool> applet_suspended_{false};
    std::atomic<bool> applet_resume_pending_{false};
    std::atomic<bool> applet_suspend_handled_{false};
    aptHookCookie apt_hook_cookie_; // initialized via InstallHook
    bool apt_hook_installed_ = false;
    bool shutdown_prepared_ = false;
    bool is_new_3ds_ = false;
    bool is_homebrew_ = false;

public:
    // Default constructor – zero‑initialises all bools.
    AppLifecycleState() = default;

    // Note: copy and assignment are intentionally not provided.
    // AppLifecycleState must not be copied or assigned — it is a member
    // of App and must only be accessed through App::lifecycle_state_.

    // Getters (cross-thread reads use relaxed ordering)
    bool IsSuspended() const {
        return applet_suspended_.load(std::memory_order_relaxed);
    }
    bool IsResumePending() const {
        return applet_resume_pending_.load(std::memory_order_relaxed);
    }
    bool IsSuspendHandled() const {
        return applet_suspend_handled_.load(std::memory_order_relaxed);
    }
    bool IsShutdownPrepared() const { return shutdown_prepared_; }
    bool IsNew3DS() const { return is_new_3ds_; }
    bool IsHomebrew() const { return is_homebrew_; }

    // Abort work if suspended or quitting (mode passed as AppMode enum).
    bool ShouldAbortWork(u8 mode) const {
        // Quit mode value is 7 (AppMode::Quit).
        return applet_suspended_.load(std::memory_order_relaxed) || (mode == 7u);
    }

    // Mutators (cross-thread writes use relaxed ordering)
    void SetSuspended(bool v) {
        applet_suspended_.store(v, std::memory_order_relaxed);
    }
    void SetResumePending(bool v) {
        applet_resume_pending_.store(v, std::memory_order_relaxed);
    }
    void SetSuspendHandled(bool v) {
        applet_suspend_handled_.store(v, std::memory_order_relaxed);
    }
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
