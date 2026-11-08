#pragma once

// Android keycodes the Xperia Play build understands. Circle arrives as BACK
// with scan code 305, which Game.onKeyDown turns into 225.
enum AKey {
    AK_BACK = 4, AK_DPAD_UP = 19, AK_DPAD_DOWN = 20, AK_DPAD_LEFT = 21, AK_DPAD_RIGHT = 22,
    AK_CROSS = 23, AK_MENU = 82, AK_SQUARE = 99, AK_TRIANGLE = 100, AK_L1 = 102, AK_R1 = 103,
    AK_START = 108, AK_SELECT = 109, AK_CIRCLE = 225,
};

// Keys bound to movement instead of an Android key.
enum MoveDir { MOVE_UP = -1, MOVE_DOWN = -2, MOVE_LEFT = -3, MOVE_RIGHT = -4 };
