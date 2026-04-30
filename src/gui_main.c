#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "raylib_gui.h"
#include "cli_battle.h"

/*
 * 函數名稱：main
 * 功能：程式進入點，解析命令列參數後啟動 GUI 主迴圈
 * 用法：dark_chess_gui [-online_battle <room_id>] [-ngui]
 *       -online_battle <room_id>  自動連線至對戰伺服器，以電腦模式加入指定房間
 *       -ngui                     不開啟 GUI 視窗，以命令行模式運行（須搭配 -online_battle）
 * 輸入：argc, argv - 命令列參數
 * 輸出：int - 程式結束代碼（0 為正常結束）
 * 最後更新：2026-04-30 (UTC+8)
 */
int main(int argc, char *argv[])
{
    srand((unsigned int)time(NULL));

    int auto_online = 0;
    int no_gui = 0;
    const char *auto_room_id = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-online_battle") == 0 && i + 1 < argc) {
            auto_online = 1;
            auto_room_id = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "-ngui") == 0) {
            no_gui = 1;
        }
    }

    /* -ngui requires -online_battle */
    if (no_gui && !auto_online) {
        fprintf(stderr, "Error: -ngui requires -online_battle <room_id>\n");
        return 1;
    }

    if (no_gui && auto_room_id) {
        return cli_battle_main(auto_room_id);
    }

    return gui_main(auto_online, auto_room_id);
}
