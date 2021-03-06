/*
  grbllib.c - An embedded CNC Controller with rs274/ngc (g-code) support
  Part of Grbl

  Copyright (c) 2017-2018 Terje Io
  Copyright (c) 2011-2015 Sungeun K. Jeon
  Copyright (c) 2009-2011 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "grbl.h"
// #include "noeeprom.h" // uncomment to enable EEPROM emulation

// Declare system global variable structure
system_t sys;
int32_t sys_position[N_AXIS];               // Real-time machine (aka home) position vector in steps.
int32_t sys_probe_position[N_AXIS];         // Last probe position in machine coordinates and steps.
bool mpg_init;                              // Enter MPG mode on startup?
volatile probe_state_t sys_probe_state;     // Probing state value.  Used to coordinate the probing cycle with stepper ISR.
volatile uint_fast16_t sys_rt_exec_state;   // Global realtime executor bitflag variable for state management. See EXEC bitmasks.
volatile uint_fast16_t sys_rt_exec_alarm;   // Global realtime executor bitflag variable for setting various alarms.

HAL hal;

// called from stream drivers while tx is blocking, return false to terminate

static bool stream_tx_blocking (void)
{
    // TODO: Restructure st_prep_buffer() calls to be executed here during a long print.
    return !(sys_rt_exec_state & EXEC_RESET);
}

#ifdef DEBUGOUT
static void debug_out (bool on)
{
    // NOOP
}
#endif

// main entry point

int grbl_enter (void)
{
#ifdef N_TOOLS
    assert(EEPROM_ADDR_GLOBAL + sizeof(settings_t) + 1 < EEPROM_ADDR_TOOL_TABLE);
    assert(EEPROM_ADDR_TOOL_TABLE + N_TOOLS * (sizeof(tool_data_t) + 1) < EEPROM_ADDR_PARAMETERS);
#else
    assert(EEPROM_ADDR_GLOBAL + sizeof(settings_t) + 1 < EEPROM_ADDR_PARAMETERS);
#endif
    assert(EEPROM_ADDR_PARAMETERS + SETTING_INDEX_NCOORD * (sizeof(coord_data_t) + 1) < EEPROM_ADDR_STARTUP_BLOCK);
    assert(EEPROM_ADDR_STARTUP_BLOCK + N_STARTUP_LINE * (MAX_STORED_LINE_LENGTH + 1) < EEPROM_ADDR_BUILD_INFO);

	bool looping = true, driver_ok;

	memset(&hal, 0, sizeof(HAL));  // Clear...

	hal.version = HAL_VERSION; // Update when signatures and/or contract is changed - driver_init() should fail
	hal.limit_interrupt_callback = limit_interrupt_handler;
	hal.control_interrupt_callback = control_interrupt_handler;
	hal.stepper_interrupt_callback = stepper_driver_interrupt_handler;
	hal.protocol_process_realtime = protocol_process_realtime;
	hal.stream_blocking_callback = stream_tx_blocking;
	hal.protocol_enqueue_gcode = protocol_enqueue_gcode;
    hal.report_status_message = report_status_message;

#ifdef DEBUGOUT
	hal.debug_out = debug_out; // must be overridden by driver to have any effect
#endif
	driver_ok = driver_init();

  #ifdef EMULATE_EEPROM
	eeprom_emu_init();
  #endif
	settings_init(); // Load Grbl settings from EEPROM

	memset(sys_position, 0, sizeof(sys_position)); // Clear machine position.

// check and configure driver

#ifdef ADAPTIVE_MULTI_AXIS_STEP_SMOOTHING
    driver_ok = driver_ok && hal.driver_cap.amass_level >= MAX_AMASS_LEVEL;
    hal.driver_cap.amass_level = MAX_AMASS_LEVEL;
#else
    hal.driver_cap.amass_level = 0;
#endif

#if DEFAULT_STEP_PULSE_DELAY > 0
    driver_ok = driver_ok & hal.driver_cap.step_pulse_delay;
#endif
/*
#if AXIS_N_SETTINGS > 4
    driver_ok = driver_ok & hal.driver_cap.axes >= AXIS_N_SETTINGS;
#endif
*/
#ifdef CONSTANT_SURFACE_SPEED_OPTION
    driver_ok = driver_ok & hal.driver_cap.constant_surface_speed;
#endif

    sys.mpg_mode = false;
    sys.message = NULL;

    driver_ok = driver_ok && hal.driver_setup(&settings);

    if(!driver_ok) {
        hal.stream_write("GrblHAL: incompatible driver\r\n");
        while(true);
    }

    mpg_init = sys.mpg_mode;

    if(hal.get_position)
        hal.get_position(&sys_position); // TODO:  restore on abort when returns true?

    // Initialize system state.
    sys.state = settings.flags.force_initialization_alarm ? STATE_ALARM : STATE_IDLE;

    // Check for power-up and set system alarm if homing is enabled to force homing cycle
    // by setting Grbl's alarm state. Alarm locks out all g-code commands, including the
    // startup scripts, but allows access to settings and internal commands. Only a homing
    // cycle '$H' or kill alarm locks '$X' will disable the alarm.
    // NOTE: The startup script will run after successful completion of the homing cycle, but
    // not after disabling the alarm locks. Prevents motion startup blocks from crashing into
    // things uncontrollably. Very bad.
    if (settings.homing.flags.enabled && settings.homing.flags.init_lock)
        sys.state = STATE_ALARM;

    if(hal.system_control_get_state().reset)
        sys.state = STATE_ALARM;

    // Grbl initialization loop upon power-up or a system abort. For the latter, all processes
    // will return to this loop to be cleanly re-initialized.
    while(looping) {

        // Reset entry points
        hal.report_status_message = report_status_message;

		// Reset system variables.
		uint_fast16_t prior_state = sys.state;

        if(sys.message)
            free(sys.message);

		memset(&sys, 0, sizeof(system_t)); // Clear system struct variable.
		set_state(prior_state);
        sys.override.feed_rate = DEFAULT_FEED_OVERRIDE;          // Set to 100%
        sys.override.rapid_rate = DEFAULT_RAPID_OVERRIDE;       // Set to 100%
        sys.override.spindle_rpm = DEFAULT_SPINDLE_RPM_OVERRIDE; // Set to 100%

		if(settings.parking.flags.enabled)
		    sys.override.control.parking_disable = settings.parking.flags.deactivate_upon_init;

		memset(sys_probe_position, 0, sizeof(sys_probe_position)); // Clear probe position.
		sys_probe_state = Probe_Off;
		sys_rt_exec_state = 0;
		sys_rt_exec_alarm = 0;

		flush_override_buffers();

		// Reset Grbl primary systems.
		hal.stream_reset_read_buffer(); // Clear input stream buffer
		gc_init(); // Set g-code parser to default state
		hal.limits_enable(settings.limits.flags.hard_enabled, false);
		plan_reset(); // Clear block buffer and planner variables
		st_reset(); // Clear stepper subsystem variables.

		// Sync cleared gcode and planner positions to current system position.
		plan_sync_position();
		gc_sync_position();

		// Print welcome message. Indicates an initialization has occured at power-up or with a reset.
		report_init_message();

        if(hal.system_control_get_state().e_stop)
            set_state(STATE_ESTOP);
        else if(sys.state == STATE_ESTOP)
            set_state(STATE_ALARM);

		if((sys.mpg_mode = sys.report.mpg_mode = mpg_init)) {
		    mpg_init = false;
            report_realtime_status();
		}

		// Start Grbl main loop. Processes program inputs and executes them.
		if(!(looping = protocol_main_loop()))
			looping = hal.driver_release == 0 || hal.driver_release();

		sys_rt_exec_state = 0;
    }

    return 0;
}
