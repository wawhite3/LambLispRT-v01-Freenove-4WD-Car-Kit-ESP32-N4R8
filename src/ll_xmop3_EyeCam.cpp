// Copyright 2026 by Frobenius Norm LLC 2026-07-02
// Free for non-commercial use. Commercial use requires a license.
//
// ESP32-S3-EYE camera (OV2640) + built-in ST7789 LCD -- P136 Phase 2.
//
// A self-contained LambLisp mop binding for the ESP32-S3-EYE dev board.  It
// uses ONLY the ESP-IDF components already bundled in the Arduino-ESP32 core:
//   * esp_camera  -- the OV2640 DVP sensor driver (RGB565 or JPEG frames)
//   * esp_lcd     -- the ST7789 240x240 IPS panel over SPI3
// No external Arduino libraries are required.
//
// Pins are from Espressif's esp-bsp (bsp/esp32_s3_eye), verified against the
// board schematic.  The frame hot-paths (grab + blit, mean-luma) stay in C++;
// Scheme drives the demo one-liners in scm/features/eye-cam.scm.
#include "LambLisp.h"


#if LL_CAMERA

#include "esp_camera.h"                 // OV2640 driver: camera_config_t, esp_camera_*
#include "driver/ledc.h"                // LEDC PWM -- backlight brightness (ACTIVE-LOW panel)
#include "esp_heap_caps.h"
// [P211]
void ll_heap_note(const char *tag);   //!< every fast-RAM claim states the remainder              // heap_caps_malloc -- DMA-capable draw buffers (not PSRAM)
//! @defgroup xmop3_camera Camera (ESP32-S3-EYE)
//! @ingroup xmop3
//! @brief LambLisp Camera (ESP32-S3-EYE) builtins.
//! @{

// [P231 phase 0]
/*! THE PANEL LEFT THIS FILE.  It is `ll_xmop3_Panel.cpp` now, behind the
    `LL_Panel` interface in `ll_platform_generic.h`, and this driver asks only ONE question of it:
    is there a panel to send a frame to?  That is `LL_PANEL`.

    What used to be here was `LL_CAM_HAS_LCD`, derived as `LL_ESP32S3` -- correct only by
    accident, because the one S3 board with a camera happened to be the one board with the
    ST7789.  Worse, the panel could not exist WITHOUT this file: all 98 LCD references were
    inside `#if LL_CAMERA`, so a board with a panel and no camera could not compile a line
    of it.  A camera driver is the wrong owner for a display. */


// succeed.  The LCD band is still lazily reserved at lcd-init (row-halving fallback) and the camera hole
// just before esp_camera_init, so restoring camera preview (B87) needs this flipped back ON *and* a
// boot-read-safe reservation strategy (e.g. reserve only after the boot .scm loads finish).
#define EYE_CAM_EARLY_DMA_RESERVE 0

// --- Camera: OV2640 DVP ---
/*! TWO BOARDS, TWO WIRINGS, AND THEY SHARE NO PIN.  These are not interchangeable and a build that
    picks the wrong set configures GPIOs that either do not exist or belong to something else.
    The Freenove numbers are NOT guessed: they are exactly what
    scm/boards/pins/Freenove-4WD-Car-Kit-ESP32.scm already declares (pin-XCLK 21, pin-PCLK 22,
    pin-HREF 23, pin-CSI_VYSNC 25, pin-SIOD 26, pin-SIOC 27, pin-CSI_Y2..Y9 = 4 5 18 19 36 39 34
    35), which is the Freenove WROVER-CAM mapping and differs from the AI-Thinker ESP32-CAM one
    that a web search hands you first.  The Scheme map and this table must agree; if the board is
    ever re-wired, change BOTH. */
// [P123 Part D] the pin map this mirrors
#if LL_ESP32S3
  // ESP32-S3-EYE
  #define EYE_CAM_XCLK    15
  #define EYE_CAM_SIOD     4           //!< SCCB SDA
  #define EYE_CAM_SIOC     5           //!< SCCB SCL
  #define EYE_CAM_VSYNC    6
  #define EYE_CAM_HREF     7
  #define EYE_CAM_PCLK    13
  #define EYE_CAM_D0      11           //!< D0..D7 are the sensor's Y2..Y9
  #define EYE_CAM_D1       9
  #define EYE_CAM_D2       8
  #define EYE_CAM_D3      10
  #define EYE_CAM_D4      12
  #define EYE_CAM_D5      18
  #define EYE_CAM_D6      17
  #define EYE_CAM_D7      16
#else
  // Freenove ESP32-WROVER-CAM (classic ESP32) -- the 4WD chassis and the breadboard WROVER
  #define EYE_CAM_XCLK    21
  #define EYE_CAM_SIOD    26           //!< SCCB SDA
  #define EYE_CAM_SIOC    27           //!< SCCB SCL
  #define EYE_CAM_VSYNC   25
  #define EYE_CAM_HREF    23
  #define EYE_CAM_PCLK    22
  #define EYE_CAM_D0       4           //!< Y2
  #define EYE_CAM_D1       5           //!< Y3
  #define EYE_CAM_D2      18           //!< Y4
  #define EYE_CAM_D3      19           //!< Y5
  #define EYE_CAM_D4      36           //!< Y6  (input-only pin -- fine, DVP data is read-only)
  #define EYE_CAM_D5      39           //!< Y7  (input-only)
  #define EYE_CAM_D6      34           //!< Y8  (input-only)
  #define EYE_CAM_D7      35           //!< Y9  (input-only)
#endif

static bool                       ll_cam_ok    = false;  //!< true once the sensor is initialised
static bool                       ll_eyecam_trace = false; //!< per-frame camera->lcd tracing -- OFF: the
                                                           //!< app-loop calls camera->lcd every tick, so a
                                                           //!< log there floods the shared serial line and
                                                           //!< buries REPL and test output.
static pixformat_t                ll_cam_fmt   = PIXFORMAT_RGB565;  //!< current camera pixel format
// [P260]
static framesize_t                ll_cam_size  = FRAMESIZE_240X240;  //!< size camera-init used
static void                      *ll_cam_dma_hole = 0; //!< contiguous internal-DMA block reserved early, freed just before esp_camera_init

static camera_config_t ll_eye_cam_config(pixformat_t fmt, framesize_t size)
{
  camera_config_t c = {};
  c.pin_pwdn     = -1;
  c.pin_reset    = -1;
  c.pin_xclk     = EYE_CAM_XCLK;
  c.pin_sccb_sda = EYE_CAM_SIOD;
  c.pin_sccb_scl = EYE_CAM_SIOC;
  c.pin_d7       = EYE_CAM_D7;
  c.pin_d6       = EYE_CAM_D6;
  c.pin_d5       = EYE_CAM_D5;
  c.pin_d4       = EYE_CAM_D4;
  c.pin_d3       = EYE_CAM_D3;
  c.pin_d2       = EYE_CAM_D2;
  c.pin_d1       = EYE_CAM_D1;
  c.pin_d0       = EYE_CAM_D0;
  c.pin_vsync    = EYE_CAM_VSYNC;
  c.pin_href     = EYE_CAM_HREF;
  c.pin_pclk     = EYE_CAM_PCLK;
  c.xclk_freq_hz = 16000000;           // 16 MHz -- 20 MHz overran the DVP DMA (EV-VSYNC-OVF -> torn
                                       //           frames that render as a diagonal colour sweep)
  c.ledc_timer   = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = fmt;
  c.frame_size   = size;
  c.jpeg_quality = 12;                 // 0-63, lower = better (JPEG mode only)
  c.fb_count     = 2;                  // double-buffered
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  // WHEN_EMPTY, not LATEST: the DVP captures on demand and WAITS when buffers are full instead of
  // free-running.  With LATEST the queue backed up whenever the loop stopped consuming (e.g. after
  // eye-live-stop), the DMA overran and HALTED, and every later fb_get returned NULL until a full
  // re-init.  WHEN_EMPTY can never overflow, so intermittent single grabs (frame-brightness,
  // camera-thumb) always return a valid frame.
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  return c;
}

//! Bring up (or re-affirm) the OV2640.  The ~23 KB internal-DMA line buffer is reserved
//! ONCE, EARLY (from the installer, while the heap is still contiguous) and held for the
//! session -- same rationale as LambLisp's cell block.  Calling this again in the SAME
//! pixel format is a no-op that KEEPS that buffer (re-initing later would free the 23 KB
//! and likely fail to re-grab it, since the heap has since fragmented to ~13 KB).  Only a
//! format change (rgb565<->jpeg) deinits + re-inits.
static bool ll_eye_camera_init(Lamb &lamb, pixformat_t fmt, framesize_t size)
{
  // [P260]
  //! THE SIZE IS PART OF "THIS MODE".  This tested only the FORMAT, so after the size
  //! became selectable a call asking for a bigger frame would have returned true and quietly
  //! kept the old one -- and the measurement would have read as "a larger capture changed
  //! nothing", which is the most expensive way to be wrong about it.
  if (ll_cam_ok && fmt == ll_cam_fmt && size == ll_cam_size) return true;  // already up this way
  if (ll_cam_ok) { esp_camera_deinit(); ll_cam_ok = false; }
  // Release the contiguous internal-DMA block reserved at boot IMMEDIATELY before
  // esp_camera_init so the freshly-freed hole is what the camera's ~23 KB line buffer lands
  // in (nothing allocates internal DMA between here and the init).
  if (ll_cam_dma_hole) { heap_caps_free(ll_cam_dma_hole); ll_cam_dma_hole = 0; }

  /*! REFUSE WITH A VALUE RATHER THAN STALL.  `esp_camera_init` needs a contiguous internal-DMA
      block for its ~23 KB line buffer, and when the heap cannot supply one it does NOT return an
      error -- it stops, taking the caller's loop with it.  On this runtime that means the REPL,
      the LLIP receiver and eventually the network all die with it, so the board looks like a
      hardware or network fault rather than an allocation failure.

      Measured on an S3-EYE: `(camera-init 'jpeg)` returns ESP_OK in milliseconds on a freshly
      booted board, and never returns if a single Scheme file has been loaded first -- the file's
      definitions come out of the same pool.  The margin is that thin.

      Checking first costs one call and converts a hang into #f, which a caller can act on.  It
      does not make the allocation succeed: that needs the camera brought up before user code
      loads (see the board setup file), or an early reserve that starves LittleFS
      boot reads and corrupts .scm files -- which is why that path is off and must stay off. */
  {
    const size_t need = 24 * 1024;
    const size_t have = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    if (have < need) {
            lamb.log("[eyecam] REFUSING camera_init: largest free internal-DMA block is %s B, needs %s B"
               " -- bring the camera up before loading user code\n",
               ascii.dec((LL_int32) have), ascii.dec((LL_int32) need));
      return false;
    }
  }
  camera_config_t cfg = ll_eye_cam_config(fmt, size);
  esp_err_t err = esp_camera_init(&cfg);
  ll_cam_ok = (err == ESP_OK);
  if (ll_cam_ok) { ll_cam_fmt = fmt; ll_cam_size = size; }
  // [P260]
  //! LOG THE SIZE, NOT JUST THE FORMAT.  The DMA requirement scales with LINE WIDTH, so a
  //! wider frame can be refused by `esp_camera_init` even though the 24 KB check above passed --
  //! that floor was measured at 240x240 and has never been re-derived.  A failure that does not
  //! name the size asked for is unattributable.
  lamb.log("[eyecam] camera_init fmt=%d size=%d -> %s\n",
           (int) fmt, (int) size, esp_err_to_name(err));
  // Discard the first frames: the OV2640 AEC/AGC and the DVP pipeline need a few frames to
  // settle -- the earliest frames are dark/torn.  Draining them also clears any startup OVF.
  if (ll_cam_ok)
    for (int i = 0; i < 4; i++) { camera_fb_t *fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); }
  return ll_cam_ok;
}

//! (camera-init [mode]) -> #t/#f.  mode 'rgb565 (default, drives the LCD) or 'jpeg (for capture).
Sexpr_t mop3_camera_init(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_camera_init()");
  ll_try {
    mop3_gc_frame({
      pixformat_t fmt  = PIXFORMAT_RGB565;
      framesize_t size = FRAMESIZE_240X240;
      if (sexpr != NIL) {
        Sexpr_t mode = lamb.car(sexpr);
        if (mode->is_any_sym_atom() && !strcmp(mode->any_sym_get_chars(), "jpeg"))
          fmt = PIXFORMAT_JPEG;
        Sexpr_t rest = lamb.cdr(sexpr);
        if (rest != NIL && lamb.car(rest)->is_any_sym_atom()) {
          static const struct { const char *name; framesize_t fs; } sizes[] = {
            { "qqvga", FRAMESIZE_QQVGA },     //!< 160x120
            { "qvga",  FRAMESIZE_QVGA  },     //!< 320x240
            { "240",   FRAMESIZE_240X240 },   //!< 240x240 -- the historical hardcoded default
            { "hvga",  FRAMESIZE_HVGA  },     //!< 480x320
            { "vga",   FRAMESIZE_VGA   },     //!< 640x480
            { "svga",  FRAMESIZE_SVGA  },     //!< 800x600
            { "xga",   FRAMESIZE_XGA   },     //!< 1024x768
            { "uxga",  FRAMESIZE_UXGA  },     //!< 1600x1200, the OV2640's maximum
          };
          const char *want = lamb.car(rest)->any_sym_get_chars();
          bool found = false;
          for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]) && !found; i++)
            if (!strcmp(want, sizes[i].name)) { size = sizes[i].fs; found = true; }
          //! REFUSE AN UNKNOWN NAME RATHER THAN FALLING BACK TO THE DEFAULT.  A typo that quietly
          //! captured at 240x240 would read as "the larger size made no difference" -- a null result
          //! from an experiment that never ran, which this tree has paid for repeatedly.
          if (!found) {
            lamb.log("camera-init: unknown size '%s' -- refusing;"
                     " use qqvga qvga 240 hvga vga svga xga uxga\n", want);
            return HASHF;
          }
        }
      }
      return ll_eye_camera_init(lamb, fmt, size) ? HASHT : HASHF;
    });
  }
  ll_catch();
}

//! (camera->lcd) -> #t/#f.  Grab one RGB565 frame and blit it to the LCD (the hot path).
#if LL_PANEL   // ---- needs a panel to send frames to; absent on the WROVER-CAM / 4WD
Sexpr_t mop3_camera_to_lcd(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_camera_to_lcd()");
  ll_try {
    if (!ll_cam_ok || !ll_panel_ready()) return HASHF;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return HASHF;
    const uint16_t *pp = (const uint16_t *) fb->buf;
    //! PER-FRAME DEBUG, OFF BY DEFAULT.  This logged unconditionally, and the eye's app-loop calls
    //! camera->lcd on EVERY tick once the live preview auto-starts -- ~20 lines/second onto the
    //! same serial line the REPL and every test harness read.  It has buried the gcpause
    //! output and the `--- done ---` sentinel under a wall of frame dumps, so a leg that had
    //! ALREADY failed for a different reason (the bench file was missing from the manifest) looked
    //! busy for 146 s instead of obviously broken.  A log inside a frame-rate path is a debugging
    //! aid, never shipped behaviour.
    if (ll_eyecam_trace)
      lamb.log("[eyecam] c2l fb w=%d h=%d len=%d fmt=%d  px0=%04x px1=%04x pxN/2=%04x pxEnd=%04x\n",
               (int) fb->width, (int) fb->height, (int) fb->len, (int) fb->format,
               pp[0], pp[1], pp[(fb->len/2)/2], pp[fb->len/2 - 1]);
    ll_panel_blit_rgb565(lamb, 0, 0, fb->width, fb->height, pp);
    esp_camera_fb_return(fb);
    return HASHT;
  }
  ll_catch();
}

//! (camera-preview [n]) -> n.  Live preview: blit n frames to the LCD (default 300).
Sexpr_t mop3_camera_preview(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_camera_preview()");
  ll_try {
    mop3_gc_frame({
      if (!ll_cam_ok || !ll_panel_ready()) return HASHF;
      LL_int32 n = (sexpr != NIL) ? lamb.car_mustbe_int32(sexpr) : 300;
      for (LL_int32 i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
          ll_panel_blit_rgb565(lamb, 0, 0, fb->width, fb->height, (const uint16_t *) fb->buf);
          esp_camera_fb_return(fb);
        }
      }
      return lamb.mk_integer(n, env_exec);
    });
  }
  ll_catch();
}

//! (camera-thumb [n]) -> #t/#f.  Grab one RGB565 frame, nearest-neighbour downsample to n*n
//! (default 24, clamped 4..48), and print it over serial as:
//!    THUMB <n> <srcW> <srcH>
//!    <n rows of n 4-hex RGB565 values, byte-swapped to native>
//! For OFF-DEVICE visual verification (reconstruct the image on the host) without the LCD --
//! isolates a camera/frame fault from an LCD/blit fault.  Output is gated by the quiet REPL,
//! so call (verbose) first.
#endif  // LL_PANEL
Sexpr_t mop3_camera_thumb(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_camera_thumb()");
  ll_try {
    mop3_gc_frame({
      if (!ll_cam_ok) return HASHF;
      // (camera-thumb n)       -> nearest-neighbour subsample of the whole frame.
      // (camera-thumb n x0 y0) -> RAW n*n crop at (x0,y0), every pixel -- full-res detail that reveals
      //                           fine moire / streaks / screen subpixels that subsampling aliases away.
      int n = 24, cx = -1, cy = -1;
      { Sexpr_t a = sexpr;
        if (a != NIL) { Sexpr_t v = lamb.car(a); if (v->is_any_int()) n  = (int) v->as_int32(); a = lamb.cdr(a); }
        if (a != NIL) { Sexpr_t v = lamb.car(a); if (v->is_any_int()) cx = (int) v->as_int32(); a = lamb.cdr(a); }
        if (a != NIL) { Sexpr_t v = lamb.car(a); if (v->is_any_int()) cy = (int) v->as_int32(); } }
      if (n < 4)  n = 4;
      if (n > 48) n = 48;
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) return HASHF;
      const uint16_t *src = (const uint16_t *) fb->buf;
      const int W = fb->width, H = fb->height;
      const bool crop = (cx >= 0 && cy >= 0);
      lamb.log("THUMB %d %d %d %s\n", n, W, H, crop ? "crop" : "sub");
      for (int ty = 0; ty < n; ty++) {
        char line[48 * 4 + 2];
        int  p  = 0;
        int  sy = crop ? (cy + ty) : (ty * H / n);
        if (sy >= H) sy = H - 1;
        for (int tx = 0; tx < n; tx++) {
          int sx = crop ? (cx + tx) : (tx * W / n);
          if (sx >= W) sx = W - 1;
          uint16_t v = src[(size_t) sy * W + sx];
          v = (uint16_t) ((v >> 8) | (v << 8));            // same byte-swap the blit applies
          static const char hx[] = "0123456789abcdef";
          line[p++] = hx[(v >> 12) & 0xf]; line[p++] = hx[(v >> 8) & 0xf];
          line[p++] = hx[(v >> 4)  & 0xf]; line[p++] = hx[v & 0xf];
        }
        line[p++] = '\n'; line[p] = 0;
        lamb.log("%s", line);
      }
      esp_camera_fb_return(fb);
      return HASHT;
    });
  }
  ll_catch();
}

/*! (camera-set key val) -> #t if the sensor took it, #f if it did not.

    Tune the sensor through its control block.  Keys, with the range each one accepts:

      EXPOSURE AND GAIN -- the controls that decide whether a dim scene is usable at all:
        exposure-ctrl  0/1       automatic exposure on (1) or off (0)
        aec-value      0..1200   manual exposure time, in sensor row periods; needs exposure-ctrl 0
        aec2           0/1       the DSP's second-stage auto exposure ("night mode")
        ae-level      -2..2      bias the automatic exposure darker or brighter
        gain-ctrl      0/1       automatic gain on (1) or off (0)
        agc-gain       0..30     manual gain; needs gain-ctrl 0
        gainceiling    0..6      how far automatic gain may go: 0 is 2x, 6 is 128x

      IMAGE:
        framesize, quality (10..63, LOWER is better), brightness/contrast/saturation (-2..2),
        sharpness, denoise, special-effect, wb-mode, whitebal, awb-gain, raw-gma, lenc, dcw,
        bpc, wpc, colorbar, hmirror, vflip.

    RAISING THE GAIN CEILING IS THE LEVER FOR A DIM SCENE, AND THE TRADE IS NOISE AND MOTION BLUR.
    Automatic exposure will not use a gain it is not permitted, so a camera indoors at dusk
    typically sits at its ceiling with the image still under-exposed; the default ceiling is low
    because a moving camera cannot afford the blur a long exposure brings.  A camera that does not
    move can: there is nothing in the frame to smear, and a noisy bright frame carries far more for
    a recogniser than a clean black one.  So a fixed installation should expect to raise
    `gainceiling`, and may set `aec-value` directly once it knows its scene.

    BRIGHTNESS IS NOT EXPOSURE.  It is a digital offset applied to pixels the sensor has already
    captured, so it lifts the black level and the noise together and collects no extra light.
    Measured on an under-exposed indoor wall: brightness 2 with contrast 2 left the scene darker
    still, because the added contrast crushed what little shadow detail there was.  Reach for the
    exposure and gain keys above instead.

    IT REPORTS WHETHER IT WORKED, AND THE THREE WAYS IT CAN FAIL ARE DISTINGUISHED IN THE LOG:
    the camera is not initialised, the key is not one of the above, or this sensor does not
    implement that control.  A control block carries a null pointer for anything the attached part
    cannot do -- the OV2640 and the OV5640 do not implement the same set -- so each call is
    checked before it is made; an unchecked call through a null entry is a reset, not an error.
    Returning a value rather than nothing matters because the three failures and a success used to
    be indistinguishable from the caller: a typo in a key silently did nothing. */
//! DEFINED AT FILE SCOPE, NOT INSIDE THE GC FRAME BELOW, AND IT MUST STAY THERE.
//! The gc frame macros are variadic, so the braced block passed to one is a macro ARGUMENT -- and a
//! `#define` written inside a macro argument is not in effect for that argument's own tokens.  Move
//! this back in and all ~20 uses below fail to compile with "'LL_CAM_SET_I' was not declared in
//! this scope", naming a definition that is plainly visible above them.
//! `#if` inside the same block DOES work (the preprocessor evaluates conditionals there), so a
//! block can contain working directives alongside one that silently did nothing.
#define LL_CAM_SET_I(FN)  do { if (s->FN) rc = s->FN(s, (int) val); else have = false; } while (0)

Sexpr_t mop3_camera_set(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_camera_set()");
  ll_try {
    mop3_gc_frame({
      Sexpr_t res = HASHF;
      if (!ll_cam_ok) {
        lamb.log("camera-set: the camera is not initialised\n");
        return HASHF;
      }
      sensor_t *s = esp_camera_sensor_get();
      if (!s) {
        lamb.log("camera-set: no sensor control block\n");
        return HASHF;
      }
      //! THE KEY CELL IS CONSUMED BEFORE THE VALUE IS FETCHED, AND THAT ORDER IS THE POINT.
      //! `car` and `cdr` ALLOCATE.  A pair slot can hold a small integer or float EMBEDDED in the
      //! slot itself, so the accessor must BOX that value into a fresh cell on the way out -- a cell
      //! the caller's argument list never contained, so the root the interpreter holds on that list
      //! does NOT cover it.  Fetching the value is itself an accessor, hence itself an allocation:
      //! holding the key cell across it would let a collection cycle reclaim the box and hand the
      //! cell out again with a different type, after which the symbol test below passes on an
      //! unrelated object and the character read follows a pointer that is not a symbol.  That is a
      //! fault with no Scheme error to catch, because the check that should have refused it has
      //! already run.  So: test the key, take its characters, and only then fetch the value.
      //! `key_cell` MUST NOT be dereferenced after the `val` line below -- it is named for the cell
      //! rather than for the key so that a reader looking for its uses finds all of them.
      Sexpr_t  key_cell   = lamb.car(sexpr);
      bool     key_is_sym = key_cell->is_any_sym_atom();
      //! Interned symbol characters outlive the call; `""` keeps the comparisons below well-defined
      //! on the rejected path rather than relying on the early return alone.
      const char *k = key_is_sym ? key_cell->any_sym_get_chars() : "";
      LL_int32 val = lamb.car_mustbe_int32(lamb.cdr(sexpr));
      if (!key_is_sym) {
        lamb.log("camera-set: the key must be a symbol\n");
        return HASHF;
      }
      bool known = true;       //!< the key is one of ours
      bool have  = true;       //!< this sensor implements it (the entry is not null)
      int  rc    = 0;          //!< what the sensor's own setter answered; 0 is success

      //! ONE LINE PER CONTROL, EACH CHECKING ITS OWN ENTRY FIRST.  The repetition is deliberate:
      //! the setters do not share a signature (framesize and gainceiling take enums), so a table
      //! would need a cast per row and would hide exactly the null check that must not be skipped.
      if      (!strcmp(k, "framesize"))      { if (s->set_framesize)   rc = s->set_framesize(s, (framesize_t) val);   else have = false; }
      else if (!strcmp(k, "gainceiling"))    { if (s->set_gainceiling) rc = s->set_gainceiling(s, (gainceiling_t) val); else have = false; }
      else if (!strcmp(k, "exposure-ctrl"))  LL_CAM_SET_I(set_exposure_ctrl);
      else if (!strcmp(k, "aec-value"))      LL_CAM_SET_I(set_aec_value);
      else if (!strcmp(k, "aec2"))           LL_CAM_SET_I(set_aec2);
      else if (!strcmp(k, "ae-level"))       LL_CAM_SET_I(set_ae_level);
      else if (!strcmp(k, "gain-ctrl"))      LL_CAM_SET_I(set_gain_ctrl);
      else if (!strcmp(k, "agc-gain"))       LL_CAM_SET_I(set_agc_gain);
      else if (!strcmp(k, "brightness"))     LL_CAM_SET_I(set_brightness);
      else if (!strcmp(k, "contrast"))       LL_CAM_SET_I(set_contrast);
      else if (!strcmp(k, "saturation"))     LL_CAM_SET_I(set_saturation);
      else if (!strcmp(k, "sharpness"))      LL_CAM_SET_I(set_sharpness);
      else if (!strcmp(k, "denoise"))        LL_CAM_SET_I(set_denoise);
      else if (!strcmp(k, "quality"))        LL_CAM_SET_I(set_quality);
      else if (!strcmp(k, "whitebal"))       LL_CAM_SET_I(set_whitebal);
      else if (!strcmp(k, "awb-gain"))       LL_CAM_SET_I(set_awb_gain);
      else if (!strcmp(k, "wb-mode"))        LL_CAM_SET_I(set_wb_mode);
      else if (!strcmp(k, "raw-gma"))        LL_CAM_SET_I(set_raw_gma);
      else if (!strcmp(k, "lenc"))           LL_CAM_SET_I(set_lenc);
      else if (!strcmp(k, "dcw"))            LL_CAM_SET_I(set_dcw);
      else if (!strcmp(k, "bpc"))            LL_CAM_SET_I(set_bpc);
      else if (!strcmp(k, "wpc"))            LL_CAM_SET_I(set_wpc);
      else if (!strcmp(k, "colorbar"))       LL_CAM_SET_I(set_colorbar);
      else if (!strcmp(k, "hmirror"))        LL_CAM_SET_I(set_hmirror);
      else if (!strcmp(k, "vflip"))          LL_CAM_SET_I(set_vflip);
      else if (!strcmp(k, "special-effect")) LL_CAM_SET_I(set_special_effect);
      else known = false;

      if (!known)
        lamb.log("camera-set: unknown key '%s'\n", k);
      else if (!have)
        lamb.log("camera-set: this sensor does not implement '%s'\n", k);
      else if (rc != 0)
        lamb.log("camera-set: the sensor refused '%s' = %s (rc %s)\n",
                 k, ascii.dec(val), ascii.dec((LL_int32) rc));
      else
        res = HASHT;
      return res;
    });
  }
  ll_catch();
}

#undef LL_CAM_SET_I

//! (camera-get) -> an alist of the sensor's CURRENT exposure and gain state.  A fixed installation
//! has to know whether automatic exposure has already run out of room before it decides to raise
//! the ceiling or to ask for more light, and the status block carries what the sensor settled on.
Sexpr_t mop3_camera_get(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_camera_get()");
  ll_try {
    Sexpr_t res = HASHF;
    sensor_t *s = ll_cam_ok ? esp_camera_sensor_get() : 0;
    if (!s) {
      lamb.log("camera-get: the camera is not initialised\n");
      return HASHF;
    }
    //! Built by prepending and reversed once, so the list reads in the order written here.
    mop3_gc_frame({
        Sexpr_t acc = NIL;
        lamb.gc_root_push(acc);
        static const struct { const char *name; int val; } st[] = {
          { "framesize",     (int) s->status.framesize },
          { "quality",       (int) s->status.quality },
          { "exposure-ctrl", (int) s->status.aec },
          { "aec-value",     (int) s->status.aec_value },
          { "aec2",          (int) s->status.aec2 },
          { "ae-level",      (int) s->status.ae_level },
          { "gain-ctrl",     (int) s->status.agc },
          { "agc-gain",      (int) s->status.agc_gain },
          { "gainceiling",   (int) s->status.gainceiling },
          { "brightness",    (int) s->status.brightness },
          { "contrast",      (int) s->status.contrast },
        };
        for (int i = (int) (sizeof(st)/sizeof(st[0])) - 1; i >= 0; i--) {
          mop3_gc_frame({
            Sexpr_t v = lamb.mk_integer(st[i].val, env_exec);
            lamb.gc_root_push(v);
            Sexpr_t nm = lamb.mk_symbol(st[i].name, env_exec);
            lamb.gc_root_push(nm);
            Sexpr_t pr = lamb.cons(nm, v, env_exec);
            lamb.gc_root_push(pr);
            acc = lamb.cons(pr, acc, env_exec);
            lamb.gc_root_push(acc);
          });
        }
        res = acc;
    });
    return res;
  }
  ll_catch();
}

//! (camera-capture) -> bytevector.  Return one JPEG frame (needs (camera-init 'jpeg));
//! returns #f if no frame is available.
Sexpr_t mop3_camera_capture(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_camera_capture()");
  ll_try {
    mop3_gc_frame({
      if (!ll_cam_ok) return HASHF;
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) return HASHF;
      // Copy the JPEG bytes into a GC-managed T_BVEC_HEAP (payload alloc; freed by the GC).
      Sexpr_t bv = lamb.mk_bytevector((LL_int32) fb->len, (Bytest_t) fb->buf, env_exec);
      esp_camera_fb_return(fb);
      return bv;
    });
  }
  ll_catch();
}

//! (frame-brightness) -> integer 0..255.  Cheap mean luma over a downsampled RGB565
//! frame -- a scalar for trigger-policy demos ("brighten past N -> fire").
Sexpr_t mop3_frame_brightness(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_frame_brightness()");
  ll_try {
    mop3_gc_frame({
      if (!ll_cam_ok) return lamb.mk_integer(-1, env_exec);
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) return lamb.mk_integer(-1, env_exec);
      if (fb->format != PIXFORMAT_RGB565) { esp_camera_fb_return(fb); return lamb.mk_integer(-1, env_exec); }
      // Big-endian RGB565: each pixel is [hi, lo].  Sample every 8th pixel and
      // accumulate an approximate luma (Rec.601 weights on 8-bit expanded channels).
      const uint8_t *p     = fb->buf;
      size_t         npix  = fb->len / 2;
      unsigned long  sum   = 0;
      unsigned long  count = 0;
      for (size_t i = 0; i < npix; i += 8) {
        uint16_t px = (uint16_t)((p[2 * i] << 8) | p[2 * i + 1]);
        int r = ((px >> 11) & 0x1F) << 3;      // 5-bit -> 8-bit
        int g = ((px >> 5)  & 0x3F) << 2;      // 6-bit -> 8-bit
        int b = ( px        & 0x1F) << 3;      // 5-bit -> 8-bit
        sum   += (r * 77 + g * 150 + b * 29) >> 8;
        count += 1;
      }
      esp_camera_fb_return(fb);
      LL_int32 mean = count ? (LL_int32)(sum / count) : 0;
      return lamb.mk_integer(mean, env_exec);
    });
  }
  ll_catch();
}

//! (lcd-backlight [percent]) -> void.  Set backlight brightness 0..100 (default 100).
//! Handy for demos: (lcd-backlight 0) blanks the screen without tearing down the panel,
//! (lcd-backlight 100) restores it.  Independent of panel content, so it also isolates a
//! backlight fault from a draw fault when bringing up new hardware.

#endif // LL_CAMERA

//! Installer -- always compiles; registers the eye mops only under LL_CAMERA.
Sexpr_t EyeCam_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::EyeCam_install_mop3()");
  ll_try {
    lamb.log("%s installing Mops\n", me);
    // [P275] `env_target` comes from `lamb.car(sexpr)`, which may box, and is read by
    // dict_bind_bang in BOTH binding blocks below, after mk_Mop3_procst_t and mk_symbol
    // allocate -- an argument to neither, so step 2 does not exempt it and a root is necessary
    // (step 3).  One frame spanning both blocks, with a per-block / per-iteration frame inside
    // (step 4) so the depth returns each time round.  The outer frame opens before `#if
    // LL_CAMERA` and closes after its `#endif`: it has to, or the braces would not balance on a
    // build without a camera.
    mop3_gc_frame({
        Sexpr_t env_target = lamb.car(sexpr);
        mop3_gc_frame({
            mop3_gcroot_Sexpr_t(proc, lamb.mk_Mop3_procst_t(EyeCam_install_mop3, env_exec));
            //! `sym` is read by dict_bind_bang with no allocation in between -- step 1 stops it.
            Sexpr_t sym = lamb.mk_symbol("EyeCam-install-mop3", env_exec);
            lamb.dict_bind_bang(env_target, sym, proc, env_exec);
        });
#if LL_CAMERA
        static const struct { Lamb::Mop3st_t func; const char *name; } eye_procs[] = {
          { mop3_camera_init,      "camera-init"      },
#if LL_PANEL
          { mop3_camera_to_lcd,    "camera->lcd"      },
          { mop3_camera_preview,   "camera-preview"   },
#endif
          { mop3_camera_thumb,     "camera-thumb"     },
          { mop3_camera_set,       "camera-set"       },
          { mop3_camera_get,       "camera-get"       },
          { mop3_camera_capture,   "camera-capture"   },
          { mop3_frame_brightness, "frame-brightness" },
        };
        const int Neye_procs = sizeof(eye_procs)/sizeof(eye_procs[0]);
        lamb.log("%s defining %d Mops\n", me, Neye_procs);
        for (int i = 0; i < Neye_procs; i++) {
          const auto &p = eye_procs[i];
          mop3_gc_frame({
              mop3_gcroot_Sexpr_t(proc, lamb.mk_Mop3_procst_t(p.func, env_exec));
              Sexpr_t sym = lamb.mk_symbol(p.name, env_exec);
              lamb.dict_bind_bang(env_target, sym, proc, env_exec);
          });
        }
        // Reserve a contiguous internal-DMA HOLE for the camera NOW -- the installer runs right
        // after the cell block but BEFORE setup.scm loads, so the heap is still contiguous (~48 KB
        // block).  We do NOT init the camera here: starting DVP capture while setup.scm reads files
        // from flash crashes on camera-DMA-vs-flash-cache contention (boot loop).  Instead we hold
        // an empty placeholder and free it right before esp_camera_init (in camera-init), so the
        // camera's ~23 KB line buffer lands in this reserved contiguous region even though the rest
        // of the heap has fragmented to ~13 KB by then (the recurring 23 KB DMA-malloc failure).
        // Reserve the camera hole NOW.  setup.scm's cell-heap EXPANSION (startup needs >8192 cells
        // -> a new internal block) also comes out of DMA-capable RAM.
        //! THE LCD BAND USED TO BE RESERVED HERE TOO, AND IS NOT ANY MORE: it belongs to the panel
        //! driver and is claimed lazily at (lcd-init).  The two claims still COMPETE
        //! for the same internal-DMA pool, so whichever runs second can still come back NULL -- that
        //! coupling is real and did not move with the code.  If a blank screen returns after a camera
        //! change, this is the interaction to suspect.
#if EYE_CAM_EARLY_DMA_RESERVE
        ll_cam_dma_hole = heap_caps_malloc(24 * 1024, MALLOC_CAP_DMA);
        ll_heap_note("claim: camera DMA hole");
        lamb.log("%s reserved camera hole %p; largest free DMA now %d\n",
                 me, ll_cam_dma_hole,
                 (int) heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
#else
        lamb.log("%s early DMA reserve OFF (B88): band+hole lazy; largest free DMA now %d\n",
                 me, (int) heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
#endif
#endif
    });
    return NIL;
  }
  ll_catch();
}
//! @}
