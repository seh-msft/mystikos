// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// Header: include/myst/clock.h
// From: tools/myst/enc/_clock.c
use libc::*;

pub const NANO_IN_SECOND: c_int = 1000000000;
pub const MICRO_IN_SECOND: c_int = 1000000;

#[repr(C)]
pub struct clock_ctrl {
    realtime0: 	c_long,
	monotime0:	c_long,
	now:		c_long,
	interval:	c_ulong,
	done:		c_int,
}

#[no_mangle]
pub extern "C" fn myst_setup_clock(ctrl: &mut clock_ctrl) -> c_int {
    -1
}
