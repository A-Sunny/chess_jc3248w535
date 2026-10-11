/*
 * Chess for Guition JC3248W535 (ESP32-S3, 320x480 QSPI display, AXS15231B + capacitive touch)
 *
 * Features: full rules (castling, en passant, promotion, check/checkmate, stalemate,
 * 50-move rule, threefold repetition, insufficient material), 1-player vs AI
 * (3 difficulty levels, choose colour) and 2-player pass-and-play, undo, flip board.
 *
 * Arduino IDE settings:
 *   Board: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
 * Library: GFX Library for Arduino (Arduino_GFX) - latest version (needs the AXS15231B driver)
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_random.h>
#include <Arduino_GFX_Library.h>
#include "chess_types.h"
#include "chess_pieces.h"
#include "chess_audio.h"

// ============================ HARDWARE CONFIG ============================
#define GFX_BL        1      // backlight pin
#define TOUCH_SDA     4
#define TOUCH_SCL     8
#define TOUCH_ADDR    0x3B

// If taps land in the wrong place, change these (see TOUCH_DEBUG output on Serial)
#define TOUCH_SWAP_XY 0
#define TOUCH_INV_X   0
#define TOUCH_INV_Y   0
#define TOUCH_DEBUG   0

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_AXS15231B *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED /*RST*/, 0 /*rotation*/, false /*IPS*/, 320, 480);
Arduino_Canvas *gfx = new Arduino_Canvas(320, 480, panel, 0, 0, 0);   // framebuffer lives in PSRAM

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ============================ COLOURS ============================
const uint16_t C_LIGHT   = rgb(240, 217, 181);
const uint16_t C_DARK    = rgb(181, 136, 99);
const uint16_t C_LAST_L  = rgb(205, 210, 106);
const uint16_t C_LAST_D  = rgb(170, 162, 58);
const uint16_t C_SEL     = rgb(246, 246, 105);
const uint16_t C_CHECK   = rgb(235, 90, 80);
const uint16_t C_MARK    = rgb(60, 140, 70);
const uint16_t C_BG      = rgb(24, 26, 32);
const uint16_t C_BAR     = rgb(38, 41, 50);
const uint16_t C_TXT     = rgb(235, 235, 235);
const uint16_t C_TITLE   = rgb(240, 190, 80);
const uint16_t C_RED     = rgb(255, 110, 100);
const uint16_t C_BTN     = rgb(62, 68, 84);
const uint16_t C_BTN_SEL = rgb(60, 130, 200);
const uint16_t C_GO      = rgb(50, 160, 90);
const uint16_t C_PANEL   = rgb(45, 48, 60);
const uint16_t C_WFILL   = rgb(250, 250, 245);
const uint16_t C_WEDGE   = rgb(20, 20, 20);
const uint16_t C_BFILL   = rgb(45, 45, 52);
const uint16_t C_BEDGE   = rgb(235, 235, 235);

// ============================ CHESS CORE ============================
// (types and enums live in chess_types.h)

static const int8_t KN_D[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
static const int8_t K_D[8][2]  = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
static const int8_t R_D[4][2]  = {{-1,0},{1,0},{0,-1},{0,1}};
static const int8_t B_D[4][2]  = {{-1,-1},{-1,1},{1,-1},{1,1}};

static void setupStart(Pos &p) {
  static const int8_t back[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
  memset(&p, 0, sizeof(p));
  for (int f = 0; f < 8; f++) {
    p.b[f] = -back[f];
    p.b[8 + f] = -PAWN;
    p.b[48 + f] = PAWN;
    p.b[56 + f] = back[f];
  }
  p.castle = 15; p.ep = -1; p.side = SIDE_W; p.half = 0;
}

static inline bool onBoard(int r, int f) { return r >= 0 && r < 8 && f >= 0 && f < 8; }

static bool attacked(const Pos &p, int sq, int by) {
  const int r = sq >> 3, f = sq & 7;
  const int s = (by == SIDE_W) ? 1 : -1;
  int pr = r + (by == SIDE_W ? 1 : -1);
  if (pr >= 0 && pr < 8) {
    if (f > 0 && p.b[pr * 8 + f - 1] == s * PAWN) return true;
    if (f < 7 && p.b[pr * 8 + f + 1] == s * PAWN) return true;
  }
  for (int i = 0; i < 8; i++) {
    int rr = r + KN_D[i][0], ff = f + KN_D[i][1];
    if (onBoard(rr, ff) && p.b[rr * 8 + ff] == s * KNIGHT) return true;
    rr = r + K_D[i][0]; ff = f + K_D[i][1];
    if (onBoard(rr, ff) && p.b[rr * 8 + ff] == s * KING) return true;
  }
  for (int i = 0; i < 4; i++) {
    int rr = r + R_D[i][0], ff = f + R_D[i][1];
    while (onBoard(rr, ff)) {
      int v = p.b[rr * 8 + ff];
      if (v) { if (v == s * ROOK || v == s * QUEEN) return true; break; }
      rr += R_D[i][0]; ff += R_D[i][1];
    }
    rr = r + B_D[i][0]; ff = f + B_D[i][1];
    while (onBoard(rr, ff)) {
      int v = p.b[rr * 8 + ff];
      if (v) { if (v == s * BISHOP || v == s * QUEEN) return true; break; }
      rr += B_D[i][0]; ff += B_D[i][1];
    }
  }
  return false;
}

static bool inCheck(const Pos &p, int side) {
  int k = (side == SIDE_W) ? KING : -KING;
  for (int i = 0; i < 64; i++) if (p.b[i] == k) return attacked(p, i, 1 - side);
  return false;
}

static inline void addMv(Move *out, int &n, int from, int to, int flags, int promo = 0) {
  out[n].from = from; out[n].to = to; out[n].flags = flags; out[n].promo = promo; n++;
}

static void slide(const Pos &p, Move *out, int &n, int sq, const int8_t d[][2], int nd) {
  const bool white = p.b[sq] > 0;
  const int r = sq >> 3, f = sq & 7;
  for (int i = 0; i < nd; i++) {
    int rr = r + d[i][0], ff = f + d[i][1];
    while (onBoard(rr, ff)) {
      int to = rr * 8 + ff, tp = p.b[to];
      if (!tp) addMv(out, n, sq, to, 0);
      else { if ((tp > 0) != white) addMv(out, n, sq, to, F_CAP); break; }
      rr += d[i][0]; ff += d[i][1];
    }
  }
}

// pseudo-legal moves (castling already checks attacked squares)
static int genMoves(const Pos &p, Move *out) {
  int n = 0;
  const int me = p.side;
  const bool white = (me == SIDE_W);
  for (int sq = 0; sq < 64; sq++) {
    int pc = p.b[sq];
    if (!pc || (pc > 0) != white) continue;
    int t = abs(pc), r = sq >> 3, f = sq & 7;
    if (t == PAWN) {
      int dir = white ? -1 : 1, startR = white ? 6 : 1, promoR = white ? 0 : 7;
      int nr = r + dir;
      if (nr < 0 || nr > 7) continue;
      int to = nr * 8 + f;
      if (!p.b[to]) {
        if (nr == promoR) { for (int pr = QUEEN; pr >= KNIGHT; pr--) addMv(out, n, sq, to, 0, pr); }
        else {
          addMv(out, n, sq, to, 0);
          if (r == startR && !p.b[to + dir * 8]) addMv(out, n, sq, to + dir * 8, F_DBL);
        }
      }
      for (int df = -1; df <= 1; df += 2) {
        int nf = f + df;
        if (nf < 0 || nf > 7) continue;
        int t2 = nr * 8 + nf, tp = p.b[t2];
        if (tp && ((tp > 0) != white)) {
          if (nr == promoR) { for (int pr = QUEEN; pr >= KNIGHT; pr--) addMv(out, n, sq, t2, F_CAP, pr); }
          else addMv(out, n, sq, t2, F_CAP);
        } else if (!tp && t2 == p.ep) {
          addMv(out, n, sq, t2, F_EP | F_CAP);
        }
      }
    } else if (t == KNIGHT || t == KING) {
      for (int i = 0; i < 8; i++) {
        int rr = r + (t == KNIGHT ? KN_D[i][0] : K_D[i][0]);
        int ff = f + (t == KNIGHT ? KN_D[i][1] : K_D[i][1]);
        if (!onBoard(rr, ff)) continue;
        int to = rr * 8 + ff, tp = p.b[to];
        if (!tp) addMv(out, n, sq, to, 0);
        else if ((tp > 0) != white) addMv(out, n, sq, to, F_CAP);
      }
      if (t == KING) {
        if (white && sq == 60) {
          if ((p.castle & 1) && !p.b[61] && !p.b[62] && p.b[63] == ROOK &&
              !attacked(p, 60, SIDE_B) && !attacked(p, 61, SIDE_B) && !attacked(p, 62, SIDE_B))
            addMv(out, n, 60, 62, F_CASTLE);
          if ((p.castle & 2) && !p.b[59] && !p.b[58] && !p.b[57] && p.b[56] == ROOK &&
              !attacked(p, 60, SIDE_B) && !attacked(p, 59, SIDE_B) && !attacked(p, 58, SIDE_B))
            addMv(out, n, 60, 58, F_CASTLE);
        } else if (!white && sq == 4) {
          if ((p.castle & 4) && !p.b[5] && !p.b[6] && p.b[7] == -ROOK &&
              !attacked(p, 4, SIDE_W) && !attacked(p, 5, SIDE_W) && !attacked(p, 6, SIDE_W))
            addMv(out, n, 4, 6, F_CASTLE);
          if ((p.castle & 8) && !p.b[3] && !p.b[2] && !p.b[1] && p.b[0] == -ROOK &&
              !attacked(p, 4, SIDE_W) && !attacked(p, 3, SIDE_W) && !attacked(p, 2, SIDE_W))
            addMv(out, n, 4, 2, F_CASTLE);
        }
      }
    } else {
      if (t == ROOK || t == QUEEN) slide(p, out, n, sq, R_D, 4);
      if (t == BISHOP || t == QUEEN) slide(p, out, n, sq, B_D, 4);
    }
  }
  return n;
}

static void makeMove(Pos &p, const Move &m) {
  int pc = p.b[m.from];
  int t = abs(pc), sgn = pc > 0 ? 1 : -1;
  bool cap = p.b[m.to] != 0 || (m.flags & F_EP);
  p.b[m.to] = m.promo ? sgn * m.promo : pc;
  p.b[m.from] = 0;
  if (m.flags & F_EP) p.b[m.to + (sgn > 0 ? 8 : -8)] = 0;
  if (m.flags & F_CASTLE) {
    if (m.to == 62) { p.b[61] = p.b[63]; p.b[63] = 0; }
    else if (m.to == 58) { p.b[59] = p.b[56]; p.b[56] = 0; }
    else if (m.to == 6)  { p.b[5] = p.b[7]; p.b[7] = 0; }
    else if (m.to == 2)  { p.b[3] = p.b[0]; p.b[0] = 0; }
  }
  const int sqs[6]  = {60, 63, 56, 4, 7, 0};
  const uint8_t clr[6] = {3, 1, 2, 12, 4, 8};
  for (int i = 0; i < 6; i++) if (m.from == sqs[i] || m.to == sqs[i]) p.castle &= ~clr[i];
  p.ep = (m.flags & F_DBL) ? (m.from + m.to) / 2 : -1;
  p.half = (t == PAWN || cap) ? 0 : p.half + 1;
  p.side ^= 1;
}

static int genLegal(const Pos &p, Move *out) {
  Move tmp[MAXMV];
  int n = genMoves(p, tmp), k = 0;
  for (int i = 0; i < n; i++) {
    Pos c = p; makeMove(c, tmp[i]);
    if (!inCheck(c, p.side)) out[k++] = tmp[i];
  }
  return k;
}

static bool samePos(const Pos &a, const Pos &b) {
  return memcmp(a.b, b.b, 64) == 0 && a.side == b.side && a.castle == b.castle && a.ep == b.ep;
}

static bool insufficient(const Pos &p) {
  int minors = 0, nn = 0, colorMask = 0;
  for (int i = 0; i < 64; i++) {
    int t = abs(p.b[i]);
    if (t == PAWN || t == ROOK || t == QUEEN) return false;
    if (t == KNIGHT) { minors++; nn++; }
    if (t == BISHOP) { minors++; colorMask |= 1 << (((i >> 3) + (i & 7)) & 1); }
  }
  if (minors <= 1) return true;
  if (nn == 0 && (colorMask == 1 || colorMask == 2)) return true;
  return false;
}

// ============================ EVALUATION + SEARCH ============================
static const int PV[7] = {0, 100, 320, 330, 500, 900, 0};
static const int OV[7] = {0, 1, 3, 3, 5, 9, 20};

static const int8_t PST_P[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
  50, 50, 50, 50, 50, 50, 50, 50,
  10, 10, 20, 30, 30, 20, 10, 10,
   5,  5, 10, 25, 25, 10,  5,  5,
   0,  0,  0, 20, 20,  0,  0,  0,
   5, -5,-10,  0,  0,-10, -5,  5,
   5, 10, 10,-20,-20, 10, 10,  5,
   0,  0,  0,  0,  0,  0,  0,  0};
static const int8_t PST_N[64] = {
 -50,-40,-30,-30,-30,-30,-40,-50,
 -40,-20,  0,  0,  0,  0,-20,-40,
 -30,  0, 10, 15, 15, 10,  0,-30,
 -30,  5, 15, 20, 20, 15,  5,-30,
 -30,  0, 15, 20, 20, 15,  0,-30,
 -30,  5, 10, 15, 15, 10,  5,-30,
 -40,-20,  0,  5,  5,  0,-20,-40,
 -50,-40,-30,-30,-30,-30,-40,-50};
static const int8_t PST_B[64] = {
 -20,-10,-10,-10,-10,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5, 10, 10,  5,  0,-10,
 -10,  5,  5, 10, 10,  5,  5,-10,
 -10,  0, 10, 10, 10, 10,  0,-10,
 -10, 10, 10, 10, 10, 10, 10,-10,
 -10,  5,  0,  0,  0,  0,  5,-10,
 -20,-10,-10,-10,-10,-10,-10,-20};
static const int8_t PST_R[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
   5, 10, 10, 10, 10, 10, 10,  5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
   0,  0,  0,  5,  5,  0,  0,  0};
static const int8_t PST_Q[64] = {
 -20,-10,-10, -5, -5,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5,  5,  5,  5,  0,-10,
  -5,  0,  5,  5,  5,  5,  0, -5,
   0,  0,  5,  5,  5,  5,  0, -5,
 -10,  5,  5,  5,  5,  5,  0,-10,
 -10,  0,  5,  0,  0,  0,  0,-10,
 -20,-10,-10, -5, -5,-10,-10,-20};
static const int8_t PST_K[64] = {
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -20,-30,-30,-40,-40,-30,-30,-20,
 -10,-20,-20,-20,-20,-20,-20,-10,
  20, 20,  0,  0,  0,  0, 20, 20,
  20, 30, 10,  0,  0, 10, 30, 20};
static const int8_t PST_KE[64] = {
 -50,-40,-30,-20,-20,-30,-40,-50,
 -30,-20,-10,  0,  0,-10,-20,-30,
 -30,-10, 20, 30, 30, 20,-10,-30,
 -30,-10, 30, 40, 40, 30,-10,-30,
 -30,-10, 30, 40, 40, 30,-10,-30,
 -30,-10, 20, 30, 30, 20,-10,-30,
 -30,-30,  0,  0,  0,  0,-30,-30,
 -50,-30,-30,-30,-30,-30,-30,-50};
static const int8_t *const PSTS[7] = {nullptr, PST_P, PST_N, PST_B, PST_R, PST_Q, nullptr};

// score from the point of view of the side to move
static int evaluate(const Pos &p) {
  int np = 0;
  for (int i = 0; i < 64; i++) { int t = abs(p.b[i]); if (t > PAWN && t < KING) np += PV[t]; }
  bool eg = np < 2600;
  int score = 0;
  for (int i = 0; i < 64; i++) {
    int pc = p.b[i];
    if (!pc) continue;
    int t = abs(pc), idx = pc > 0 ? i : (i ^ 56);
    int v = PV[t];
    if (t == KING) v += eg ? PST_KE[idx] : PST_K[idx];
    else v += PSTS[t][idx];
    score += pc > 0 ? v : -v;
  }
  return p.side == SIDE_W ? score : -score;
}

#define INF  32000
#define MATE 30000

static volatile bool timeUp = false;
static uint32_t deadline = 0, nodes = 0;

static inline bool checkTime() {
  if ((++nodes & 2047) == 0) {
    if (millis() > deadline) timeUp = true;
    vTaskDelay(1);
  }
  return timeUp;
}

static int orderScore(const Pos &p, const Move &m) {
  int s = 0;
  int victim = (m.flags & F_EP) ? PAWN : abs(p.b[m.to]);
  if (victim) s = 1000 + 100 * OV[victim] - OV[abs(p.b[m.from])];
  if (m.promo) s += 800 + OV[m.promo] * 10;
  return s;
}

static inline void pickBest(Move *mv, int16_t *sc, int i, int n) {
  int bi = i;
  for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
  if (bi != i) {
    Move t = mv[i]; mv[i] = mv[bi]; mv[bi] = t;
    int16_t s = sc[i]; sc[i] = sc[bi]; sc[bi] = s;
  }
}

static int qsearch(const Pos &p, int alpha, int beta, int qd) {
  if (checkTime()) return 0;
  int stand = evaluate(p);
  if (stand >= beta) return beta;
  if (stand > alpha) alpha = stand;
  if (qd <= 0) return alpha;
  Move mv[MAXMV]; int16_t sc[MAXMV];
  int n = genMoves(p, mv), k = 0;
  for (int i = 0; i < n; i++)
    if ((mv[i].flags & F_CAP) || mv[i].promo) { mv[k] = mv[i]; sc[k] = orderScore(p, mv[i]); k++; }
  for (int i = 0; i < k; i++) {
    pickBest(mv, sc, i, k);
    Pos c = p; makeMove(c, mv[i]);
    if (inCheck(c, p.side)) continue;
    int s = -qsearch(c, -beta, -alpha, qd - 1);
    if (timeUp) return 0;
    if (s >= beta) return beta;
    if (s > alpha) alpha = s;
  }
  return alpha;
}

static int negamax(const Pos &p, int depth, int alpha, int beta, int ply) {
  if (checkTime()) return 0;
  if (p.half >= 100) return 0;
  bool chk = inCheck(p, p.side);
  if (chk && ply < 8) depth++;
  if (depth <= 0) return qsearch(p, alpha, beta, 4);
  Move mv[MAXMV]; int16_t sc[MAXMV];
  int n = genMoves(p, mv);
  for (int i = 0; i < n; i++) sc[i] = orderScore(p, mv[i]);
  int legalCnt = 0;
  for (int i = 0; i < n; i++) {
    pickBest(mv, sc, i, n);
    Pos c = p; makeMove(c, mv[i]);
    if (inCheck(c, p.side)) continue;
    legalCnt++;
    int s = -negamax(c, depth - 1, -beta, -alpha, ply + 1);
    if (timeUp) return 0;
    if (s >= beta) return beta;
    if (s > alpha) alpha = s;
  }
  if (!legalCnt) return chk ? -MATE + ply : 0;
  return alpha;
}

// ============================ GAME STATE ============================
enum { ST_MENU, ST_PLAY, ST_PROMO, ST_AI, ST_OVER, ST_SOUND };
static int  state = ST_MENU;
static int  mode = 1;                // 1 = vs computer, 2 = two players
static int  humanSide = SIDE_W;
static int  level = 1;               // 0 easy, 1 medium, 2 hard
static bool autoFlip = false;        // 2-player: rotate view to the side to move
static bool flipped = false;

static Pos  hist[MAXH + 1];
static Move mvHist[MAXH];
static int  histN = 0;
#define CUR hist[histN]

static Move legalMv[MAXMV];
static int  nLegal = 0;
static int  sel = -1;
static bool tgt[64];
static int  gameResult = 0;          // 0 none,1 W mates,2 B mates,3 stalemate,4 50-move,5 repetition,6 material
static bool inChk = false;
static int  promoFrom = -1, promoTo = -1;

// chess clock (2-player mode only)
static const int CLOCK_OPTS[5] = {0, 3, 5, 10, 15};   // minutes per player, 0 = off
static int      clockIdx = 0;
static int32_t  timeLeft[2] = {0, 0};                 // ms
static uint32_t lastTick = 0;
static inline bool clockOn() { return mode == 2 && CLOCK_OPTS[clockIdx] > 0; }

// sound volumes, 0 (off) .. 10 (saved in flash)
static int musicVol = 8, sfxVol = 8;
static int soundPrev = ST_MENU;          // screen to return to from the sound settings
static void sfx(int id) { if (sfxVol > 0) audioSfx(id); }

struct Lvl { int depth; uint32_t ms; int noise; };
static const Lvl LV[3] = {{2, 800, 60}, {4, 2500, 0}, {7, 7000, 0}};

// ---- root search (runs inside its own task) ----
static Move searchBest(const Pos &root, int maxDepth, uint32_t ms, int noise) {
  Move mv[MAXMV]; int16_t sc[MAXMV];
  int n = genLegal(root, mv);
  Move best = mv[0];
  if (n <= 1) return best;
  deadline = millis() + ms; timeUp = false; nodes = 0;
  for (int i = 0; i < n; i++) sc[i] = orderScore(root, mv[i]);
  for (int i = 0; i < n; i++) pickBest(mv, sc, i, n);

  for (int d = 1; d <= maxDepth; d++) {
    int alpha = -INF, bestS = -INF, bi = 0;
    bool ok = true;
    for (int i = 0; i < n; i++) {
      Pos c = root; makeMove(c, mv[i]);
      int s;
      int rep = 0;
      for (int j = 0; j <= histN; j++) if (samePos(hist[j], c)) rep++;
      if (rep >= 2) {
        s = 0;                                   // would be a threefold repetition
      } else {
        int a = noise ? -INF : alpha;
        s = -negamax(c, d - 1, -INF, -a, 1);
        if (timeUp) { ok = false; break; }
      }
      if (noise) s += (int)(esp_random() % (2 * noise + 1)) - noise;
      if (s > bestS) { bestS = s; bi = i; }
      if (!noise && s > alpha) alpha = s;
    }
    if (!ok) break;
    best = mv[bi];
    Move t = mv[bi];
    for (int k = bi; k > 0; k--) mv[k] = mv[k - 1];
    mv[0] = t;
    if (bestS > MATE - 200) break;               // forced mate found
  }
  return best;
}

static volatile bool aiDone = false;
static Move aiMove;
static Pos  aiPos;

static void aiTask(void *) {
  const Lvl &L = LV[level];
  Move m = searchBest(aiPos, L.depth, L.ms, L.noise);
  aiMove = m;
  __sync_synchronize();
  aiDone = true;
  vTaskDelete(NULL);
}

// ============================ DRAWING ============================
#define BOARD_Y 40
#define SQ      40

static void textCentered(const char *s, int cx, int y, int size, uint16_t col) {
  int w = strlen(s) * 6 * size - size;
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(cx - w / 2, y);
  gfx->print(s);
}

static void drawBtn(int x, int y, int w, int h, const char *label, bool selected, uint16_t selCol = C_BTN_SEL) {
  gfx->fillRoundRect(x, y, w, h, 8, selected ? selCol : C_BTN);
  textCentered(label, x + w / 2, y + (h - 14) / 2, 2, C_TXT);
}

static inline bool inR(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// piece sprite colours: outline, fill, detail
static const uint16_t SPR_W_OUT = rgb(20, 20, 20),  SPR_W_FILL = rgb(250, 250, 245), SPR_W_DET = rgb(20, 20, 20);
static const uint16_t SPR_B_OUT = rgb(12, 12, 14),  SPR_B_FILL = rgb(60, 60, 68),    SPR_B_DET = rgb(230, 230, 230);

// draws a 36x36 2-bit sprite centred on (cx, cy), one horizontal run at a time
static void drawPiece(int pc, int cx, int cy) {
  int t = abs(pc);
  bool w = pc > 0;
  const uint8_t *spr = PIECE_BMP[t - 1];
  const uint16_t cOut  = w ? SPR_W_OUT  : SPR_B_OUT;
  const uint16_t cFill = w ? SPR_W_FILL : SPR_B_FILL;
  const uint16_t cDet  = w ? SPR_W_DET  : SPR_B_DET;
  int x0 = cx - PIECE_W / 2, y0 = cy - PIECE_H / 2;
  for (int y = 0; y < PIECE_H; y++) {
    const uint8_t *row = spr + y * PIECE_STRIDE;
    int runCode = 0, start = 0;
    for (int x = 0; x <= PIECE_W; x++) {
      int code = (x < PIECE_W) ? ((row[x >> 2] >> ((x & 3) * 2)) & 3) : -1;
      if (code != runCode) {
        if (runCode > 0) {
          uint16_t col = (runCode == 1) ? cOut : (runCode == 2 ? cFill : cDet);
          gfx->drawFastHLine(x0 + start, y0 + y, x - start, col);
        }
        runCode = code;
        start = x;
      }
    }
  }
}

// small speaker icon (top-right): X = off, one wave = effects only, two waves = music + effects
static void drawSoundIcon() {
  int x = 288, y = 8;
  gfx->fillRect(x, y + 9, 5, 8, C_TXT);
  gfx->fillTriangle(x + 5, y + 9, x + 12, y + 3, x + 12, y + 23, C_TXT);
  gfx->fillTriangle(x + 5, y + 9, x + 12, y + 23, x + 5, y + 17, C_TXT);
  if (musicVol == 0 && sfxVol == 0) {
    gfx->drawLine(x + 16, y + 9, x + 24, y + 17, C_RED);
    gfx->drawLine(x + 24, y + 9, x + 16, y + 17, C_RED);
  } else {
    gfx->drawLine(x + 16, y + 9, x + 18, y + 13, C_TXT);
    gfx->drawLine(x + 18, y + 13, x + 16, y + 17, C_TXT);
    if (musicVol > 0 && sfxVol > 0) {
      gfx->drawLine(x + 20, y + 6, x + 23, y + 13, C_TXT);
      gfx->drawLine(x + 23, y + 13, x + 20, y + 20, C_TXT);
    }
  }
}

static void drawStatus() {
  gfx->fillRect(0, 0, 320, BOARD_Y, C_BAR);
  char buf[40];
  uint16_t col = C_TXT;
  if (gameResult) {
    col = C_TITLE;
    switch (gameResult) {
      case 1: case 2: {
        bool humanWon = (mode == 1) && ((gameResult == 1 && humanSide == SIDE_W) || (gameResult == 2 && humanSide == SIDE_B));
        if (mode == 1) strcpy(buf, humanWon ? "Checkmate - You win!" : "Checkmate - You lose");
        else strcpy(buf, gameResult == 1 ? "Checkmate! White wins" : "Checkmate! Black wins");
        break;
      }
      case 3: strcpy(buf, "Stalemate - draw"); break;
      case 4: strcpy(buf, "Draw: 50-move rule"); break;
      case 5: strcpy(buf, "Draw: repetition"); break;
      case 7: strcpy(buf, "Time out! Black wins"); break;
      case 8: strcpy(buf, "Time out! White wins"); break;
      default: strcpy(buf, "Draw: no material"); break;
    }
  } else if (state == ST_AI) {
    strcpy(buf, "Thinking...");
  } else {
    const char *who = (mode == 1) ? "Your move" : (CUR.side == SIDE_W ? "White to move" : "Black to move");
    snprintf(buf, sizeof(buf), "%s%s", who, inChk ? " - CHECK" : "");
    if (inChk) col = C_RED;
  }
  textCentered(buf, 160, 12, 2, col);
  drawSoundIcon();
}

static void drawBoard() {
  int lastFrom = -1, lastTo = -1;
  if (histN > 0) { lastFrom = mvHist[histN - 1].from; lastTo = mvHist[histN - 1].to; }
  int kingSq = -1;
  if (inChk) {
    int k = (CUR.side == SIDE_W) ? KING : -KING;
    for (int i = 0; i < 64; i++) if (CUR.b[i] == k) { kingSq = i; break; }
  }
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      int r = flipped ? 7 - row : row, f = flipped ? 7 - col : col;
      int sq = r * 8 + f;
      int x = col * SQ, y = BOARD_Y + row * SQ;
      bool light = ((r + f) & 1) == 0;
      uint16_t c = light ? C_LIGHT : C_DARK;
      if (sq == lastFrom || sq == lastTo) c = light ? C_LAST_L : C_LAST_D;
      if (sq == sel) c = C_SEL;
      if (sq == kingSq) c = C_CHECK;
      gfx->fillRect(x, y, SQ, SQ, c);
      uint16_t lc = light ? C_DARK : C_LIGHT;
      gfx->setTextSize(1);
      gfx->setTextColor(lc);
      if (row == 7) { gfx->setCursor(x + SQ - 8, y + SQ - 10); gfx->print((char)('a' + f)); }
      if (col == 0) { gfx->setCursor(x + 2, y + 2); gfx->print((char)('0' + (8 - r))); }
      int pc = CUR.b[sq];
      if (pc) drawPiece(pc, x + SQ / 2, y + SQ / 2);
      if (sel >= 0 && tgt[sq]) {
        if (!pc) gfx->fillCircle(x + SQ / 2, y + SQ / 2, 6, C_MARK);
        else { gfx->drawCircle(x + SQ / 2, y + SQ / 2, 19, C_MARK); gfx->drawCircle(x + SQ / 2, y + SQ / 2, 18, C_MARK); }
      }
    }
  }
}

static void drawClocks() {
  gfx->fillRect(0, 362, 320, 46, C_BG);
  for (int side = 0; side < 2; side++) {
    int x = side == 0 ? 8 : 164;
    bool active = (state != ST_OVER) && (CUR.side == side);
    uint16_t bg = C_PANEL;
    if (active) bg = (timeLeft[side] <= 30000) ? C_CHECK : C_BTN_SEL;
    if (timeLeft[side] <= 0) bg = C_CHECK;
    gfx->fillRoundRect(x, 364, 148, 42, 8, bg);
    gfx->setTextSize(1);
    gfx->setTextColor(C_TXT);
    gfx->setCursor(x + 8, 367);
    gfx->print(side == 0 ? "WHITE" : "BLACK");
    char tb[12];
    int s = (timeLeft[side] + 999) / 1000;
    snprintf(tb, sizeof(tb), "%d:%02d", s / 60, s % 60);
    textCentered(tb, x + 74, 378, 3, C_TXT);
  }
}

static void drawBottom() {
  int y0 = BOARD_Y + 8 * SQ;     // 360
  gfx->fillRect(0, y0, 320, 480 - y0, C_BG);
  char buf[40] = "New game";
  if (histN > 0) {
    const Move &m = mvHist[histN - 1];
    char mt[16];
    if (m.flags & F_CASTLE) strcpy(mt, (m.to & 7) == 6 ? "O-O" : "O-O-O");
    else {
      char pr[3] = {0, 0, 0};
      if (m.promo) { pr[0] = '='; pr[1] = " PNBRQK"[m.promo]; }
      snprintf(mt, sizeof(mt), "%c%d%c%c%d%s", 'a' + (m.from & 7), 8 - (m.from >> 3),
               (m.flags & F_CAP) ? 'x' : '-', 'a' + (m.to & 7), 8 - (m.to >> 3), pr);
    }
    snprintf(buf, sizeof(buf), "Move %d: %s", (histN + 1) / 2, mt);
  }
  if (clockOn()) drawClocks();
  else textCentered(buf, 160, y0 + 14, 2, C_TXT);
  drawBtn(8,   412, 96, 52, "Undo", false);
  drawBtn(112, 412, 96, 52, "Flip", false);
  drawBtn(216, 412, 96, 52, "Menu", false);
}

static const int PROMO_OPTS[4] = {QUEEN, ROOK, BISHOP, KNIGHT};

static void drawPromo() {
  gfx->fillRoundRect(10, 150, 300, 100, 10, C_PANEL);
  gfx->drawRoundRect(10, 150, 300, 100, 10, C_TITLE);
  textCentered("Promote to:", 160, 158, 2, C_TXT);
  for (int i = 0; i < 4; i++) {
    int x = 22 + i * 72, y = 182;
    gfx->fillRoundRect(x, y, 64, 58, 8, C_BTN);
    int t = PROMO_OPTS[i];
    drawPiece(CUR.side == SIDE_W ? t : -t, x + 32, y + 29);
  }
}

static void drawAll() {
  gfx->fillScreen(C_BG);
  drawStatus();
  drawBoard();
  drawBottom();
  if (state == ST_PROMO) drawPromo();
}
static void redraw() { drawAll(); gfx->flush(); }

static void drawMenu() {
  gfx->fillScreen(C_BG);
  textCentered("CHESS", 160, 24, 6, C_TITLE);
  textCentered("Mode", 160, 98, 2, C_TXT);
  drawBtn(15, 125, 140, 50, "1 Player", mode == 1);
  drawBtn(165, 125, 140, 50, "2 Players", mode == 2);
  if (mode == 1) {
    textCentered("You play", 160, 193, 2, C_TXT);
    drawBtn(15, 220, 140, 50, "White", humanSide == SIDE_W);
    drawBtn(165, 220, 140, 50, "Black", humanSide == SIDE_B);
    textCentered("Difficulty", 160, 283, 2, C_TXT);
    drawBtn(15, 310, 90, 50, "Easy", level == 0);
    drawBtn(115, 310, 90, 50, "Medium", level == 1);
    drawBtn(215, 310, 90, 50, "Hard", level == 2);
  } else {
    textCentered("Rotate board each move", 160, 193, 2, C_TXT);
    drawBtn(15, 220, 140, 50, "Off", !autoFlip);
    drawBtn(165, 220, 140, 50, "On", autoFlip);
    textCentered("Time per player", 160, 283, 2, C_TXT);
    for (int i = 0; i < 5; i++) {
      char lb[8];
      if (CLOCK_OPTS[i] == 0) strcpy(lb, "Off");
      else snprintf(lb, sizeof(lb), "%dm", CLOCK_OPTS[i]);
      drawBtn(8 + i * 64, 310, 56, 50, lb, clockIdx == i);
    }
  }
  drawBtn(60, 366, 200, 30, "Sound settings", false);
  drawBtn(40, 400, 240, 60, "START", true, C_GO);
  gfx->flush();
}

// ============================ SOUND SETTINGS ============================
static void loadSettings() {
  Preferences p;
  p.begin("chess", false);
  musicVol = constrain((int)p.getUChar("mvol", 8), 0, 10);
  sfxVol   = constrain((int)p.getUChar("svol", 8), 0, 10);
  p.end();
}

static void saveSettings() {
  Preferences p;
  p.begin("chess", false);
  p.putUChar("mvol", (uint8_t)musicVol);
  p.putUChar("svol", (uint8_t)sfxVol);
  p.end();
}

static void drawVolRow(int y, const char *name, int level) {
  char b[24];
  snprintf(b, sizeof(b), "%s: %d", name, level);
  textCentered(b, 160, y, 2, C_TXT);
  drawBtn(10, y + 30, 64, 60, "-", false);
  drawBtn(246, y + 30, 64, 60, "+", false);
  for (int i = 0; i < 10; i++)
    gfx->fillRect(84 + i * 15, y + 42, 12, 36, i < level ? C_BTN_SEL : C_BTN);
}

static void drawSoundScreen() {
  gfx->fillScreen(C_BG);
  textCentered("SOUND", 160, 24, 4, C_TITLE);
  drawVolRow(90, "Music", musicVol);       // controls at y = 120..180
  drawVolRow(230, "Effects", sfxVol);      // controls at y = 260..320
  drawBtn(60, 400, 200, 60, "DONE", true, C_GO);
  gfx->flush();
}

static void openSound() {
  soundPrev = state;
  state = ST_SOUND;
  drawSoundScreen();
}

static void closeSound() {
  saveSettings();
  state = soundPrev;
  if (state == ST_MENU) drawMenu(); else redraw();
}

static void soundTap(int x, int y) {
  int *vol = nullptr;
  bool isSfx = false;
  if (y >= 120 && y < 180) vol = &musicVol;
  else if (y >= 260 && y < 320) { vol = &sfxVol; isSfx = true; }
  if (vol) {
    if (x >= 10 && x < 74) { if (*vol > 0) (*vol)--; }
    else if (x >= 246 && x < 310) { if (*vol < 10) (*vol)++; }
    else if (x >= 84 && x < 236) { *vol = constrain((x - 84) / 15 + 1, 1, 10); }
    else return;
    audioSetVolumes(musicVol, sfxVol);
    if (isSfx) sfx(SFX_MOVE);              // preview the effects volume
    drawSoundScreen();                      // music is already playing, so you hear its level live
    return;
  }
  if (inR(x, y, 60, 400, 200, 60)) closeSound();
}

// ============================ GAME FLOW ============================
static void refreshLegal() { nLegal = genLegal(CUR, legalMv); }

static void evalState() {
  gameResult = 0;
  const Pos &p = CUR;
  inChk = inCheck(p, p.side);
  if (nLegal == 0) { gameResult = inChk ? (p.side == SIDE_W ? 2 : 1) : 3; return; }
  if (insufficient(p)) { gameResult = 6; return; }
  if (p.half >= 100) { gameResult = 4; return; }
  int cnt = 0;
  for (int i = 0; i < histN; i++) if (samePos(hist[i], p)) cnt++;
  if (cnt >= 2) gameResult = 5;
}

static void startAI() {
  state = ST_AI;
  redraw();
  aiPos = CUR;
  aiDone = false;
  xTaskCreatePinnedToCore(aiTask, "ai", 65536, NULL, 1, NULL, 1);
}

// jingle for the end of the game (win / lose / draw)
static void playResultSfx() {
  if (gameResult == 1 || gameResult == 2 || gameResult == 7 || gameResult == 8) {
    int winner = (gameResult == 1 || gameResult == 8) ? SIDE_W : SIDE_B;   // 7 = white flagged, 8 = black flagged
    if (mode == 1) sfx(winner == humanSide ? SFX_WIN : SFX_LOSE);
    else sfx(SFX_WIN);
  } else {
    sfx(SFX_DRAW);
  }
}

static void afterMove() {
  if (gameResult) { state = ST_OVER; redraw(); playResultSfx(); }
  else if (mode == 1 && CUR.side != humanSide) startAI();
  else { state = ST_PLAY; redraw(); }
}

static void applyMove(const Move &m) {
  if (histN >= MAXH - 1) return;
  hist[histN + 1] = hist[histN];
  makeMove(hist[histN + 1], m);
  mvHist[histN] = m;
  histN++;
  sel = -1; memset(tgt, 0, sizeof(tgt));
  refreshLegal();
  evalState();
  if (mode == 2 && autoFlip) flipped = (CUR.side == SIDE_B);
  if (!gameResult) {
    if (inChk) sfx(SFX_CHECK);
    else if (m.promo) sfx(SFX_PROMO);
    else if (m.flags & F_CASTLE) sfx(SFX_CASTLE);
    else if (m.flags & F_CAP) sfx(SFX_CAPTURE);
    else sfx(SFX_MOVE);
  }
  afterMove();
}

static void newGame() {
  setupStart(hist[0]);
  histN = 0; sel = -1; memset(tgt, 0, sizeof(tgt));
  timeLeft[0] = timeLeft[1] = (int32_t)CLOCK_OPTS[clockIdx] * 60000L;
  lastTick = millis();
  audioRestartMusic();
  sfx(SFX_START);
  flipped = (mode == 1 && humanSide == SIDE_B);
  refreshLegal();
  evalState();
  afterMove();
}

static void showMenu() { state = ST_MENU; drawMenu(); }

static void doUndo() {
  if (state == ST_AI || state == ST_PROMO || state == ST_MENU) return;
  if (gameResult >= 7) return;   // game ended on time, no undo
  int minPly = (mode == 1 && humanSide == SIDE_B) ? 1 : 0;
  if (histN <= minPly) return;
  histN--;
  while (mode == 1 && CUR.side != humanSide && histN > minPly) histN--;
  sel = -1; memset(tgt, 0, sizeof(tgt));
  refreshLegal();
  evalState();
  if (mode == 2 && autoFlip) flipped = (CUR.side == SIDE_B);
  state = ST_PLAY;
  redraw();
}

// ============================ INPUT ============================
static void boardTap(int x, int y) {
  int col = x / SQ, row = (y - BOARD_Y) / SQ;
  if (col < 0 || col > 7 || row < 0 || row > 7) return;
  int r = flipped ? 7 - row : row, f = flipped ? 7 - col : col;
  int sq = r * 8 + f;
  const Pos &p = CUR;
  int pc = p.b[sq];
  bool own = pc && ((pc > 0) == (p.side == SIDE_W));

  if (sel >= 0 && tgt[sq]) {
    Move chosen = {0, 0, 0, 0};
    bool found = false, needPromo = false;
    for (int i = 0; i < nLegal; i++) {
      if (legalMv[i].from == sel && legalMv[i].to == sq) {
        found = true; chosen = legalMv[i];
        if (legalMv[i].promo) needPromo = true;
      }
    }
    if (!found) return;
    if (needPromo) {
      promoFrom = sel; promoTo = sq;
      state = ST_PROMO;
      redraw();
      return;
    }
    applyMove(chosen);
    return;
  }
  memset(tgt, 0, sizeof(tgt));
  if (own) {
    sel = sq;
    for (int i = 0; i < nLegal; i++) if (legalMv[i].from == sq) tgt[legalMv[i].to] = true;
    sfx(SFX_SELECT);
  } else {
    sel = -1;
  }
  redraw();
}

static void promoTap(int x, int y) {
  for (int i = 0; i < 4; i++) {
    if (inR(x, y, 22 + i * 72, 182, 64, 58)) {
      for (int k = 0; k < nLegal; k++) {
        if (legalMv[k].from == promoFrom && legalMv[k].to == promoTo && legalMv[k].promo == PROMO_OPTS[i]) {
          applyMove(legalMv[k]);
          return;
        }
      }
    }
  }
  state = ST_PLAY; sel = -1; memset(tgt, 0, sizeof(tgt));
  redraw();
}

static void menuTap(int x, int y) {
  sfx(SFX_TICK);
  if (inR(x, y, 15, 125, 140, 50)) mode = 1;
  else if (inR(x, y, 165, 125, 140, 50)) mode = 2;
  else if (inR(x, y, 40, 400, 240, 60)) { newGame(); return; }
  else if (inR(x, y, 60, 366, 200, 30)) { openSound(); return; }
  else if (mode == 1) {
    if (inR(x, y, 15, 220, 140, 50)) humanSide = SIDE_W;
    else if (inR(x, y, 165, 220, 140, 50)) humanSide = SIDE_B;
    else if (inR(x, y, 15, 310, 90, 50)) level = 0;
    else if (inR(x, y, 115, 310, 90, 50)) level = 1;
    else if (inR(x, y, 215, 310, 90, 50)) level = 2;
    else return;
  } else {
    if (inR(x, y, 15, 220, 140, 50)) autoFlip = false;
    else if (inR(x, y, 165, 220, 140, 50)) autoFlip = true;
    else {
      bool hit = false;
      for (int i = 0; i < 5; i++) if (inR(x, y, 8 + i * 64, 310, 56, 50)) { clockIdx = i; hit = true; }
      if (!hit) return;
    }
  }
  drawMenu();
}

static void handleTap(int x, int y) {
  if (state != ST_MENU && state != ST_SOUND && inR(x, y, 284, 2, 34, 36)) {   // speaker icon: sound settings
    sfx(SFX_TICK);
    openSound();
    return;
  }
  switch (state) {
    case ST_MENU: menuTap(x, y); break;
    case ST_SOUND: soundTap(x, y); break;
    case ST_AI: break;
    case ST_PROMO: promoTap(x, y); break;
    default:
      if (inR(x, y, 8, 412, 96, 52)) { sfx(SFX_TICK); doUndo(); }
      else if (inR(x, y, 112, 412, 96, 52)) { sfx(SFX_TICK); flipped = !flipped; redraw(); }
      else if (inR(x, y, 216, 412, 96, 52)) { sfx(SFX_TICK); showMenu(); }
      else if (state == ST_PLAY && y >= BOARD_Y && y < BOARD_Y + 8 * SQ) boardTap(x, y);
      break;
  }
}

// AXS15231B touch controller (I2C 0x3B)
static bool readTouch(int &x, int &y) {
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08};
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, 8);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)8) != 8) return false;
  uint8_t d[8];
  for (int i = 0; i < 8; i++) d[i] = Wire.read();
  if (d[1] == 0 || d[1] > 5) return false;
  int rx = ((d[2] & 0x0F) << 8) | d[3];
  int ry = ((d[4] & 0x0F) << 8) | d[5];
#if TOUCH_DEBUG
  Serial.printf("touch raw x=%d y=%d\n", rx, ry);
#endif
#if TOUCH_SWAP_XY
  { int t = rx; rx = ry; ry = t; }
#endif
#if TOUCH_INV_X
  rx = 319 - rx;
#endif
#if TOUCH_INV_Y
  ry = 479 - ry;
#endif
  if (rx < 0 || rx > 319 || ry < 0 || ry > 479) return false;
  x = rx; y = ry;
  return true;
}

// counts down the side to move; flags a time-out
static void tickClock() {
  uint32_t now = millis();
  if (!(clockOn() && (state == ST_PLAY || state == ST_PROMO) && !gameResult)) {
    lastTick = now;
    return;
  }
  uint32_t dt = now - lastTick;
  lastTick = now;
  int s = CUR.side;
  timeLeft[s] -= (int32_t)dt;
  if (timeLeft[s] <= 0) {
    timeLeft[s] = 0;
    gameResult = (s == SIDE_W) ? 7 : 8;
    state = ST_OVER;
    sel = -1; memset(tgt, 0, sizeof(tgt));
    redraw();
    playResultSfx();
    return;
  }
  static int lastSecs = -1, lastSide = -1;
  int secs = (timeLeft[s] + 999) / 1000;
  if (secs != lastSecs || s != lastSide) {
    lastSecs = secs; lastSide = s;
    drawClocks();
    gfx->flush();
  }
}

// ============================ ARDUINO ============================
void setup() {
  Serial.begin(115200);
  delay(2000);   // give the Serial Monitor time to connect
  Serial.println("Chess starting");
  Serial.printf("PSRAM found: %d, PSRAM size: %u, free heap: %u\n",
                (int)psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  bool ok = gfx->begin();
  Serial.printf("gfx->begin() returned %d\n", (int)ok);
  if (!ok) {
    Serial.println("Display init failed - check PSRAM setting (OPI vs QSPI)");
    while (true) delay(1000);
  }
  // colour test: red / green / blue bands
  gfx->fillRect(0, 0, 320, 160, rgb(255, 0, 0));
  gfx->fillRect(0, 160, 320, 160, rgb(0, 255, 0));
  gfx->fillRect(0, 320, 320, 160, rgb(0, 0, 255));
  gfx->flush();
  Serial.println("Test pattern flushed");
  delay(2000);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
  loadSettings();
  audioBegin();
  audioSetVolumes(musicVol, sfxVol);
  showMenu();
}

void loop() {
  if (state == ST_AI && aiDone) {
    aiDone = false;
    applyMove(aiMove);
  }
  static bool wasDown = false;
  static uint32_t lastTap = 0;
  int tx, ty;
  bool down = readTouch(tx, ty);
  if (down && !wasDown && millis() - lastTap > 200) {
    lastTap = millis();
    handleTap(tx, ty);
  }
  wasDown = down;
  tickClock();
  audioSetVolumes(musicVol, sfxVol);
  audioMusic(musicVol > 0 && state != ST_OVER);     // music plays in menu and game, stops when the game ends
  delay(10);
}
