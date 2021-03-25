// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// Header: include\myst\clock.h
// From: _clock.c


#[repr(C)]
pub struct clock_ctrl {
    realtime0: 	i64,
	monotime0:	i64,
	now:		i64,
	interval:	u64,
	done:		i32,
}

#[no_mangle]
pub extern "C" fn myst_setup_clock(ctrl: &mut clock_ctrl) -> i32 {
}
