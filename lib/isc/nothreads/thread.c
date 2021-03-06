/*
 * Copyright (C) 2000, 2001, 2004, 2007, 2016, 2017  Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/* $Id: thread.c,v 1.5 2007/06/19 23:47:18 tbox Exp $ */

#include <config.h>

#include <isc/thread.h>
#include <isc/util.h>

void
isc_thread_setconcurrency(unsigned int level) {
	UNUSED(level);
}

void isc_thread_setname(isc_thread_t thread, const char *name) {
	UNUSED(thread);
	UNUSED(name);
}
