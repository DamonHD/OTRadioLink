/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2015--2016
                            Deniz Erbilgin 2016
*/

/*
 * Hardware-independent logic to control a hardware valve base with proportional control.
 */


#include "OTRadValve_CurrentSenseValveMotorDirect.h"


namespace OTRadValve
{


#ifdef CurrentSenseValveMotorDirect_DEFINED

// Called with each motor run sub-cycle tick.
// Is ISR-/thread- safe ***on AVR***.
void CurrentSenseValveMotorDirect::signalRunSCTTick(const bool opening)
  {
#ifdef ARDUINO_ARCH_AVR
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
#endif // ARDUINO_ARCH_AVR
    {
    // Crudely avoid/ignore underflow/overflow for now.
    // Accumulate ticks in different directions in different counters
    // and resolve/reconcile later in significant chunks.
    if(!opening)
      {
      if(ticksFromOpen < MAX_TICKS_FROM_OPEN) { ++ticksFromOpen; }
      }
    else
      {
      if(ticksReverse < MAX_TICKS_FROM_OPEN) { ++ticksReverse; }
      }
    }
  }

// (Re)populate structure and compute derived parameters.
// Ensures that all necessary items are gathered at once and none forgotten!
// Returns true in case of success.
// May return false and force error state if inputs unusable,
// though will still try to compute all values.
bool CurrentSenseValveMotorDirect::CalibrationParameters::updateAndCompute(const uint16_t _ticksFromOpenToClosed, const uint16_t _ticksFromClosedToOpen)
  {
  ticksFromOpenToClosed = _ticksFromOpenToClosed;
  ticksFromClosedToOpen = _ticksFromClosedToOpen;

  // Compute approx precision in % as min ticks / DR size in range [0,100].
  // Inflate estimate slightly to allow for inertia, etc.
  const uint16_t minticks = OTV0P2BASE::fnmin(_ticksFromOpenToClosed, _ticksFromClosedToOpen);
  approxPrecisionPC = (uint8_t) OTV0P2BASE::fnmin(100UL, (128UL*minMotorDRTicks) / minticks);

  // Compute a small conversion ratio back and forth
  // which does not add too much error but allows single dead-reckoning steps
  // to be converted back and forth.
  uint16_t tfotc = _ticksFromOpenToClosed;
  uint16_t tfcto = _ticksFromClosedToOpen;
  while(OTV0P2BASE::fnmax(tfotc, tfcto) > minMotorDRTicks)
    {
    tfotc >>= 1;
    tfcto >>= 1;
    }
  tfotcSmall = tfotc;
  tfctoSmall = tfcto;

  // Fail if precision far too poor to be usable.
  if(approxPrecisionPC > 25) { return(false); }
  // Fail if lower ratio value so low (< 4 bits) as to introduce huge error.
  if(OTV0P2BASE::fnmin(tfotc, tfcto) < 8) { return(false); }

  // All OK.
  return(true);
  }

// Compute reconciliation/adjustment of ticks, and compute % position [0,100].
// Reconcile any reverse ticks (and adjust with forward ticks if needed).
// Call after moving the valve in normal mode.
// Unit testable.
uint8_t CurrentSenseValveMotorDirect::CalibrationParameters::computePosition(
            volatile uint16_t &ticksFromOpen,
            volatile uint16_t &ticksReverse) const
  {
  // Back out the effect of reverse ticks in blocks for dead-reckoning...
  // Should only usually be about 1 block at a time,
  // so don't do anything too clever here.
  while(ticksReverse >= tfctoSmall)
    {
    if(0 == tfctoSmall) { break; } // Prevent hang if not initialised correctly.
    ticksReverse -= tfctoSmall;
    if(ticksFromOpen > tfotcSmall) { ticksFromOpen -= tfotcSmall; }
    else { ticksFromOpen = 0; }
    }

  // TODO: use shaft encoder tracking by preference, ie when available.

  // Do simple % open calcs for range extremes, based on dead-reckoning.
  if(0 == ticksFromOpen) { return(100); }
  if(ticksFromOpen >= ticksFromOpenToClosed) { return(0); }
  // Compute percentage open for intermediate position, based on dead-reckoning.
  // TODO: optimise!
  return((uint8_t) (((ticksFromOpenToClosed - ticksFromOpen) * 100UL) / ticksFromOpenToClosed));
  }

// Get estimated minimum percentage open for significant flow for this device; strictly positive in range [1,99].
uint8_t CurrentSenseValveMotorDirect::getMinPercentOpen() const
    {
    // If in dead-reckoning mode then use a very safe estimate,
    // else use a somewhat tighter one.
    // TODO: optimise, ie don't compute each time if frequently called.
    return(usingPositionalEncoder() ?
            OTV0P2BASE::fnmax((uint8_t)(10 + cp.getApproxPrecisionPC()), (uint8_t)DEFAULT_VALVE_PC_MIN_REALLY_OPEN) :
            OTV0P2BASE::fnmax((uint8_t)(50 + cp.getApproxPrecisionPC()), (uint8_t)DEFAULT_VALVE_PC_SAFER_OPEN));
    }

// Minimally wiggle the motor to give tactile feedback and/or show to be working.
// May take a significant fraction of a second.
// Finishes with the motor turned off, and a bias to closing the valve.
// Should also have enough movement/play to allow calibration of the shaft encoder.
// May also help set some bounds on stall current, eg if highly asymmetric at each end of travel.
void CurrentSenseValveMotorDirect::wiggle()
  {
  hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorOff, *this);
  hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorDriveOpening, *this);
  hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorDriveClosing, *this);
  hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorOff, *this);
  }

// Run fast towards/to end stop as far as possible in this call.
// Terminates significantly before the end of the sub-cycle.
// Possibly allows partial recalibration, or at least re-homing.
// Returns true if end-stop has apparently been hit,
// else will require one or more further calls in new sub-cycles
// to hit the end-stop.
// May attempt to ride through stiff mechanics.
bool CurrentSenseValveMotorDirect::runFastTowardsEndStop(const bool toOpen)
  {
  // Clear the end-stop detection flag ready.
  endStopDetected = false;
  // Run motor as far as possible on this sub-cycle.
  hw->motorRun(~0, toOpen ?
      OTRadValve::HardwareMotorDriverInterface::motorDriveOpening
    : OTRadValve::HardwareMotorDriverInterface::motorDriveClosing, *this);
  // Stop motor and ensure power off.
  hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorOff, *this);
  // Report if end-stop has apparently been hit.
  return(endStopDetected);
  }

// Run at 'normal' speed towards/to end for a fixed time/distance.
// Terminates significantly before the end of the sub-cycle.
// Runs at same speed as during calibration.
// Does the right thing with dead-reckoning and/or position detection.
// Returns true if end-stop has apparently been hit.
bool CurrentSenseValveMotorDirect::runTowardsEndStop(const bool toOpen)
  {
  // Clear the end-stop detection flag ready.
  endStopDetected = false;
  // Run motor as far as possible on this sub-cycle.
  hw->motorRun(cp.minMotorDRTicks, toOpen ?
      OTRadValve::HardwareMotorDriverInterface::motorDriveOpening
    : OTRadValve::HardwareMotorDriverInterface::motorDriveClosing, *this);
  // Stop motor and ensure power off.
  hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorOff, *this);
  // Report if end-stop has apparently been hit.
  return(endStopDetected);
  }

// Report an apparent serious tracking error that may need full recalibration.
void CurrentSenseValveMotorDirect::reportTrackingError()
  {
  // Possibly ignore tracking errors for a minimum interval.
  // May simply switch to 'binary' on/off mode if the calibration is off.
  needsRecalibrating = true;
  }

// True if (re)calibration should be deferred.
// Potentially an expensive call in time and energy.
bool CurrentSenseValveMotorDirect::shouldDeferCalibration()
    {
    // Try to force measurement of supply voltage now.
    const bool haveBattMonitor = (NULL != lowBattOpt);
    if(haveBattMonitor) { lowBattOpt->read(); }
    // Defer calibration if doing it now would be a bad idea, eg in a bedroom at night.
    const bool deferRecalibration =
        (haveBattMonitor && lowBattOpt->isSupplyVoltageLow()) ||
        ((NULL != minimiseActivityOpt) && minimiseActivityOpt());
    return(deferRecalibration);
    }

// Poll.
// Regular poll every 1s or 2s,
// though tolerates missed polls eg because of other time-critical activity.
// May block for hundreds of milliseconds.
void CurrentSenseValveMotorDirect::poll()
  {

#if 0 && defined(V0P2BASE_DEBUG)
OTV0P2BASE::serialPrintAndFlush(F("    isOnShaftEncoderMark(): "));
OTV0P2BASE::serialPrintAndFlush(hw->isOnShaftEncoderMark());
OTV0P2BASE::serialPrintlnAndFlush();
#endif

#if 0
OTV0P2BASE::serialPrintAndFlush("poll(): ");
OTV0P2BASE::serialPrintAndFlush(state);
OTV0P2BASE::serialPrintlnAndFlush();
#endif

  // Run the state machine based on the major state.
  switch(state)
    {
    // Power-up: wiggle and then wait to move to 'pin withdrawing' state.
    case init:
      {
//V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("  init");
//      // Make sure that the motor is unconditionally turned off.
//      hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorOff, *this);

      // Tactile feedback and ensure that the motor is left stopped.
      // Should also allow calibration of the shaft-encoder outputs, ie [min.max].
      // May also help free 'stuck' mechanics.
      wiggle();

      // Wait before withdrawing pin (just after power-up).
      changeState(initWaiting);
      break;
      }

    // Wait to start withdrawing pin.
    // A strategic wait here helps make other start-up easier, including CLI-based provisioning.
    case initWaiting:
      {
//V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("  initWaiting");

      // Assume 2s between calls to poll().
      if(perState.initWaiting.ticksWaited < initialRetractDelay_s/2) { ++perState.initWaiting.ticksWaited; break; } // Postpone pin withdraw after power-up.

      // Tactile feedback and ensure that the motor is left stopped.
      // Should also allow calibration of the shaft-encoder outputs, ie [min.max].
      // May also help free 'stuck' mechanics.
      wiggle();

      // Now start on fully withdrawing pin.
      changeState(valvePinWithdrawing);
      // TODO: record time withdrawal starts (to allow time out).
      break;
      }

    // Fully withdrawing pin (nominally opening valve) to make valve head easy to fit.
    case valvePinWithdrawing:
      {
//V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("  valvePinWithdrawing");

      // If taking stupidly long to withdraw the pin fully
      // then assume a problem with the motor/mechanics and give up.
      // Don't panic() so that the unit can still (for example) transmit stats.
      if(++perState.valvePinWithdrawing.wallclock2sTicks > MAX_TRAVEL_WALLCLOCK_2s_TICKS)
          {
          OTV0P2BASE::serialPrintlnAndFlush(F("!valve pin withdraw fail"));
          changeState(valveError);
          break;
          }

      // Once end-stop has been hit, move to state to wait for user signal and then start calibration.

      // Run cautiously while supply voltage low to try to avoid browning out.
      const bool low = ((NULL != lowBattOpt) && ((0 == lowBattOpt->read()) || lowBattOpt->isSupplyVoltageLow()));

      if(runTowardsEndStop(true, low)) { changeState(valvePinWithdrawn); }
      break;
      }

    // Running (initial) calibration cycle.
    case valvePinWithdrawn:
      {
//V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("  valvePinWithdrawn");

      // Wait for signal from user that valve has been fitted...
      // TODO: alternative timeout allows for automatic recovery from crash/restart after say 10 mins.
      // From: void signalValveFitted() { perState.valvePinWithdrawn.valveFitted = true; }

      // Note that the valve is now fully open.
      currentPC = 100;

      // Note that (initial) calibration is needed.
      needsRecalibrating = true;

      // Once the valve has been fitted, move to calibration.
      if(perState.valvePinWithdrawn.valveFitted)
        { changeState(valveCalibrating); }
      break;
      }

    // Running (initial or re-) calibration cycle.
    case valveCalibrating:
      {
//V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("  valveCalibrating");
//      V0P2BASE_DEBUG_SERIAL_PRINT_FLASHSTRING("    calibState: ");
//      V0P2BASE_DEBUG_SERIAL_PRINT(perState.calibrating.calibState);
//      V0P2BASE_DEBUG_SERIAL_PRINTLN();

      // Defer calibration if doing it now would be a bad idea, eg in a bedroom at night.
      if(shouldDeferCalibration())
        {
        changeState(valveNormal);
        break;
        }

      // If taking stupidly long to calibrate
      // then assume a problem with the motor/mechanics and give up.
      // Don't panic() so that the unit can still (for example) transmit stats.
      if(++perState.valveCalibrating.wallclock2sTicks > MAX_TRAVEL_WALLCLOCK_2s_TICKS)
        {
        OTV0P2BASE::serialPrintlnAndFlush(F("!valve calibration fail"));
        changeState(valveError);
        break;
        }

      // Select activity based on micro-state.
      switch(perState.valveCalibrating.calibState)
        {
        case 0:
          {
#if 0 && defined(V0P2BASE_DEBUG)
V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("+calibrating");
#endif
          ++perState.valveCalibrating.calibState; // Move to next micro state.
          break;
          }
        case 1:
          {
          // Run fast to fully retracted (easy to fit, nomninally valve fully open).
          if(runFastTowardsEndStop(true))
            {
            // Reset tick count.
            ticksFromOpen = 0;
            ticksReverse = 0;
            perState.valveCalibrating.wallclock2sTicks = 0;
            ++perState.valveCalibrating.calibState; // Move to next micro state.
            }
          break;
          }
        case 2:
          {
          // Run pin to fully extended (valve closed).
          // Be prepared to run the (usually small) dead-reckoning pulse while lots of sub-cycle still available.
          do
            {
            // Once end-stop has been hit, capture run length and prepare to run in opposite direction.
            if(runTowardsEndStop(false))
              {
              const uint16_t tfotc = ticksFromOpen;
              perState.valveCalibrating.ticksFromOpenToClosed = tfotc;
              perState.valveCalibrating.wallclock2sTicks = 0;
              ++perState.valveCalibrating.calibState; // Move to next micro state.
              break;
              }
            } while(getSubCycleTimeFn() <= computeSctAbsLimitDR());
          break;
          }
        case 3:
          {
          // Run pin to fully retracted again (valve open).
          // Be prepared to run the (usually small) pulse while lots of sub-cycle still available.
          do
            {
            // Once end-stop has been hit, capture run length and prepare to run in opposite direction.
            if(runTowardsEndStop(true))
              {
              const uint16_t tfcto = ticksReverse;
              // Help avoid premature termination of this direction
              // by NOT terminating this run if much shorter than run in other direction.
              if(tfcto >= (perState.valveCalibrating.ticksFromOpenToClosed >> 1))
                {
                perState.valveCalibrating.ticksFromClosedToOpen = tfcto;
                // Reset tick count.
                ticksFromOpen = 0;
                ticksReverse = 0;
                perState.valveCalibrating.wallclock2sTicks = 0;
                ++perState.valveCalibrating.calibState; // Move to next micro state.
                }
              break; // In all cases when end-stop hit don't try to run further in this sub-cycle.
              }
            } while(getSubCycleTimeFn() <= computeSctAbsLimitDR());
          break;
          }
        case 4:
          {
          // Set all measured calibration input parameters and current position.
          cp.updateAndCompute(perState.valveCalibrating.ticksFromOpenToClosed, perState.valveCalibrating.ticksFromClosedToOpen);

#if 0 && defined(V0P2BASE_DEBUG)
//V0P2BASE_DEBUG_SERIAL_PRINT_FLASHSTRING("    ticksFromOpenToClosed: ");
//V0P2BASE_DEBUG_SERIAL_PRINT(cp.getTicksFromOpenToClosed());
//V0P2BASE_DEBUG_SERIAL_PRINTLN();
//V0P2BASE_DEBUG_SERIAL_PRINT_FLASHSTRING("    ticksFromClosedToOpen: ");
//V0P2BASE_DEBUG_SERIAL_PRINT(cp.getTicksFromClosedToOpen());
//V0P2BASE_DEBUG_SERIAL_PRINTLN();
//V0P2BASE_DEBUG_SERIAL_PRINT_FLASHSTRING("    precision %: ");
//V0P2BASE_DEBUG_SERIAL_PRINT(cp.getApproxPrecisionPC());
//V0P2BASE_DEBUG_SERIAL_PRINTLN();

OTV0P2BASE::serialPrintAndFlush(F("    ticksFromOpenToClosed: "));
OTV0P2BASE::serialPrintAndFlush(cp.getTicksFromOpenToClosed());
OTV0P2BASE::serialPrintlnAndFlush();
OTV0P2BASE::serialPrintAndFlush(F("    ticksFromClosedToOpen: "));
OTV0P2BASE::serialPrintAndFlush(cp.getTicksFromClosedToOpen());
OTV0P2BASE::serialPrintlnAndFlush();
OTV0P2BASE::serialPrintAndFlush(F("    precision %: "));
OTV0P2BASE::serialPrintAndFlush(cp.getApproxPrecisionPC());
OTV0P2BASE::serialPrintlnAndFlush();
#endif

          // Move to normal valve running state...
          needsRecalibrating = false;
          currentPC = 100; // Valve is currently fully open.
          // Reset tick count.
          ticksFromOpen = 0;
          ticksReverse = 0;
          changeState(valveNormal);
          break;
          }
        // In case of unexpected microstate shut down gracefully.
        default: { changeState(valveError); break; }
        }
      break;
      }

    // Normal running state: attempt to track the specified target valve open percentage.
    case valveNormal:
      {
//V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("  valveNormal");

      // If the current estimated position matches the target
      // then there is usually nothing to do.
      if(currentPC == targetPC) { break; }

      // Recalibrate if a serious tracking error was detected.
      if(needsRecalibrating)
        {
#if 0 && defined(V0P2BASE_DEBUG)
V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("!needsRecalibrating");
#endif

        // Defer calibration if doing it now would be a bad idea, eg in a bedroom at night.
        if(!shouldDeferCalibration())
            {
            changeState(valveCalibrating);
            break;
            }
        }

      // If the current estimated position does NOT match the target
      // then (incrementally) try to adjust to match.
#if 0 && defined(V0P2BASE_DEBUG)
V0P2BASE_DEBUG_SERIAL_PRINT_FLASHSTRING("  valve err: @");
V0P2BASE_DEBUG_SERIAL_PRINT(currentPC);
V0P2BASE_DEBUG_SERIAL_PRINT_FLASHSTRING(" vs target ");
V0P2BASE_DEBUG_SERIAL_PRINT(targetPC);
V0P2BASE_DEBUG_SERIAL_PRINTLN();
#endif

      // If true, running directly to end-stops and not doing any dead-reckoning.
      const bool binaryMode = inNonProprtionalMode();
      // If in binary mode, should the valve be fully open.
      // Set to the same threshold value used to trigger boiler call for heat.
      const bool binaryOpen = (targetPC >= OTRadValve::DEFAULT_VALVE_PC_SAFER_OPEN);

      // Refuse to close the valve while supply voltage low to try to avoid browning out or leaving valve shut.
      const bool low = ((NULL != lowBattOpt) && ((0 == lowBattOpt->read()) || lowBattOpt->isSupplyVoltageLow()));
      if(low && (targetPC < currentPC)) { break; }

      // Special case where target is an end-point (or close to).
      // Run fast to the end-stop.
      // Be eager and pull to end stop if near for continuous auto-recalibration.
      // Must work when eps is zero (ie with sub-percent precision).
      const uint8_t eps = cp.getApproxPrecisionPC();
      const bool toOpenFast = (binaryMode && binaryOpen) || (targetPC >= (100 - 2*eps));
      if(binaryMode || toOpenFast || (targetPC <= OTV0P2BASE::fnmax(2*eps, minOpenPC>>1)))
        {
        // If not apparently yet at end-stop
        // (ie not at correct end stop or with spurious unreconciled ticks)
        // then try again to run to end-stop.
        if(!binaryMode && (0 == ticksReverse) && (currentPC == (toOpenFast ? 100 : 0))) { break; } // Done
        else if(runTowardsEndStop(toOpenFast, low))
            {
            // TODO: may need to protect against spurious stickiness before end...
            // Reset positional values.
            currentPC = toOpenFast ? 100 : 0;
            ticksReverse = 0;
            ticksFromOpen = toOpenFast ? 0 : cp.getTicksFromOpenToClosed();
            }
        // Estimate intermediate position.
        else { recomputePosition(); }
#if 0 && defined(V0P2BASE_DEBUG)
if(toOpenFast) { V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("-->"); } else { V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("--<"); }
#endif
        break;
        }

      // More general case where target position is somewhere between end-stops.
      // Don't do anything if close enough, ie within computed precision (eps).
      // Else move incrementally to reduce the error.
      // (Incremental small moves may also help when absolute accuracy not that good,
      // allowing closed-loop feedback time to work.)

      // Not open enough.
      if((targetPC > currentPC) && (targetPC >= currentPC + eps)) // Overflow not possible with eps addition.
        {
        // TODO: use shaft encoder positioning by preference, ie when available.
        const bool hitEndStop = runTowardsEndStop(true);
        recomputePosition();
        // Hit the end-stop, possibly prematurely.
        if(hitEndStop)
          {
          // Report serious tracking error (well before 'fairly open' %).
          if(currentPC < OTV0P2BASE::fnmin(fairlyOpenPC, (uint8_t)(100 - 8*eps)))
            { reportTrackingError(); }
          // Silently auto-adjust when end-stop hit close to expected position.
          else
            {
            currentPC = 100;
            ticksReverse = 0;
            ticksFromOpen = 0;
            }
          }
#if 0 && defined(V0P2BASE_DEBUG)
V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("->");
#endif
        break;
        }
      // Not closed enough.
      else if((targetPC < currentPC) && (targetPC + eps <= currentPC)) // Overflow not possible with eps addition.
        {
        // TODO: use shaft encoder positioning by preference, ie when available.
        const bool hitEndStop = runTowardsEndStop(false);
        recomputePosition();
        // Hit the end-stop, possibly prematurely.
        if(hitEndStop)
          {
          // Report serious tracking error.
//          if(currentPC > max(min(DEFAULT_VALVE_PC_MODERATELY_OPEN-1, 2*DEFAULT_VALVE_PC_MODERATELY_OPEN), 8*eps))
          if(currentPC > OTV0P2BASE::fnmax(2*minOpenPC, 8*eps))
            { reportTrackingError(); }
          // Silently auto-adjust when end-stop hit close to expected position.
          else
            {
            currentPC = 0;
            ticksReverse = 0;
            ticksFromOpen = cp.getTicksFromOpenToClosed();
            }
          }
#if 0 && defined(V0P2BASE_DEBUG)
V0P2BASE_DEBUG_SERIAL_PRINTLN_FLASHSTRING("-<");
#endif
        break;
        }

      // Within eps; do nothing.

      break;
      }

    // Unexpected: go to error state, stop motor and report error on serial.
    default:
      {
      changeState(valveError);
      hw->motorRun(0, OTRadValve::HardwareMotorDriverInterface::motorOff, *this);
      OTV0P2BASE::serialPrintlnAndFlush(F("!valve error"));
      //panic(); // FIXME // Not expected to return.
      return;
      }
    }
  }

#endif // CurrentSenseValveMotorDirect_DEFINED


}

