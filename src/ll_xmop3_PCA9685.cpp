// Copyright 2026 by Frobenius Norm LLC 2026-05-16
// Free for non-commercial use. Commercial use requires a license.
#include "LambLisp.h"


#if LL_PCA9685

#include "PCA9685.h"
//! @defgroup xmop3_pca9685 PCA9685 PWM / Servo Driver
//! @ingroup xmop3
//! @brief LambLisp PCA9685 PWM / Servo Driver builtins.
//! @{

PCA9685 *pca9685 = 0;

//! Initialize PCA9685 at I2C addr via Wire; sets 50 Hz PWM frequency.
Sexpr_t PCA9685_mop3_begin(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::PCA9685_mop3_begin()");
  LL_int32 addr = lamb.car_mustbe_int32(sexpr);
  LL_Wire->beginTransmission(addr);
  LL_Wire->write((uint8_t) 0x00);
  LL_Wire->write((uint8_t) 0x00);
  LL_int32 res = LL_Wire->endTransmission();
  // `%s` + ascii.dec(), never `%d`, for an LL_int32: the type names a WIDTH and the C spelling
  // moves (`int32_t` is `int` on x86-64, `long` on xtensa), so a fixed specifier is UB on one of
  // them.  For more than one integer in a message, later ones take their own AsciiConverter --
  // `ascii` is a singleton, so two `ascii.dec()` calls in one call would share a buffer.
  if (res) lamb.log("%s No device at %s, LL_Wire->endTransmission == %s\n", me,
                    ascii.dec(addr), AsciiConverter().dec(res));
  else {
    lamb.log("%s I2C device found at %s\n", me, ascii.dec(addr));
    pca9685->setupSingleDevice(Wire, addr, false);
    pca9685->setSingleDeviceToFrequency(addr, 50);
    lamb.log("%s PCA freq readback %d\n", me, pca9685->getSingleDeviceFrequency(0x5f));
  }
  return lamb.mk_integer(res, env_exec);
}

//! Configure a single PCA9685 device on the Wire bus at the given I2C address.
Sexpr_t PCA9685_mop3_setupSingleDevice(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  LL_int32 addr = lamb.car_mustbe_int32(sexpr);
  pca9685->setupSingleDevice(Wire, addr, false);
  return OBJ_UNDEF;
}

//! Set PWM oscillator frequency (Hz) for a single device at the given I2C address.
Sexpr_t PCA9685_mop3_setSingleDeviceToFrequency(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::PCA9685_mop3_setSingleDeviceToFrequency()");
  LL_int32 addr = lamb.car_mustbe_int32(sexpr);
  LL_int32 freq = lamb.car_coerce_int32(lamb.cdr(sexpr));
  lamb.log("%s (%s, %s)\n", me, ascii.dec(addr), AsciiConverter().dec(freq));
  
  pca9685->setSingleDeviceToFrequency(addr, freq);
  return OBJ_UNDEF;
}

//! Read back the PWM oscillator frequency (Hz) for a single device at the given address.
Sexpr_t PCA9685_mop3_getSingleDeviceFrequency(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::PCA9685_mop3_getSingleDeviceFrequency()");
  LL_int32 addr = lamb.car_mustbe_int32(sexpr);
  LL_int32 freq = pca9685->getSingleDeviceFrequency(addr);
  lamb.log("%s (%s) ==> %s\n", me, ascii.dec(addr), AsciiConverter().dec(freq));
  return lamb.mk_integer(freq, env_exec);
}

//! Set channel duty cycle as a fraction [0.0, 1.0] with optional phase shift fraction.
Sexpr_t PCA9685_mop3_setChannelDutyCycle(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  LL_int32 chan  = lamb.car_mustbe_int32(sexpr);
  LL_float32 frac = lamb.car_coerce_float32(lamb.cdr(sexpr));
  LL_float32 phsh = lamb.car_coerce_float32(lamb.cddr(sexpr));
  pca9685->setChannelDutyCycle(chan, frac, phsh);
  return OBJ_UNDEF;
}

//! Set channel pulse width in raw 12-bit ticks (0--4095).
Sexpr_t PCA9685_mop3_setChannelPulseWidth(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  LL_int32 chan = lamb.car_mustbe_int32(sexpr);
  LL_int32 pw   = lamb.car_coerce_int32(lamb.cdr(sexpr));
  pca9685->setChannelPulseWidth(chan, 0, pw);
  return OBJ_UNDEF;
}

//! Set channel servo pulse duration in microseconds.
Sexpr_t PCA9685_mop3_setChannelServoPulseDuration(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  LL_int32 chan = lamb.car_mustbe_int32(sexpr);
  LL_int32 dur  = lamb.car_coerce_int32(lamb.cdr(sexpr));
  pca9685->setChannelServoPulseDuration(chan, dur);
  return OBJ_UNDEF;
}

//! Set channel on-time and off-time in raw 12-bit ticks; on-time anchored at 0.
Sexpr_t PCA9685_mop3_setChannelOnAndOffTime(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  LL_int32 chan = lamb.car_mustbe_int32(sexpr);
  LL_int32 t_on = lamb.car_coerce_int32(lamb.cdr(sexpr));
  pca9685->setChannelOnAndOffTime(chan, 0, t_on);
  return OBJ_UNDEF;
}

#endif

Sexpr_t PCA9685_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::PCA9685_install_mop3()");
  ll_try {
#if LL_PCA9685
    if (pca9685) { delete pca9685;  pca9685 = 0; }
    pca9685 = new PCA9685;
#endif
    lamb.log("%s installing Mops\n", me);
    // [P275] `env_target` is read by dict_bind_bang in BOTH loops below, after mk_Mop3_procst_t
    // and mk_symbol allocate, and is an argument to neither -> it needs a root spanning both.
    // Per-iteration frames go inside each loop so the depth returns each time round; `sym` needs
    // nothing (dict_bind_bang protects its arguments).  The frame body spans the `#if LL_PCA9685`
    // region: GCC accepts a directive inside macro arguments and this tree is GCC on every target,
    // with no `-pedantic` in the build flags -- measured for the same shape in d3f3cc43.
    mop3_gc_frame({
        Sexpr_t env_target = lamb.car(sexpr);
        static const struct { Lamb::Mop3st_t func; const char *name; bool syntax; } base_procs[] = {
          { PCA9685_install_mop3, "PCA9685.install-mop3", false },
        };
        const int Nbase_procs = sizeof(base_procs)/sizeof(base_procs[0]);
        for (int i = 0; i < Nbase_procs; i++) {
          const auto &p = base_procs[i];
          mop3_gc_frame({
              mop3_gcroot_Sexpr_t(proc, lamb.mk_Mop3_procst_t(p.func, env_exec));
              Sexpr_t sym = lamb.mk_symbol(p.name, env_exec);
              lamb.dict_bind_bang(env_target, sym, proc, env_exec);
          });
        }
#if LL_PCA9685
    static const struct { Lamb::Mop3st_t func; const char *name; bool syntax; } pca_procs[] = {
      { PCA9685_mop3_begin,                         "PCA9685.begin",                         false },
      { PCA9685_mop3_setupSingleDevice,             "PCA9685.setupSingleDevice",             false },
      { PCA9685_mop3_setSingleDeviceToFrequency,    "PCA9685.setSingleDeviceToFrequency",    false },
      { PCA9685_mop3_getSingleDeviceFrequency,      "PCA9685.getSingleDeviceFrequency",      false },
      { PCA9685_mop3_setChannelDutyCycle,           "PCA9685.setChannelDutyCycle",           false },
      { PCA9685_mop3_setChannelOnAndOffTime,        "PCA9685.setChannelOnAndOffTime",        false },
      { PCA9685_mop3_setChannelPulseWidth,          "PCA9685.setChannelPulseWidth",          false },
      { PCA9685_mop3_setChannelServoPulseDuration,  "PCA9685.setChannelServoPulseDuration",  false },
    };
        const int Npca_procs = sizeof(pca_procs)/sizeof(pca_procs[0]);
        lamb.log("%s defining %d Mops\n", me, Npca_procs);
        for (int i = 0; i < Npca_procs; i++) {
          const auto &p = pca_procs[i];
          mop3_gc_frame({
              mop3_gcroot_Sexpr_t(proc, lamb.mk_Mop3_procst_t(p.func, env_exec));
              Sexpr_t sym = lamb.mk_symbol(p.name, env_exec);
              lamb.dict_bind_bang(env_target, sym, proc, env_exec);
          });
        }
#endif
    });
    return NIL;
  }
  ll_catch();

}
//! @}
