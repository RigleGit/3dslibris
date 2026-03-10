/*
    3dslibris - font.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Font selection menu model and state for UI typeface configuration.
    - Integrates available font files with menu navigation and persistence.
*/

#pragma once

#include <3ds.h>
#include <string>
#include <vector>
#include "button.h"
#include "menu.h"
#include "text.h"

class FontMenu : public Menu {
public:
    FontMenu(App* app);
    ~FontMenu();
    void Open(u8 requested_mode);
    void draw();
    void Draw() override { draw(); } // Override to use the draw method
    void HandleInput(u32 keys) override {
        (void)keys;
        handleInput();
    }
    inline const std::vector<std::string>& getFiles() const { return files; }
    void handleInput();
    inline bool isDirty() const { return dirty; }
    inline void setDirty(bool d = true) { dirty = d; }
private:
    enum ViewState {
        VIEW_TARGETS,
        VIEW_FILES
    };

    void findFiles();
    void refreshTargetButtons();
    void enterTargetView(u8 requested_mode);
    void enterFileView();
    void handleTargetInput(u32 keys);
    void handleFileInput(u32 keys);
    void handleTargetTouchInput();
    void handleButtonPress();
    void handleFileTouchInput();
    void nextPage();
    void previousPage();
    void selectNext();
    void selectPrevious();
    void selectNextTarget();
    void selectPreviousTarget();
    std::string dir;
    std::vector<std::string> files;
    std::vector<Button *> targetButtons;
    ViewState viewState;
    u8 targetSelected;
};
