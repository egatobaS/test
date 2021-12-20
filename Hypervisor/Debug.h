#pragma once


/******************** Public Typedefs ********************/

/******************** Public Constants ********************/

#define _DEBUG

#ifdef _DEBUG 
#define DEBUG_PRINT( X, ... )                                                                                           \
	DbgPrintEx( DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[P] [%04i] " X,                                          \
		   1, __VA_ARGS__ )

	//#define DEBUG_PRINT(format, ...) DbgPrint(format, ##__VA_ARGS__)
#else
	#define `(format, ...)
#endif

/******************** Public Variables ********************/

/******************** Public Prototypes ********************/
