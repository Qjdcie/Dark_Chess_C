#ifndef RAYLIB_GUI_H
#define RAYLIB_GUI_H

#include "board.h"

/*
 * 函數名稱：gui_main
 * 功能：遊戲主迴圈，含選擇先手畫面、首翻決定顏色、玩家輸入處理、AI 回合、畫面繪製
 * 輸入：auto_online - 非零表示自動啟動線上對戰（電腦模式）
 *       auto_room_id - 自動加入的房號（NULL 表示不自動加入）
 * 輸出：int - 正常結束回傳 0
 * 最後更新：2026-04-26 (UTC+8)
 */
int gui_main(int auto_online, const char *auto_room_id);

#endif /* RAYLIB_GUI_H */
