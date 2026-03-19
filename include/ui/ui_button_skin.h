/*
    3dslibris - ui_button_skin.h
    New UI rendering module for Nintendo 3DS port by Rigle.

    Summary:
    - Public API for cached procedural button backgrounds and icons.
    - Defines visual states/icons consumed by Button drawing pipeline.
*/

#pragma once

#include <3ds.h>

enum UiButtonSkinState {
  UI_BUTTON_STATE_NORMAL = 0,
  UI_BUTTON_STATE_PRESSED = 1,
  UI_BUTTON_STATE_SELECTED = 2,
  UI_BUTTON_STATE_DISABLED = 3
};

enum UiButtonIconId {
  UI_BUTTON_ICON_NONE = -1,
  UI_BUTTON_ICON_GEAR = 0,
  UI_BUTTON_ICON_NEXT = 1,
  UI_BUTTON_ICON_PREV = 2,
  UI_BUTTON_ICON_BACK = 3,
  UI_BUTTON_ICON_HOME = 4,
  UI_BUTTON_ICON_COUNT = 5
};

bool UiButtonSkin_Init();
void UiButtonSkin_Exit();
void UiButtonSkin_ResetCache();

UiButtonIconId UiButtonSkin_IconFromLabel(const char *label);
int UiButtonSkin_IconBlockWidth(int buttonHeight);

void UiButtonSkin_Draw(u16 *screen, int stride, int logicalHeight, int x, int y,
                       int w, int h, UiButtonSkinState state, bool withIcon);
void UiButtonSkin_DrawIcon(u16 *screen, int stride, int logicalHeight, int x,
                           int y, int w, int h, UiButtonIconId icon,
                           bool disabled);
