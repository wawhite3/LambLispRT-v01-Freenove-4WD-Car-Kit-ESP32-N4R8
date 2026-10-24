// Copyright 2026 by Frobenius Norm LLC 2026-10-07 09:40:00
//
// TensorRT inference for LambLisp: load a serialised engine once, report its IO tensors, and keep
// it resident for repeated inference.
//
// THE WHOLE FILE IS BEHIND `LL_TENSORRT`, and that gate is DISTINCT FROM `LL_CUDA` on purpose.
// CUDA and TensorRT are separate installs: a host can have the CUDA runtime and no TensorRT at all,
// and `linux_x86_64_cuda` is exactly that case.  Gating this on `LL_CUDA` would break that env's
// build for a library it does not have.  `build_src_filter` compiles every file under `src/` for
// every target, so a file that must not participate has to compile to NOTHING rather than rely on
// not being listed.

#if LL_TENSORRT

#include "LambLisp.h"
#include <NvInfer.h>
#include <cuda_runtime_api.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

/*! TensorRT reports through a callback rather than a return code, so a logger is mandatory even for
    a program that only wants errors.  The severity filter keeps the volume sane.

    IT WRITES TO `stderr`, NOT THROUGH THE RUNTIME'S OWN TERMINAL, AND THAT IS A PACKAGING
    CONSTRAINT RATHER THAN A PREFERENCE.  `ll_term` is declared in `ll_vm_term.h`, and `ll_vm_*` is
    STRIPPED from every customer package while `ll_xmop3_*` ships -- so an `ll_xmop3_` file that
    includes an `ll_vm_` header compiles here and fails for a customer on a missing include, which
    is the worst place to find it.  The callback also has no `Lamb &` to reach a logger through.
    Anything in this directory should check which side of that line a header sits on before
    including it. */
class LLTrtLogger : public nvinfer1::ILogger
{
public:
  void log(Severity severity, const char *msg) noexcept override
  {
    if (severity <= Severity::kWARNING) fprintf(stderr, "[trt] %s\n", msg);
  }
};

static LLTrtLogger            trt_logger;
static nvinfer1::IRuntime    *trt_runtime = nullptr;
static nvinfer1::ICudaEngine *trt_engine  = nullptr;
static nvinfer1::IExecutionContext *trt_ctx = nullptr;

//! Free in reverse order of creation.  A context outlives nothing and an engine outlives its
//! context; deleting the runtime first leaves the other two pointing into freed memory, which
//! surfaces as a crash at the NEXT load rather than at this one.
static void trt_release()
{
  if (trt_ctx)     { delete trt_ctx;     trt_ctx     = nullptr; }
  if (trt_engine)  { delete trt_engine;  trt_engine  = nullptr; }
  if (trt_runtime) { delete trt_runtime; trt_runtime = nullptr; }
}

/*! @brief `(vision-engine-load! "PATH")` — deserialise a TensorRT engine and keep it resident.

    Returns a list of `(NAME (DIM ...))` for every IO tensor, or raises on failure.

    THE ENGINE IS LOADED ONCE AND KEPT.  A serialised engine is hundreds of megabytes and
    deserialising it costs seconds; doing that per inference would dominate the measurement and
    exhaust memory on a shared appliance.  Calling this again releases the previous engine first,
    so a reload is safe and does not leak.

    AN ENGINE IS TIED TO THE EXACT TensorRT VERSION, GPU AND DRIVER IT WAS BUILT ON.  A stale or
    foreign engine fails HERE, in deserialisation, and TensorRT's own message talks about
    serialization rather than versions — so this says the useful thing instead. */
Sexpr_t mop3_vision_engine_load(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_vision_engine_load()");
  Sexpr_t res = OBJ_UNDEF;
  mop3_try {
    mop3_gc_frame({
      if (sexpr == NIL) throw lamb.mk_syserror("%s: a path is required", me);
      const char *path = lamb.car(sexpr)->mustbe_any_str_t()->any_str_get_chars();

      std::ifstream f(path, std::ios::binary | std::ios::ate);
      if (!f) throw lamb.mk_syserror("%s: cannot open %s", me, path);
      const std::streamsize n = f.tellg();
      f.seekg(0, std::ios::beg);
      std::vector<char> blob((size_t) n);
      if (!f.read(blob.data(), n)) throw lamb.mk_syserror("%s: short read on %s", me, path);

      trt_release();
      trt_runtime = nvinfer1::createInferRuntime(trt_logger);
      if (!trt_runtime) throw lamb.mk_syserror("%s: createInferRuntime failed", me);

      trt_engine = trt_runtime->deserializeCudaEngine(blob.data(), (size_t) n);
      if (!trt_engine) {
        trt_release();
        throw lamb.mk_syserror("%s: engine failed to deserialise -- it is tied to the exact "
                               "TensorRT version, GPU and driver it was built on; rebuild it", me);
      }
      trt_ctx = trt_engine->createExecutionContext();
      if (!trt_ctx) {
        trt_release();
        throw lamb.mk_syserror("%s: createExecutionContext failed", me);
      }

      //! Built back-to-front and reversed once, so the list comes out in IO-tensor order without
      //! walking it twice.  Every intermediate is rooted: `cons` returns an issued cell, but the
      //! accumulator is NOT reachable from `sexpr` and would otherwise be collectable.
      Sexpr_t acc = NIL;
      const int nio = trt_engine->getNbIOTensors();
      for (int i = nio - 1; i >= 0; i--) {
        const char *tname = trt_engine->getIOTensorName(i);
        nvinfer1::Dims d  = trt_engine->getTensorShape(tname);
        Sexpr_t dims = NIL;
        mop3_gc_frame({ lamb.gc_root_push(acc);
          for (int k = d.nbDims - 1; k >= 0; k--) {
            mop3_gc_frame({ lamb.gc_root_push(dims);
              dims = lamb.cons(lamb.mk_int32((LL_int32) d.d[k], env_exec), dims, env_exec);
            });
          }
        });
        mop3_gc_frame({ lamb.gc_root_push(acc); lamb.gc_root_push(dims);
          Sexpr_t nm   = lamb.mk_string(env_exec, "%s", tname);
          Sexpr_t pair = lamb.cons(nm, lamb.cons(dims, NIL, env_exec), env_exec);
          acc = lamb.cons(pair, acc, env_exec);
        });
      }
      res = acc;
    });
    return res;
  }
  mop3_catch();
}

/*! @brief `(vision-engine-loaded?)` — #t when an engine is resident.

    SEPARATE FROM THE LOAD, so a caller can ask without paying for a reload.  A policy that has to
    deserialise an engine to find out whether it already has one is a policy that never has one. */
Sexpr_t mop3_vision_engine_loaded(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_vision_engine_loaded()");
  mop3_try {
    return (trt_engine && trt_ctx) ? HASHT : HASHF;
  }
  mop3_catch();
}

/*! @brief `(vision-engine-free!)` — release the engine and its context.

    Explicit rather than implicit: the appliance shares its GPU with other work, and an engine held
    by a finished program is memory nobody can account for. */
Sexpr_t mop3_vision_engine_free(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_vision_engine_free()");
  mop3_try {
    trt_release();
    return HASHT;
  }
  mop3_catch();
}


//! MegaDetector v5a's three classes, in the order the engine emits them.  A name list is the one
//! thing a model file does not carry, so getting it wrong relabels every detection silently.
static const char *md_classes[3] = { "animal", "person", "vehicle" };

/*! Resize preserving aspect ratio and pad to a square, returning the scale and the pad offsets.

    THE PADDING MUST BE UNDONE ON THE WAY OUT.  A box in letterboxed coordinates is wrong in the
    original frame by the pad offset, and wrong SILENTLY -- the detection looks plausible and sits
    in the wrong place, which is worse than no detection at all.

    114 is YOLOv5's pad value and is not arbitrary: the network was trained with it, so padding
    with black would put an edge the model has never seen around every frame. */
static void trt_letterbox(const cv::Mat &img, int size, std::vector<float> &chw,
                          float &scale, int &padx, int &pady)
{
  const int h = img.rows, w = img.cols;
  scale = std::min((float) size / (float) h, (float) size / (float) w);
  const int nh = (int) lroundf(h * scale), nw = (int) lroundf(w * scale);
  cv::Mat r;
  cv::resize(img, r, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
  cv::Mat out(size, size, CV_8UC3, cv::Scalar(114, 114, 114));
  pady = (size - nh) / 2;
  padx = (size - nw) / 2;
  r.copyTo(out(cv::Rect(padx, pady, nw, nh)));

  //! BGR -> RGB, HWC -> CHW, /255.  Written as an explicit plane walk rather than through cv::dnn
  //! so the ordering is visible: a silently transposed tensor produces detections that are merely
  //! wrong rather than absent, which is the failure that takes longest to find.
  chw.resize((size_t) 3 * size * size);
  for (int y = 0; y < size; y++) {
    const uint8_t *row = out.ptr<uint8_t>(y);
    for (int x = 0; x < size; x++) {
      const uint8_t b = row[3 * x + 0], g = row[3 * x + 1], rr = row[3 * x + 2];
      const size_t base = (size_t) y * size + x;
      chw[(size_t) 0 * size * size + base] = rr / 255.0f;
      chw[(size_t) 1 * size * size + base] = g  / 255.0f;
      chw[(size_t) 2 * size * size + base] = b  / 255.0f;
    }
  }
}

//! Greedy IoU suppression over xyxy boxes, highest score first.  Returns the kept indices.
static std::vector<int> trt_nms(const std::vector<float> &xyxy, const std::vector<float> &scores,
                                float thr)
{
  std::vector<int> idx(scores.size());
  for (size_t i = 0; i < idx.size(); i++) idx[i] = (int) i;
  std::sort(idx.begin(), idx.end(),
            [&scores](int a, int b) { return scores[(size_t) a] > scores[(size_t) b]; });
  std::vector<int> keep;
  std::vector<char> dead(scores.size(), 0);
  for (size_t a = 0; a < idx.size(); a++) {
    const int i = idx[a];
    if (dead[(size_t) i]) continue;
    keep.push_back(i);
    const float ix1 = xyxy[4 * (size_t) i + 0], iy1 = xyxy[4 * (size_t) i + 1];
    const float ix2 = xyxy[4 * (size_t) i + 2], iy2 = xyxy[4 * (size_t) i + 3];
    const float ai = (ix2 - ix1) * (iy2 - iy1);
    for (size_t b = a + 1; b < idx.size(); b++) {
      const int j = idx[b];
      if (dead[(size_t) j]) continue;
      const float jx1 = xyxy[4 * (size_t) j + 0], jy1 = xyxy[4 * (size_t) j + 1];
      const float jx2 = xyxy[4 * (size_t) j + 2], jy2 = xyxy[4 * (size_t) j + 3];
      const float xx1 = std::max(ix1, jx1), yy1 = std::max(iy1, jy1);
      const float xx2 = std::min(ix2, jx2), yy2 = std::min(iy2, jy2);
      const float iw = std::max(0.0f, xx2 - xx1), ih = std::max(0.0f, yy2 - yy1);
      const float inter = iw * ih;
      const float aj = (jx2 - jx1) * (jy2 - jy1);
      if (inter / (ai + aj - inter + 1e-9f) > thr) dead[(size_t) j] = 1;
    }
  }
  return keep;
}

/*! @brief `(vision-detect BYTEVECTOR [CONF] [IOU])` — detect in an encoded image held in memory.

    Returns a list of `(GROUP NAME CONFIDENCE X Y W H)` in ORIGINAL frame coordinates, highest
    confidence first.  CONF defaults to 0.25, IOU to 0.45.

    IT TAKES ENCODED BYTES, NOT A PATH.  A frame arriving over the wire is already in memory, and
    round-tripping it through the filesystem would cost a write and a read per frame and make the
    file the only copy of something that was already here.

    AN UNDECODABLE IMAGE RETURNS `()`, NOT AN ERROR.  One bad frame in a stream is not a reason to
    stop recognising, and a raise here would take down a caller whose job is to keep receiving. */
Sexpr_t mop3_vision_detect(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_vision_detect()");
  mop3_try {
    mop3_gc_frame({
      if (!trt_engine || !trt_ctx) throw lamb.mk_syserror("%s: no engine -- call vision-engine-load! first", me);
      if (sexpr == NIL) throw lamb.mk_syserror("%s: a bytevector is required", me);

      //! THE BYTEVECTOR IS RESOLVED TO ITS COUNT AND POINTER BEFORE THE THRESHOLDS ARE PARSED, AND
      //! THAT ORDER IS DELIBERATE -- DO NOT MOVE THE FETCH BACK BELOW THEM.  `car` and `cdr` are
      //! allocating calls: a pair slot can hold a small integer or float EMBEDDED in the slot itself,
      //! so the accessor must BOX that value into a fresh cell on the way out -- a cell the caller's
      //! argument list never contained, so the root the interpreter holds on that list does NOT cover
      //! it.  Parsing the two optional thresholds is three more accessor calls, hence three more
      //! chances for a collection cycle to run; a cell held across one and then read afterwards can
      //! have been reclaimed and handed out again with a different type, which is a fault with no
      //! Scheme error to catch because the holder's type check has already run.  Resolving the
      //! bytevector first leaves nothing of it held: `bytes` points into the bytevector object, which
      //! the argument list roots for the whole call.
      //!
      //! `any_bvec_get_info` hands back BOTH the count and the pointer, which is the designed
      //! accessor: fetching them separately invites a length that belongs to a different buffer.
      Sexpr_t bv = lamb.car(sexpr);
      if (!bv->is_any_bvec_atom()) throw lamb.mk_syserror("%s: a bytevector is required", me);
      LL_int32  nbytes = 0;
      ByteVec_t bytes  = nullptr;
      bv->any_bvec_get_info(nbytes, bytes);

      float conf_thr = 0.25f, iou_thr = 0.45f;
      Sexpr_t rest = lamb.cdr(sexpr);
      if (rest != NIL) { conf_thr = (float) lamb.car_coerce_float64(rest); rest = lamb.cdr(rest); }
      if (rest != NIL) { iou_thr  = (float) lamb.car_coerce_float64(rest); }

      //! Kept here, below the threshold parsing, so an empty bytevector still reports as `()` only
      //! after a malformed threshold has been rejected -- the order callers already see.
      if (!bytes || nbytes <= 0) return NIL;

      cv::Mat enc(1, (int) nbytes, CV_8UC1, (void *) bytes);
      cv::Mat img = cv::imdecode(enc, cv::IMREAD_COLOR);
      if (img.empty()) return NIL;

      const int W = img.cols, H = img.rows;
      const int SZ = 640;
      std::vector<float> chw;
      float scale = 1.0f; int padx = 0, pady = 0;
      trt_letterbox(img, SZ, chw, scale, padx, pady);

      //! Shapes come FROM THE ENGINE, not from a constant here.  A model swapped for one with a
      //! different output width would otherwise be read with the wrong stride and produce confident
      //! nonsense rather than an error.
      const char *in_name  = trt_engine->getIOTensorName(0);
      const char *out_name = trt_engine->getIOTensorName(1);
      nvinfer1::Dims od = trt_engine->getTensorShape(out_name);
      size_t nrows = 1, ncols = 1;
      if (od.nbDims >= 3) { nrows = (size_t) od.d[1]; ncols = (size_t) od.d[2]; }
      else throw lamb.mk_syserror("%s: output tensor has %d dims, expected 3", me, (int) od.nbDims);
      if (ncols < 6) throw lamb.mk_syserror("%s: output width %d is too narrow for box+obj+class",
                                            me, (int) ncols);

      void *d_in = nullptr, *d_out = nullptr;
      const size_t in_bytes  = chw.size() * sizeof(float);
      const size_t out_bytes = nrows * ncols * sizeof(float);
      if (cudaMalloc(&d_in, in_bytes) != cudaSuccess) throw lamb.mk_syserror("%s: cudaMalloc in", me);
      if (cudaMalloc(&d_out, out_bytes) != cudaSuccess) {
        cudaFree(d_in);
        throw lamb.mk_syserror("%s: cudaMalloc out", me);
      }
      std::vector<float> host_out(nrows * ncols);
      bool ok = (cudaMemcpy(d_in, chw.data(), in_bytes, cudaMemcpyHostToDevice) == cudaSuccess);
      if (ok) ok = trt_ctx->setTensorAddress(in_name, d_in);
      if (ok) ok = trt_ctx->setTensorAddress(out_name, d_out);
      if (ok) ok = trt_ctx->enqueueV3(0);                       //!< default stream
      if (ok) ok = (cudaStreamSynchronize(0) == cudaSuccess);
      if (ok) ok = (cudaMemcpy(host_out.data(), d_out, out_bytes, cudaMemcpyDeviceToHost) == cudaSuccess);
      cudaFree(d_in);
      cudaFree(d_out);
      if (!ok) throw lamb.mk_syserror("%s: inference failed", me);

      //! Row layout: cx cy w h obj c0 c1 ...  Score is obj * the best class probability, which is
      //! YOLOv5's convention; using obj alone reports a confident "something" with no class.
      const size_t ncls = ncols - 5;
      std::vector<float> xyxy; std::vector<float> scores; std::vector<int> klass;
      for (size_t r = 0; r < nrows; r++) {
        const float *row = &host_out[r * ncols];
        const float obj = row[4];
        size_t best = 0; float bestp = row[5];
        for (size_t c = 1; c < ncls; c++) if (row[5 + c] > bestp) { bestp = row[5 + c]; best = c; }
        const float sc = obj * bestp;
        if (sc < conf_thr) continue;
        //! cxcywh -> xyxy, UNDO the pad, then the scale, then clip to the ORIGINAL frame.
        float x1 = (row[0] - row[2] / 2 - padx) / scale;
        float y1 = (row[1] - row[3] / 2 - pady) / scale;
        float x2 = (row[0] + row[2] / 2 - padx) / scale;
        float y2 = (row[1] + row[3] / 2 - pady) / scale;
        x1 = std::min(std::max(x1, 0.0f), (float) W);
        x2 = std::min(std::max(x2, 0.0f), (float) W);
        y1 = std::min(std::max(y1, 0.0f), (float) H);
        y2 = std::min(std::max(y2, 0.0f), (float) H);
        xyxy.push_back(x1); xyxy.push_back(y1); xyxy.push_back(x2); xyxy.push_back(y2);
        scores.push_back(sc);
        klass.push_back((int) best);
      }
      if (scores.empty()) return NIL;

      std::vector<int> keep = trt_nms(xyxy, scores, iou_thr);

      //! Built back-to-front so the result comes out highest-confidence first without a second pass.
      //! `acc` is not reachable from `sexpr`, so it needs explicit protection across every cons.
      Sexpr_t acc = NIL;
      for (size_t k = keep.size(); k-- > 0; ) {
        const int i = keep[k];
        const int cls = klass[(size_t) i];
        const char *nm = (cls >= 0 && cls < 3) ? md_classes[cls] : "unknown";
        const LL_int32 x = (LL_int32) xyxy[4 * (size_t) i + 0];
        const LL_int32 y = (LL_int32) xyxy[4 * (size_t) i + 1];
        const LL_int32 w = (LL_int32) (xyxy[4 * (size_t) i + 2] - xyxy[4 * (size_t) i + 0]);
        const LL_int32 h = (LL_int32) (xyxy[4 * (size_t) i + 3] - xyxy[4 * (size_t) i + 1]);
        const LL_int32 pc = (LL_int32) lroundf(scores[(size_t) i] * 100.0f);
        mop3_gc_frame({ lamb.gc_root_push(acc);
          Sexpr_t rec = NIL;
          rec = lamb.cons(lamb.mk_int32(h, env_exec), rec, env_exec);
          mop3_gc_frame({ lamb.gc_root_push(rec); rec = lamb.cons(lamb.mk_int32(w, env_exec), rec, env_exec); });
          mop3_gc_frame({ lamb.gc_root_push(rec); rec = lamb.cons(lamb.mk_int32(y, env_exec), rec, env_exec); });
          mop3_gc_frame({ lamb.gc_root_push(rec); rec = lamb.cons(lamb.mk_int32(x, env_exec), rec, env_exec); });
          mop3_gc_frame({ lamb.gc_root_push(rec); rec = lamb.cons(lamb.mk_int32(pc, env_exec), rec, env_exec); });
          mop3_gc_frame({ lamb.gc_root_push(rec); rec = lamb.cons(lamb.mk_string(env_exec, "%s", nm), rec, env_exec); });
          mop3_gc_frame({ lamb.gc_root_push(rec); rec = lamb.cons(lamb.mk_symbol(nm, env_exec), rec, env_exec); });
          acc = lamb.cons(rec, acc, env_exec);
        });
      }
      return acc;
    });
  }
  mop3_catch();
}

Sexpr_t TensorRT_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::TensorRT_install_mop3()");
  ll_try {
    static const struct { Lamb::Mop3st_t func; const char *name; } procs[] = {
      { mop3_vision_engine_load,   "vision-engine-load!"   },
      { mop3_vision_engine_loaded, "vision-engine-loaded?" },
      { mop3_vision_engine_free,   "vision-engine-free!"   },
      { mop3_vision_detect,        "vision-detect"         },
      { TensorRT_install_mop3,     "TensorRT-install-mop3" }
    };
    const int nprocs = sizeof(procs) / sizeof(procs[0]);
    lamb.log("%s defining %d Mops\n", me, nprocs);
    //! [P275] `env_target` is read by dict_bind_bang on every iteration, after
    //! mk_Mop3_procst_t and mk_symbol allocate, and is an argument to neither, so it needs a
    //! root that spans the loop.  The per-iteration frame goes INSIDE the loop -- where the old
    //! the per-iteration frame is -- so the rootstack depth returns each time round.
    mop3_gc_frame({
        Sexpr_t env_target = lamb.car(sexpr);
        for (int i = 0; i < nprocs; i++) {
          mop3_gc_frame({
              mop3_gcroot_Sexpr_t(proc, lamb.mk_Mop3_procst_t(procs[i].func, env_exec));
              Sexpr_t sym = lamb.mk_symbol(procs[i].name, env_exec);
              lamb.dict_bind_bang(env_target, sym, proc, env_exec);
          });
        }
    });
    return NIL;
  }
  ll_catch();
}

#endif  // LL_TENSORRT
