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
#include "ui/button.h"
#include "menus/menu.h"
#include "settings/font_menu_context.h"
#include "ui/text.h"

enum class AppMode : u8;

class FontMenu : public Menu {
public:
    FontMenu(App* app);
    explicit FontMenu(const FontMenuContext &context);
    ~FontMenu();
    void Open(AppMode requested_mode);
    u16 GetPageCount() const override;
    u16 GetCurrentPage() const override;
    void draw();
    void Draw() override { draw(); } // Override to use the draw method
    void HandleInput(const FrameInput &input) override { handleInput(input); }
    inline const std::vector<std::string>& getFiles() const { return files; }
    void handleInput(const FrameInput &input);
    inline bool isDirty() const { return dirty; }
    inline void setDirty(bool d = true) { dirty = d; }
private:
    enum ViewState {
        VIEW_TARGETS,
        VIEW_FILES
    };

    void findFiles();
    void refreshTargetButtons();
    void enterTargetView(u8 requested_target);
    void enterFileView();
    void handleTargetInput(const FrameInput &input);
    void handleFileInput(const FrameInput &input);
    void handleTargetTouchInput(const FrameInput &input);
    void handleButtonPress();
    void relayoutTargets();
    void relayoutFiles();
    void handleFileTouchInput(const FrameInput &input);
    void nextTargetPage();
    void previousTargetPage();
    u8 getTargetPageCount() const;
    u8 getTargetCurrentPage() const;
    u8 getTargetPageStart() const;
    u8 getTargetPageEnd() const;
    void syncTargetPageToSelection();
    void nextPage();
    void previousPage();
    void selectNext();
    void selectPrevious();
    void selectNextTarget();
    void selectPreviousTarget();
    FontMenuContext context_;
    std::string dir;
    std::vector<std::string> files;
    std::vector<Button *> targetButtons;
    ViewState viewState;
    u8 targetSelected;
    u8 targetPage;
    // Rows per page, paginated by cumulative wrapped height.
    std::vector<int> targetPageSizes;
    std::vector<int> filePageSizes;
};
