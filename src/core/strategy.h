#ifndef STRATEGY_H
#define STRATEGY_H

#include "board.h"

/* =====================================================================
 *  策略系統（Strategy System）
 *
 *  對外介面只有兩個函數：
 *    strategy_select_move() — AI 選擇走法（每回合呼叫）
 *    strategy_reset()       — 重置內部狀態（新遊戲時呼叫）
 * ===================================================================== */

/*
 * 函數名稱：strategy_select_move
 * 功能：AI 主策略函數 — 綜合算牌、情勢評估、啟發式評分與搜尋，選出最佳走法
 * 輸入：gs - 指向當前遊戲狀態的常數指標, out_move - 儲存所選走法的指標
 * 輸出：int - 成功選擇走法回傳 1，無合法走法回傳 0
 * 最後更新：2026-04-02 23:30 (UTC+8)
 */
int strategy_select_move(const GameState *gs, Move *out_move);

/*
 * 函數名稱：strategy_reset
 * 功能：重置策略系統的所有內部狀態（算牌、追逐歷史、置換表等），新遊戲時呼叫
 * 輸入：無
 * 輸出：無
 * 最後更新：2026-04-02 23:30 (UTC+8)
 */
void strategy_reset(void);

/*
 * 函數名稱：strategy_is_draw_mode
 * 功能：回傳 AI 當前是否處於平局模式（我方無法吃任何敵子）
 * 輸入：無
 * 輸出：int - 1 = draw_mode 中，0 = 正常模式
 */
int strategy_is_draw_mode(void);

#endif /* STRATEGY_H */
