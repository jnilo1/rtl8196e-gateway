
/*
==========================================================================
; Copyright (C) 2000,2010 Global View Corporation
;
; Module Name:
;		monitor.h
;
; Description:
;		Monitor global constant define
;
; author:
;	Steve Chen
;
; Revision History:
;	2000/12/26		: Initial release
===========================================================================
*/
#ifndef _MONITOR_H_
#define _MONITOR_H_

#define MAX_ARGV 20
#define MAX_MONITOR_BUFFER 128

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Command Table */
typedef struct {
	const char *cmd; // Input command string
	int n_arg;
	int (*func)(int argc, char *argv[]);
	const char *msg; // Help message
} COMMAND_TABLE;

#endif
