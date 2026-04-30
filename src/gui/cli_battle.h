#ifndef CLI_BATTLE_H
#define CLI_BATTLE_H

/*
 * cli_battle_main
 * 功能：不開啟 GUI 視窗，純命令列執行線上對戰（AI 模式）
 * 必須搭配 -online_battle 使用
 * 輸入：room_id — 要加入的房號
 * 輸出：0 = 正常結束，1 = 連線/加入失敗
 */
int cli_battle_main(const char *room_id);

#endif /* CLI_BATTLE_H */
