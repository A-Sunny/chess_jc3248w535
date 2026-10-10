#pragma once
#include <stdint.h>

enum { SIDE_W = 0, SIDE_B = 1 };
enum { PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { F_CAP = 1, F_EP = 2, F_CASTLE = 4, F_DBL = 8 };

#define MAXMV 256
#define MAXH  600

struct Move { uint8_t from, to, promo, flags; };

struct Pos {
  int8_t  b[64];     // index = rank_row*8 + file, row 0 = rank 8 (top). + white, - black
  uint8_t castle;    // 1=WK 2=WQ 4=BK 8=BQ
  int8_t  ep;        // en-passant target square or -1
  uint8_t side;      // side to move
  uint8_t half;      // half-move clock (50-move rule)
};
