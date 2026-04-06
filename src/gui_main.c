#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "board.h"
#include "raylib_gui.h"

/*
 * 函數名稱：main
 * 功能：程式進入點，初始化亂數種子後啟動 GUI 主迴圈
 * 輸入：無
 * 輸出：int - 程式結束代碼（0 為正常結束）
 * 最後更新：2026-04-06 15:00 (UTC+8)
 */
int main()
{
    srand((unsigned int)time(NULL));

    return gui_main();
}
