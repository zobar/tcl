/*
 * tclExecute.c --
 *
 *	This file contains procedures that execute byte-compiled Tcl commands.
 *
 * Copyright (c) 1996-1997 Sun Microsystems, Inc.
 * Copyright (c) 1998-2000 by Scriptics Corporation.
 * Copyright (c) 2001 by Kevin B. Kenny. All rights reserved.
 * Copyright (c) 2002-2010 by Miguel Sofer.
 * Copyright (c) 2005-2007 by Donal K. Fellows.
 * Copyright (c) 2007 Daniel A. Steffen <das@users.sourceforge.net>
 * Copyright (c) 2006-2008 by Joe Mistachkin.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"
#include "tclCompile.h"
#include "tclOOInt.h"
#include "tommath.h"
#include <math.h>

#if NRE_ENABLE_ASSERTS
#include <assert.h>
#endif

/*
 * Hack to determine whether we may expect IEEE floating point. The hack is
 * formally incorrect in that non-IEEE platforms might have the same precision
 * and range, but VAX, IBM, and Cray do not; are there any other floating
 * point units that we might care about?
 */

#if (FLT_RADIX == 2) && (DBL_MANT_DIG == 53) && (DBL_MAX_EXP == 1024)
#define IEEE_FLOATING_POINT
#endif

/*
 * A mask (should be 2**n-1) that is used to work out when the bytecode engine
 * should call Tcl_AsyncReady() to see whether there is a signal that needs
 * handling.
 */

#ifndef ASYNC_CHECK_COUNT_MASK
#   define ASYNC_CHECK_COUNT_MASK	63
#endif /* !ASYNC_CHECK_COUNT_MASK */

/*
 * Boolean flag indicating whether the Tcl bytecode interpreter has been
 * initialized.
 */

static int execInitialized = 0;
TCL_DECLARE_MUTEX(execMutex)

static int cachedInExit = 0;

#ifdef TCL_COMPILE_DEBUG
/*
 * Variable that controls whether execution tracing is enabled and, if so,
 * what level of tracing is desired:
 *    0: no execution tracing
 *    1: trace invocations of Tcl procs only
 *    2: trace invocations of all (not compiled away) commands
 *    3: display each instruction executed
 * This variable is linked to the Tcl variable "tcl_traceExec".
 */

int tclTraceExec = 0;
#endif

/*
 * Mapping from expression instruction opcodes to strings; used for error
 * messages. Note that these entries must match the order and number of the
 * expression opcodes (e.g., INST_LOR) in tclCompile.h.
 *
 * Does not include the string for INST_EXPON (and beyond), as that is
 * disjoint for backward-compatability reasons.
 */

static const char *const operatorStrings[] = {
    "||", "&&", "|", "^", "&", "==", "!=", "<", ">", "<=", ">=", "<<", ">>",
    "+", "-", "*", "/", "%", "+", "-", "~", "!",
    "BUILTIN FUNCTION", "FUNCTION",
    "", "", "", "", "", "", "", "", "eq", "ne"
};

/*
 * Mapping from Tcl result codes to strings; used for error and debugging
 * messages.
 */

#ifdef TCL_COMPILE_DEBUG
static const char *const resultStrings[] = {
    "TCL_OK", "TCL_ERROR", "TCL_RETURN", "TCL_BREAK", "TCL_CONTINUE"
};
#endif

/*
 * These are used by evalstats to monitor object usage in Tcl.
 */

#ifdef TCL_COMPILE_STATS
long		tclObjsAlloced = 0;
long		tclObjsFreed = 0;
long		tclObjsShared[TCL_MAX_SHARED_OBJ_STATS] = { 0, 0, 0, 0, 0 };
#endif /* TCL_COMPILE_STATS */

/*
 * Support pre-8.5 bytecodes unless specifically requested otherwise.
 */

#ifndef TCL_SUPPORT_84_BYTECODE
#define TCL_SUPPORT_84_BYTECODE 1
#endif

#if TCL_SUPPORT_84_BYTECODE
/*
 * We need to know the tclBuiltinFuncTable to support translation of pre-8.5
 * math functions to the namespace-based ::tcl::mathfunc::op in 8.5+.
 */

typedef struct {
    const char *name;		/* Name of function. */
    int numArgs;		/* Number of arguments for function. */
} BuiltinFunc;

/*
 * Table describing the built-in math functions. Entries in this table are
 * indexed by the values of the INST_CALL_BUILTIN_FUNC instruction's
 * operand byte.
 */

static BuiltinFunc const tclBuiltinFuncTable[] = {
    {"acos", 1},
    {"asin", 1},
    {"atan", 1},
    {"atan2", 2},
    {"ceil", 1},
    {"cos", 1},
    {"cosh", 1},
    {"exp", 1},
    {"floor", 1},
    {"fmod", 2},
    {"hypot", 2},
    {"log", 1},
    {"log10", 1},
    {"pow", 2},
    {"sin", 1},
    {"sinh", 1},
    {"sqrt", 1},
    {"tan", 1},
    {"tanh", 1},
    {"abs", 1},
    {"double", 1},
    {"int", 1},
    {"rand", 0},
    {"round", 1},
    {"srand", 1},
    {"wide", 1},
    {NULL, 0},
};

#define LAST_BUILTIN_FUNC	25
#endif

/*
 * NR_TEBC
 * Helpers for NR - non-recursive calls to TEBC
 * Minimal data required to fully reconstruct the execution state.
 */

typedef struct TEBCdata {
    ByteCode *codePtr;		/* Constant until the BC returns */
				/* -----------------------------------------*/
    const unsigned char *pc;	/* These fields are used on return TO this */
    ptrdiff_t *catchTop;	/* this level: they record the state when a */
    int cleanup;		/* new codePtr was received for NR */
    Tcl_Obj *auxObjList;	/* execution. */
    int checkInterp;
    CmdFrame cmdFrame;
    void *stack[1];		/* Start of the actual combined catch and obj
				 * stacks; the struct will be expanded as
				 * necessary */
} TEBCdata;

#define TEBC_YIELD() \
    do {								\
	esPtr->tosPtr = tosPtr;						\
	TD->pc = pc;							\
	TD->cleanup = cleanup;						\
	TclNRAddCallback(interp, TEBCresume, TD, INT2PTR(1), NULL, NULL); \
    } while (0)

#define TEBC_DATA_DIG() \
    do {					\
	pc = TD->pc;				\
	cleanup = TD->cleanup;			\
	tosPtr = esPtr->tosPtr;			\
    } while (0)

#define PUSH_TAUX_OBJ(objPtr) \
    do {							\
	objPtr->internalRep.ptrAndLongRep.ptr = auxObjList;	\
	auxObjList = objPtr;					\
    } while (0)

#define POP_TAUX_OBJ() \
    do {							\
	tmpPtr = auxObjList;					\
	auxObjList = tmpPtr->internalRep.ptrAndLongRep.ptr;	\
	Tcl_DecrRefCount(tmpPtr);				\
    } while (0)

/*
 * These variable-access macros have to coincide with those in tclVar.c
 */

#define VarHashGetValue(hPtr) \
    ((Var *) ((char *)hPtr - TclOffset(VarInHash, entry)))

static inline Var *
VarHashCreateVar(
    TclVarHashTable *tablePtr,
    Tcl_Obj *key,
    int *newPtr)
{
    Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&tablePtr->table,
	    key, newPtr);

    if (!hPtr) {
	return NULL;
    }
    return VarHashGetValue(hPtr);
}

#define VarHashFindVar(tablePtr, key) \
    VarHashCreateVar((tablePtr), (key), NULL)

/*
 * The new macro for ending an instruction; note that a reasonable C-optimiser
 * will resolve all branches at compile time. (result) is always a constant;
 * the macro NEXT_INST_F handles constant (nCleanup), NEXT_INST_V is resolved
 * at runtime for variable (nCleanup).
 *
 * ARGUMENTS:
 *    pcAdjustment: how much to increment pc
 *    nCleanup: how many objects to remove from the stack
 *    resultHandling: 0 indicates no object should be pushed on the stack;
 *	otherwise, push objResultPtr. If (result < 0), objResultPtr already
 *	has the correct reference count.
 *
 * We use the new compile-time assertions to cheack that nCleanup is constant
 * and within range.
 */

#define NEXT_INST_F(pcAdjustment, nCleanup, resultHandling) \
    do {							\
	TCL_CT_ASSERT((nCleanup >= 0) && (nCleanup <= 2));	\
	if (nCleanup == 0) {					\
	    if (resultHandling != 0) {				\
		if ((resultHandling) > 0) {			\
		    PUSH_OBJECT(objResultPtr);			\
		} else {					\
		    *(++tosPtr) = objResultPtr;			\
		}						\
	    }							\
	    pc += (pcAdjustment);				\
	    goto cleanup0;					\
	} else if (resultHandling != 0) {			\
	    if ((resultHandling) > 0) {				\
		Tcl_IncrRefCount(objResultPtr);			\
	    }							\
	    pc += (pcAdjustment);				\
	    switch (nCleanup) {					\
	    case 1: goto cleanup1_pushObjResultPtr;		\
	    case 2: goto cleanup2_pushObjResultPtr;		\
	    }							\
	} else {						\
	    pc += (pcAdjustment);				\
	    switch (nCleanup) {					\
	    case 1: goto cleanup1;				\
	    case 2: goto cleanup2;				\
	    }							\
	}							\
    } while (0)

#define NEXT_INST_V(pcAdjustment, nCleanup, resultHandling) \
    do {							\
	pc += (pcAdjustment);					\
	cleanup = (nCleanup);					\
	if (resultHandling) {					\
	    if ((resultHandling) > 0) {				\
		Tcl_IncrRefCount(objResultPtr);			\
	    }							\
	    goto cleanupV_pushObjResultPtr;			\
	} else {						\
	    goto cleanupV;					\
	}							\
    } while (0)

/*
 * Macros used to cache often-referenced Tcl evaluation stack information
 * in local variables. Note that a DECACHE_STACK_INFO()-CACHE_STACK_INFO()
 * pair must surround any call inside TclNRExecuteByteCode (and a few other
 * procedures that use this scheme) that could result in a recursive call
 * to TclNRExecuteByteCode.
 */

#define CACHE_STACK_INFO() \
    checkInterp = 1

#define DECACHE_STACK_INFO() \
    esPtr->tosPtr = tosPtr

/*
 * Macros used to access items on the Tcl evaluation stack. PUSH_OBJECT
 * increments the object's ref count since it makes the stack have another
 * reference pointing to the object. However, POP_OBJECT does not decrement
 * the ref count. This is because the stack may hold the only reference to the
 * object, so the object would be destroyed if its ref count were decremented
 * before the caller had a chance to, e.g., store it in a variable. It is the
 * caller's responsibility to decrement the ref count when it is finished with
 * an object.
 *
 * WARNING! It is essential that objPtr only appear once in the PUSH_OBJECT
 * macro. The actual parameter might be an expression with side effects, and
 * this ensures that it will be executed only once.
 */

#define PUSH_OBJECT(objPtr) \
    Tcl_IncrRefCount(*(++tosPtr) = (objPtr))

#define POP_OBJECT()	*(tosPtr--)

#define OBJ_AT_TOS	*tosPtr

#define OBJ_UNDER_TOS	*(tosPtr-1)

#define OBJ_AT_DEPTH(n)	*(tosPtr-(n))

#define CURR_DEPTH	((ptrdiff_t) (tosPtr - initTosPtr))

/*
 * Macros used to trace instruction execution. The macros TRACE,
 * TRACE_WITH_OBJ, and O2S are only used inside TclNRExecuteByteCode. O2S is
 * only used in TRACE* calls to get a string from an object.
 */

#ifdef TCL_COMPILE_DEBUG
#   define TRACE(a) \
    while (traceInstructions) {					\
	fprintf(stdout, "%2d: %2d (%u) %s ", iPtr->numLevels,	\
		(int) CURR_DEPTH,				\
		(unsigned) (pc - codePtr->codeStart),		\
		GetOpcodeName(pc));				\
	printf a;						\
	break;							\
    }
#   define TRACE_APPEND(a) \
    while (traceInstructions) {		\
	printf a;			\
	break;				\
    }
#   define TRACE_WITH_OBJ(a, objPtr) \
    while (traceInstructions) {					\
	fprintf(stdout, "%2d: %2d (%u) %s ", iPtr->numLevels,	\
		(int) CURR_DEPTH,				\
		(unsigned) (pc - codePtr->codeStart),		\
		GetOpcodeName(pc));				\
	printf a;						\
	TclPrintObject(stdout, objPtr, 30);			\
	fprintf(stdout, "\n");					\
	break;							\
    }
#   define O2S(objPtr) \
    (objPtr ? TclGetString(objPtr) : "")
#else /* !TCL_COMPILE_DEBUG */
#   define TRACE(a)
#   define TRACE_APPEND(a)
#   define TRACE_WITH_OBJ(a, objPtr)
#   define O2S(objPtr)
#endif /* TCL_COMPILE_DEBUG */

/*
 * DTrace instruction probe macros.
 */

#define TCL_DTRACE_INST_NEXT() \
    do {								\
	if (TCL_DTRACE_INST_DONE_ENABLED()) {				\
	    if (curInstName) {						\
		TCL_DTRACE_INST_DONE(curInstName, (int) CURR_DEPTH,	\
			tosPtr);					\
	    }								\
	    curInstName = tclInstructionTable[*pc].name;		\
	    if (TCL_DTRACE_INST_START_ENABLED()) {			\
		TCL_DTRACE_INST_START(curInstName, (int) CURR_DEPTH,	\
			tosPtr);					\
	    }								\
	} else if (TCL_DTRACE_INST_START_ENABLED()) {			\
	    TCL_DTRACE_INST_START(tclInstructionTable[*pc].name,	\
			(int) CURR_DEPTH, tosPtr);			\
	}								\
    } while (0)
#define TCL_DTRACE_INST_LAST() \
    do {								\
	if (TCL_DTRACE_INST_DONE_ENABLED() && curInstName) {		\
	    TCL_DTRACE_INST_DONE(curInstName, (int) CURR_DEPTH, tosPtr);\
	}								\
    } while (0)

/*
 * Macro used in this file to save a function call for common uses of
 * TclGetNumberFromObj(). The ANSI C "prototype" is:
 *
 * MODULE_SCOPE int GetNumberFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
 *			ClientData *ptrPtr, int *tPtr);
 */

#ifdef NO_WIDE_TYPE
#define GetNumberFromObj(interp, objPtr, ptrPtr, tPtr) \
    (((objPtr)->typePtr == &tclIntType)					\
	?	(*(tPtr) = TCL_NUMBER_LONG,				\
		*(ptrPtr) = (ClientData)				\
		    (&((objPtr)->internalRep.longValue)), TCL_OK) :	\
    ((objPtr)->typePtr == &tclDoubleType)				\
	?	(((TclIsNaN((objPtr)->internalRep.doubleValue))		\
		    ?	(*(tPtr) = TCL_NUMBER_NAN)			\
		    :	(*(tPtr) = TCL_NUMBER_DOUBLE)),			\
		*(ptrPtr) = (ClientData)				\
		    (&((objPtr)->internalRep.doubleValue)), TCL_OK) :	\
    ((((objPtr)->typePtr == NULL) && ((objPtr)->bytes == NULL)) ||	\
    (((objPtr)->bytes != NULL) && ((objPtr)->length == 0)))		\
	? TCL_ERROR :							\
    TclGetNumberFromObj((interp), (objPtr), (ptrPtr), (tPtr)))
#else /* !NO_WIDE_TYPE */
#define GetNumberFromObj(interp, objPtr, ptrPtr, tPtr) \
    (((objPtr)->typePtr == &tclIntType)					\
	?	(*(tPtr) = TCL_NUMBER_LONG,				\
		*(ptrPtr) = (ClientData)				\
		    (&((objPtr)->internalRep.longValue)), TCL_OK) :	\
    ((objPtr)->typePtr == &tclWideIntType)				\
	?	(*(tPtr) = TCL_NUMBER_WIDE,				\
		*(ptrPtr) = (ClientData)				\
		    (&((objPtr)->internalRep.wideValue)), TCL_OK) :	\
    ((objPtr)->typePtr == &tclDoubleType)				\
	?	(((TclIsNaN((objPtr)->internalRep.doubleValue))		\
		    ?	(*(tPtr) = TCL_NUMBER_NAN)			\
		    :	(*(tPtr) = TCL_NUMBER_DOUBLE)),			\
		*(ptrPtr) = (ClientData)				\
		    (&((objPtr)->internalRep.doubleValue)), TCL_OK) :	\
    ((((objPtr)->typePtr == NULL) && ((objPtr)->bytes == NULL)) ||	\
    (((objPtr)->bytes != NULL) && ((objPtr)->length == 0)))		\
	? TCL_ERROR :							\
    TclGetNumberFromObj((interp), (objPtr), (ptrPtr), (tPtr)))
#endif /* NO_WIDE_TYPE */

/*
 * Macro used in this file to save a function call for common uses of
 * Tcl_GetBooleanFromObj(). The ANSI C "prototype" is:
 *
 * MODULE_SCOPE int TclGetBooleanFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
 *			int *boolPtr);
 */

#define TclGetBooleanFromObj(interp, objPtr, boolPtr) \
    ((((objPtr)->typePtr == &tclIntType)				\
	|| ((objPtr)->typePtr == &tclBooleanType))			\
	? (*(boolPtr) = ((objPtr)->internalRep.longValue!=0), TCL_OK)	\
	: Tcl_GetBooleanFromObj((interp), (objPtr), (boolPtr)))

/*
 * Macro used in this file to save a function call for common uses of
 * Tcl_GetWideIntFromObj(). The ANSI C "prototype" is:
 *
 * MODULE_SCOPE int TclGetWideIntFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
 *			Tcl_WideInt *wideIntPtr);
 */

#ifdef NO_WIDE_TYPE
#define TclGetWideIntFromObj(interp, objPtr, wideIntPtr) \
    (((objPtr)->typePtr == &tclIntType)					\
	? (*(wideIntPtr) = (Tcl_WideInt)				\
		((objPtr)->internalRep.longValue), TCL_OK) :		\
	Tcl_GetWideIntFromObj((interp), (objPtr), (wideIntPtr)))
#else /* !NO_WIDE_TYPE */
#define TclGetWideIntFromObj(interp, objPtr, wideIntPtr)		\
    (((objPtr)->typePtr == &tclWideIntType)				\
	? (*(wideIntPtr) = (objPtr)->internalRep.wideValue, TCL_OK) :	\
    ((objPtr)->typePtr == &tclIntType)					\
	? (*(wideIntPtr) = (Tcl_WideInt)				\
		((objPtr)->internalRep.longValue), TCL_OK) :		\
	Tcl_GetWideIntFromObj((interp), (objPtr), (wideIntPtr)))
#endif /* NO_WIDE_TYPE */

/*
 * Macro used to make the check for type overflow more mnemonic. This works by
 * comparing sign bits; the rest of the word is irrelevant. The ANSI C
 * "prototype" (where inttype_t is any integer type) is:
 *
 * MODULE_SCOPE int Overflowing(inttype_t a, inttype_t b, inttype_t sum);
 *
 * Check first the condition most likely to fail in usual code (at least for
 * usage in [incr]: do the first summand and the sum have != signs?
 */

#define Overflowing(a,b,sum) ((((a)^(sum)) < 0) && (((a)^(b)) >= 0))

/*
 * Macro for checking whether the type is NaN, used when we're thinking about
 * throwing an error for supplying a non-number number.
 */

#ifndef ACCEPT_NAN
#define IsErroringNaNType(type)		((type) == TCL_NUMBER_NAN)
#else
#define IsErroringNaNType(type)		0
#endif

/*
 * Auxiliary tables used to compute powers of small integers.
 */

#if (LONG_MAX == 0x7fffffff)

/*
 * Maximum base that, when raised to powers 2, 3, ... 8, fits in a 32-bit
 * signed integer.
 */

static const long MaxBase32[] = {46340, 1290, 215, 73, 35, 21, 14};
static const size_t MaxBase32Size = sizeof(MaxBase32)/sizeof(long);

/*
 * Table giving 3, 4, ..., 11, raised to the powers 9, 10, ..., as far as they
 * fit in a 32-bit signed integer. Exp32Index[i] gives the starting index of
 * powers of i+3; Exp32Value[i] gives the corresponding powers.
 */

static const unsigned short Exp32Index[] = {
    0, 11, 18, 23, 26, 29, 31, 32, 33
};
static const size_t Exp32IndexSize =
    sizeof(Exp32Index) / sizeof(unsigned short);
static const long Exp32Value[] = {
    19683, 59049, 177147, 531441, 1594323, 4782969, 14348907, 43046721,
    129140163, 387420489, 1162261467, 262144, 1048576, 4194304,
    16777216, 67108864, 268435456, 1073741824, 1953125, 9765625,
    48828125, 244140625, 1220703125, 10077696, 60466176, 362797056,
    40353607, 282475249, 1977326743, 134217728, 1073741824, 387420489,
    1000000000
};
static const size_t Exp32ValueSize = sizeof(Exp32Value)/sizeof(long);
#endif /* LONG_MAX == 0x7fffffff -- 32 bit machine */

#if (LONG_MAX > 0x7fffffff) || !defined(TCL_WIDE_INT_IS_LONG)

/*
 * Maximum base that, when raised to powers 2, 3, ..., 16, fits in a
 * Tcl_WideInt.
 */

static const Tcl_WideInt MaxBase64[] = {
    (Tcl_WideInt)46340*65536+62259,	/* 3037000499 == isqrt(2**63-1) */
    (Tcl_WideInt)2097151, (Tcl_WideInt)55108, (Tcl_WideInt)6208,
    (Tcl_WideInt)1448, (Tcl_WideInt)511, (Tcl_WideInt)234, (Tcl_WideInt)127,
    (Tcl_WideInt)78, (Tcl_WideInt)52, (Tcl_WideInt)38, (Tcl_WideInt)28,
    (Tcl_WideInt)22, (Tcl_WideInt)18, (Tcl_WideInt)15
};
static const size_t MaxBase64Size = sizeof(MaxBase64)/sizeof(Tcl_WideInt);

/*
 * Table giving 3, 4, ..., 13 raised to powers greater than 16 when the
 * results fit in a 64-bit signed integer.
 */

static const unsigned short Exp64Index[] = {
    0, 23, 38, 49, 57, 63, 67, 70, 72, 74, 75, 76
};
static const size_t Exp64IndexSize =
    sizeof(Exp64Index) / sizeof(unsigned short);
static const Tcl_WideInt Exp64Value[] = {
    (Tcl_WideInt)243*243*243*3*3,
    (Tcl_WideInt)243*243*243*3*3*3,
    (Tcl_WideInt)243*243*243*3*3*3*3,
    (Tcl_WideInt)243*243*243*243,
    (Tcl_WideInt)243*243*243*243*3,
    (Tcl_WideInt)243*243*243*243*3*3,
    (Tcl_WideInt)243*243*243*243*3*3*3,
    (Tcl_WideInt)243*243*243*243*3*3*3*3,
    (Tcl_WideInt)243*243*243*243*243,
    (Tcl_WideInt)243*243*243*243*243*3,
    (Tcl_WideInt)243*243*243*243*243*3*3,
    (Tcl_WideInt)243*243*243*243*243*3*3*3,
    (Tcl_WideInt)243*243*243*243*243*3*3*3*3,
    (Tcl_WideInt)243*243*243*243*243*243,
    (Tcl_WideInt)243*243*243*243*243*243*3,
    (Tcl_WideInt)243*243*243*243*243*243*3*3,
    (Tcl_WideInt)243*243*243*243*243*243*3*3*3,
    (Tcl_WideInt)243*243*243*243*243*243*3*3*3*3,
    (Tcl_WideInt)243*243*243*243*243*243*243,
    (Tcl_WideInt)243*243*243*243*243*243*243*3,
    (Tcl_WideInt)243*243*243*243*243*243*243*3*3,
    (Tcl_WideInt)243*243*243*243*243*243*243*3*3*3,
    (Tcl_WideInt)243*243*243*243*243*243*243*3*3*3*3,
    (Tcl_WideInt)1024*1024*1024*4*4,
    (Tcl_WideInt)1024*1024*1024*4*4*4,
    (Tcl_WideInt)1024*1024*1024*4*4*4*4,
    (Tcl_WideInt)1024*1024*1024*1024,
    (Tcl_WideInt)1024*1024*1024*1024*4,
    (Tcl_WideInt)1024*1024*1024*1024*4*4,
    (Tcl_WideInt)1024*1024*1024*1024*4*4*4,
    (Tcl_WideInt)1024*1024*1024*1024*4*4*4*4,
    (Tcl_WideInt)1024*1024*1024*1024*1024,
    (Tcl_WideInt)1024*1024*1024*1024*1024*4,
    (Tcl_WideInt)1024*1024*1024*1024*1024*4*4,
    (Tcl_WideInt)1024*1024*1024*1024*1024*4*4*4,
    (Tcl_WideInt)1024*1024*1024*1024*1024*4*4*4*4,
    (Tcl_WideInt)1024*1024*1024*1024*1024*1024,
    (Tcl_WideInt)1024*1024*1024*1024*1024*1024*4,
    (Tcl_WideInt)3125*3125*3125*5*5,
    (Tcl_WideInt)3125*3125*3125*5*5*5,
    (Tcl_WideInt)3125*3125*3125*5*5*5*5,
    (Tcl_WideInt)3125*3125*3125*3125,
    (Tcl_WideInt)3125*3125*3125*3125*5,
    (Tcl_WideInt)3125*3125*3125*3125*5*5,
    (Tcl_WideInt)3125*3125*3125*3125*5*5*5,
    (Tcl_WideInt)3125*3125*3125*3125*5*5*5*5,
    (Tcl_WideInt)3125*3125*3125*3125*3125,
    (Tcl_WideInt)3125*3125*3125*3125*3125*5,
    (Tcl_WideInt)3125*3125*3125*3125*3125*5*5,
    (Tcl_WideInt)7776*7776*7776*6*6,
    (Tcl_WideInt)7776*7776*7776*6*6*6,
    (Tcl_WideInt)7776*7776*7776*6*6*6*6,
    (Tcl_WideInt)7776*7776*7776*7776,
    (Tcl_WideInt)7776*7776*7776*7776*6,
    (Tcl_WideInt)7776*7776*7776*7776*6*6,
    (Tcl_WideInt)7776*7776*7776*7776*6*6*6,
    (Tcl_WideInt)7776*7776*7776*7776*6*6*6*6,
    (Tcl_WideInt)16807*16807*16807*7*7,
    (Tcl_WideInt)16807*16807*16807*7*7*7,
    (Tcl_WideInt)16807*16807*16807*7*7*7*7,
    (Tcl_WideInt)16807*16807*16807*16807,
    (Tcl_WideInt)16807*16807*16807*16807*7,
    (Tcl_WideInt)16807*16807*16807*16807*7*7,
    (Tcl_WideInt)32768*32768*32768*8*8,
    (Tcl_WideInt)32768*32768*32768*8*8*8,
    (Tcl_WideInt)32768*32768*32768*8*8*8*8,
    (Tcl_WideInt)32768*32768*32768*32768,
    (Tcl_WideInt)59049*59049*59049*9*9,
    (Tcl_WideInt)59049*59049*59049*9*9*9,
    (Tcl_WideInt)59049*59049*59049*9*9*9*9,
    (Tcl_WideInt)100000*100000*100000*10*10,
    (Tcl_WideInt)100000*100000*100000*10*10*10,
    (Tcl_WideInt)161051*161051*161051*11*11,
    (Tcl_WideInt)161051*161051*161051*11*11*11,
    (Tcl_WideInt)248832*248832*248832*12*12,
    (Tcl_WideInt)371293*371293*371293*13*13
};
static const size_t Exp64ValueSize = sizeof(Exp64Value) / sizeof(Tcl_WideInt);
#endif /* (LONG_MAX > 0x7fffffff) || !defined(TCL_WIDE_INT_IS_LONG) */

/*
 * Markers for ExecuteExtendedBinaryMathOp.
 */

#define DIVIDED_BY_ZERO		((Tcl_Obj *) -1)
#define EXPONENT_OF_ZERO	((Tcl_Obj *) -2)
#define GENERAL_ARITHMETIC_ERROR ((Tcl_Obj *) -3)

/*
 * Declarations for local procedures to this file:
 */

#ifdef TCL_COMPILE_STATS
static int		EvalStatsCmd(ClientData clientData,
			    Tcl_Interp *interp, int objc,
			    Tcl_Obj *const objv[]);
#endif /* TCL_COMPILE_STATS */
#ifdef TCL_COMPILE_DEBUG
static const char *	GetOpcodeName(const unsigned char *pc);
static void		PrintByteCodeInfo(ByteCode *codePtr);
static const char *	StringForResultCode(int result);
static void		ValidatePcAndStackTop(ByteCode *codePtr,
			    const unsigned char *pc, int stackTop,
			    int stackLowerBound, int checkStack);
#endif /* TCL_COMPILE_DEBUG */
static ByteCode *	CompileExprObj(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void		DeleteExecStack(ExecStack *esPtr);
static void		DupExprCodeInternalRep(Tcl_Obj *srcPtr,
			    Tcl_Obj *copyPtr);
MODULE_SCOPE int	TclCompareTwoNumbers(Tcl_Obj *valuePtr,
			    Tcl_Obj *value2Ptr);
static Tcl_Obj *	ExecuteExtendedBinaryMathOp(Tcl_Interp *interp,
			    int opcode, Tcl_Obj **constants,
			    Tcl_Obj *valuePtr, Tcl_Obj *value2Ptr);
static Tcl_Obj *	ExecuteExtendedUnaryMathOp(int opcode,
			    Tcl_Obj *valuePtr);
static void		FreeExprCodeInternalRep(Tcl_Obj *objPtr);
static ExceptionRange *	GetExceptRangeForPc(const unsigned char *pc,
			    int catchOnly, ByteCode *codePtr);
static const char *	GetSrcInfoForPc(const unsigned char *pc,
			    ByteCode *codePtr, size_t *lengthPtr,
			    const unsigned char **pcBeg);
static Tcl_Obj **	GrowEvaluationStack(ExecEnv *eePtr, int growth,
			    int move);
static void		IllegalExprOperandType(Tcl_Interp *interp,
			    const unsigned char *pc, Tcl_Obj *opndPtr);
static void		InitByteCodeExecution(Tcl_Interp *interp);
static inline int	OFFSET(void *ptr);
static void		ReleaseDictIterator(Tcl_Obj *objPtr);
/* Useful elsewhere, make available in tclInt.h or stubs? */
static Tcl_Obj **	StackAllocWords(Tcl_Interp *interp, int numWords);
static Tcl_Obj **	StackReallocWords(Tcl_Interp *interp, int numWords);
static Tcl_NRPostProc	CopyCallback;
static Tcl_NRPostProc	ExprObjCallback;

static Tcl_NRPostProc   TEBCresume;

/*
 * The structure below defines a bytecode Tcl object type to hold the
 * compiled bytecode for Tcl expressions.
 */

static const Tcl_ObjType exprCodeType = {
    "exprcode",
    FreeExprCodeInternalRep,	/* freeIntRepProc */
    DupExprCodeInternalRep,	/* dupIntRepProc */
    NULL,			/* updateStringProc */
    NULL			/* setFromAnyProc */
};

/*
 * Custom object type only used in this file; values of its type should never
 * be seen by user scripts.
 */

static const Tcl_ObjType dictIteratorType = {
    "dictIterator",
    ReleaseDictIterator,
    NULL, NULL, NULL
};

/*
 *----------------------------------------------------------------------
 *
 * ReleaseDictIterator --
 *
 *	This takes apart a dictionary iterator that is stored in the given Tcl
 *	object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Deallocates memory, marks the object as being untyped.
 *
 *----------------------------------------------------------------------
 */

static void
ReleaseDictIterator(
    Tcl_Obj *objPtr)
{
    Tcl_DictSearch *searchPtr;
    Tcl_Obj *dictPtr;

    /*
     * First kill the search, and then release the reference to the dictionary
     * that we were holding.
     */

    searchPtr = objPtr->internalRep.twoPtrValue.ptr1;
    Tcl_DictObjDone(searchPtr);
    ckfree(searchPtr);

    dictPtr = objPtr->internalRep.twoPtrValue.ptr2;
    TclDecrRefCount(dictPtr);

    objPtr->typePtr = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * InitByteCodeExecution --
 *
 *	This procedure is called once to initialize the Tcl bytecode
 *	interpreter.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	This procedure initializes the array of instruction names. If
 *	compiling with the TCL_COMPILE_STATS flag, it initializes the array
 *	that counts the executions of each instruction and it creates the
 *	"evalstats" command. It also establishes the link between the Tcl
 *	"tcl_traceExec" and C "tclTraceExec" variables.
 *
 *----------------------------------------------------------------------
 */

static void
InitByteCodeExecution(
    Tcl_Interp *interp)		/* Interpreter for which the Tcl variable
				 * "tcl_traceExec" is linked to control
				 * instruction tracing. */
{
#ifdef TCL_COMPILE_DEBUG
    if (Tcl_LinkVar(interp, "tcl_traceExec", (char *) &tclTraceExec,
	    TCL_LINK_INT) != TCL_OK) {
	Tcl_Panic("InitByteCodeExecution: can't create link for tcl_traceExec variable");
    }
#endif
#ifdef TCL_COMPILE_STATS
    Tcl_CreateObjCommand(interp, "evalstats", EvalStatsCmd, NULL, NULL);
#endif /* TCL_COMPILE_STATS */
}

/*
 *----------------------------------------------------------------------
 *
 * TclCreateExecEnv --
 *
 *	This procedure creates a new execution environment for Tcl bytecode
 *	execution. An ExecEnv points to a Tcl evaluation stack. An ExecEnv is
 *	typically created once for each Tcl interpreter (Interp structure) and
 *	recursively passed to TclNRExecuteByteCode to execute ByteCode sequences
 *	for nested commands.
 *
 * Results:
 *	A newly allocated ExecEnv is returned. This points to an empty
 *	evaluation stack of the standard initial size.
 *
 * Side effects:
 *	The bytecode interpreter is also initialized here, as this procedure
 *	will be called before any call to TclNRExecuteByteCode.
 *
 *----------------------------------------------------------------------
 */

ExecEnv *
TclCreateExecEnv(
    Tcl_Interp *interp,		/* Interpreter for which the execution
				 * environment is being created. */
    int size)			/* The initial stack size, in number of words
				 * [sizeof(Tcl_Obj*)] */
{
    ExecEnv *eePtr = ckalloc(sizeof(ExecEnv));
    ExecStack *esPtr = ckalloc(sizeof(ExecStack)
	    + (size_t) (size-1) * sizeof(Tcl_Obj *));

    eePtr->execStackPtr = esPtr;
    TclNewBooleanObj(eePtr->constants[0], 0);
    Tcl_IncrRefCount(eePtr->constants[0]);
    TclNewBooleanObj(eePtr->constants[1], 1);
    Tcl_IncrRefCount(eePtr->constants[1]);
    eePtr->interp = interp;
    eePtr->callbackPtr = NULL;
    eePtr->corPtr = NULL;
    eePtr->rewind = 0;

    esPtr->prevPtr = NULL;
    esPtr->nextPtr = NULL;
    esPtr->markerPtr = NULL;
    esPtr->endPtr = &esPtr->stackWords[size-1];
    esPtr->tosPtr = &esPtr->stackWords[-1];

    Tcl_MutexLock(&execMutex);
    if (!execInitialized) {
	TclInitAuxDataTypeTable();
	InitByteCodeExecution(interp);
	execInitialized = 1;
    }
    Tcl_MutexUnlock(&execMutex);

    return eePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclDeleteExecEnv --
 *
 *	Frees the storage for an ExecEnv.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Storage for an ExecEnv and its contained storage (e.g. the evaluation
 *	stack) is freed.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteExecStack(
    ExecStack *esPtr)
{
    if (esPtr->markerPtr && !cachedInExit) {
	Tcl_Panic("freeing an execStack which is still in use");
    }

    if (esPtr->prevPtr) {
	esPtr->prevPtr->nextPtr = esPtr->nextPtr;
    }
    if (esPtr->nextPtr) {
	esPtr->nextPtr->prevPtr = esPtr->prevPtr;
    }
    ckfree(esPtr);
}

void
TclDeleteExecEnv(
    ExecEnv *eePtr)		/* Execution environment to free. */
{
    ExecStack *esPtr = eePtr->execStackPtr, *tmpPtr;

	cachedInExit = TclInExit();

    /*
     * Delete all stacks in this exec env.
     */

    while (esPtr->nextPtr) {
	esPtr = esPtr->nextPtr;
    }
    while (esPtr) {
	tmpPtr = esPtr;
	esPtr = tmpPtr->prevPtr;
	DeleteExecStack(tmpPtr);
    }

    TclDecrRefCount(eePtr->constants[0]);
    TclDecrRefCount(eePtr->constants[1]);
    if (eePtr->callbackPtr && !cachedInExit) {
	Tcl_Panic("Deleting execEnv with pending TEOV callbacks!");
    }
    if (eePtr->corPtr && !cachedInExit) {
	Tcl_Panic("Deleting execEnv with existing coroutine");
    }
    ckfree(eePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TclFinalizeExecution --
 *
 *	Finalizes the execution environment setup so that it can be later
 *	reinitialized.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	After this call, the next time TclCreateExecEnv will be called it will
 *	call InitByteCodeExecution.
 *
 *----------------------------------------------------------------------
 */

void
TclFinalizeExecution(void)
{
    Tcl_MutexLock(&execMutex);
    execInitialized = 0;
    Tcl_MutexUnlock(&execMutex);
    TclFinalizeAuxDataTypeTable();
}

/*
 * Auxiliary code to insure that GrowEvaluationStack always returns correctly
 * aligned memory.
 *
 * WALLOCALIGN represents the alignment reqs in words, just as TCL_ALLOCALIGN
 * represents the reqs in bytes. This assumes that TCL_ALLOCALIGN is a
 * multiple of the wordsize 'sizeof(Tcl_Obj *)'.
 */

#define WALLOCALIGN \
    (TCL_ALLOCALIGN/sizeof(Tcl_Obj *))

/*
 * OFFSET computes how many words have to be skipped until the next aligned
 * word. Note that we are only interested in the low order bits of ptr, so
 * that any possible information loss in PTR2INT is of no consequence.
 */

static inline int
OFFSET(
    void *ptr)
{
    int mask = TCL_ALLOCALIGN-1;
    int base = PTR2INT(ptr) & mask;
    return (TCL_ALLOCALIGN - base)/sizeof(Tcl_Obj *);
}

/*
 * Given a marker, compute where the following aligned memory starts.
 */

#define MEMSTART(markerPtr) \
    ((markerPtr) + OFFSET(markerPtr))

/*
 *----------------------------------------------------------------------
 *
 * GrowEvaluationStack --
 *
 *	This procedure grows a Tcl evaluation stack stored in an ExecEnv,
 *	copying over the words since the last mark if so requested. A mark is
 *	set at the beginning of the new area when no copying is requested.
 *
 * Results:
 *	Returns a pointer to the first usable word in the (possibly) grown
 *	stack.
 *
 * Side effects:
 *	The size of the evaluation stack may be grown, a marker is set
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj **
GrowEvaluationStack(
    ExecEnv *eePtr,		/* Points to the ExecEnv with an evaluation
				 * stack to enlarge. */
    int growth,			/* How much larger than the current used
				 * size. */
    int move)			/* 1 if move words since last marker. */
{
    ExecStack *esPtr = eePtr->execStackPtr, *oldPtr = NULL;
    int newBytes, newElems, currElems;
    int needed = growth - (esPtr->endPtr - esPtr->tosPtr);
    Tcl_Obj **markerPtr = esPtr->markerPtr, **memStart;
    int moveWords = 0;

    if (move) {
	if (!markerPtr) {
	    Tcl_Panic("STACK: Reallocating with no previous alloc");
	}
	if (needed <= 0) {
	    return MEMSTART(markerPtr);
	}
    } else {
	Tcl_Obj **tmpMarkerPtr = esPtr->tosPtr + 1;
	int offset = OFFSET(tmpMarkerPtr);

	if (needed + offset < 0) {
	    /*
	     * Put a marker pointing to the previous marker in this stack, and
	     * store it in esPtr as the current marker. Return a pointer to
	     * the start of aligned memory.
	     */

	    esPtr->markerPtr = tmpMarkerPtr;
	    memStart = tmpMarkerPtr + offset;
	    esPtr->tosPtr = memStart - 1;
	    *esPtr->markerPtr = (Tcl_Obj *) markerPtr;
	    return memStart;
	}
    }

    /*
     * Reset move to hold the number of words to be moved to new stack (if
     * any) and growth to hold the complete stack requirements: add one for
     * the marker, (WALLOCALIGN-1) for the maximal possible offset.
     */

    if (move) {
	moveWords = esPtr->tosPtr - MEMSTART(markerPtr) + 1;
    }
    needed = growth + moveWords + WALLOCALIGN;

    /*
     * Check if there is enough room in the next stack (if there is one, it
     * should be both empty and the last one!)
     */

    if (esPtr->nextPtr) {
	oldPtr = esPtr;
	esPtr = oldPtr->nextPtr;
	currElems = esPtr->endPtr - &esPtr->stackWords[-1];
	if (esPtr->markerPtr || (esPtr->tosPtr != &esPtr->stackWords[-1])) {
	    Tcl_Panic("STACK: Stack after current is in use");
	}
	if (esPtr->nextPtr) {
	    Tcl_Panic("STACK: Stack after current is not last");
	}
	if (needed <= currElems) {
	    goto newStackReady;
	}
	DeleteExecStack(esPtr);
	esPtr = oldPtr;
    } else {
	currElems = esPtr->endPtr - &esPtr->stackWords[-1];
    }

    /*
     * We need to allocate a new stack! It needs to store 'growth' words,
     * including the elements to be copied over and the new marker.
     */

    newElems = 2*currElems;
    while (needed > newElems) {
	newElems *= 2;
    }
    newBytes = sizeof(ExecStack) + (newElems-1) * sizeof(Tcl_Obj *);

    oldPtr = esPtr;
    esPtr = ckalloc(newBytes);

    oldPtr->nextPtr = esPtr;
    esPtr->prevPtr = oldPtr;
    esPtr->nextPtr = NULL;
    esPtr->endPtr = &esPtr->stackWords[newElems-1];

  newStackReady:
    eePtr->execStackPtr = esPtr;

    /*
     * Store a NULL marker at the beginning of the stack, to indicate that
     * this is the first marker in this stack and that rewinding to here
     * should actually be a return to the previous stack.
     */

    esPtr->stackWords[0] = NULL;
    esPtr->markerPtr = &esPtr->stackWords[0];
    memStart = MEMSTART(esPtr->markerPtr);
    esPtr->tosPtr = memStart - 1;

    if (move) {
	memcpy(memStart, MEMSTART(markerPtr), moveWords*sizeof(Tcl_Obj *));
	esPtr->tosPtr += moveWords;
	oldPtr->markerPtr = (Tcl_Obj **) *markerPtr;
	oldPtr->tosPtr = markerPtr-1;
    }

    /*
     * Free the old stack if it is now unused.
     */

    if (!oldPtr->markerPtr) {
	DeleteExecStack(oldPtr);
    }

    return memStart;
}

/*
 *--------------------------------------------------------------
 *
 * TclStackAlloc, TclStackRealloc, TclStackFree --
 *
 *	Allocate memory from the execution stack; it has to be returned later
 *	with a call to TclStackFree.
 *
 * Results:
 *	A pointer to the first byte allocated, or panics if the allocation did
 *	not succeed.
 *
 * Side effects:
 *	The execution stack may be grown.
 *
 *--------------------------------------------------------------
 */

static Tcl_Obj **
StackAllocWords(
    Tcl_Interp *interp,
    int numWords)
{
    /*
     * Note that GrowEvaluationStack sets a marker in the stack. This marker
     * is read when rewinding, e.g., by TclStackFree.
     */

    Interp *iPtr = (Interp *) interp;
    ExecEnv *eePtr = iPtr->execEnvPtr;
    Tcl_Obj **resPtr = GrowEvaluationStack(eePtr, numWords, 0);

    eePtr->execStackPtr->tosPtr += numWords;
    return resPtr;
}

static Tcl_Obj **
StackReallocWords(
    Tcl_Interp *interp,
    int numWords)
{
    Interp *iPtr = (Interp *) interp;
    ExecEnv *eePtr = iPtr->execEnvPtr;
    Tcl_Obj **resPtr = GrowEvaluationStack(eePtr, numWords, 1);

    eePtr->execStackPtr->tosPtr += numWords;
    return resPtr;
}

void
TclStackFree(
    Tcl_Interp *interp,
    void *freePtr)
{
    Interp *iPtr = (Interp *) interp;
    ExecEnv *eePtr;
    ExecStack *esPtr;
    Tcl_Obj **markerPtr, *marker;

    if (iPtr == NULL || iPtr->execEnvPtr == NULL) {
	Tcl_Free((char *) freePtr);
	return;
    }

    /*
     * Rewind the stack to the previous marker position. The current marker,
     * as set in the last call to GrowEvaluationStack, contains a pointer to
     * the previous marker.
     */

    eePtr = iPtr->execEnvPtr;
    esPtr = eePtr->execStackPtr;
    markerPtr = esPtr->markerPtr;
    marker = *markerPtr;

    if ((freePtr != NULL) && (MEMSTART(markerPtr) != (Tcl_Obj **)freePtr)) {
	Tcl_Panic("TclStackFree: incorrect freePtr (%p != %p). Call out of sequence?",
		freePtr, MEMSTART(markerPtr));
    }

    esPtr->tosPtr = markerPtr - 1;
    esPtr->markerPtr = (Tcl_Obj **) marker;
    if (marker) {
	return;
    }

    /*
     * Return to previous active stack. Note that repeated expansions or
     * reallocs could have generated several unused intervening stacks: free
     * them too.
     */

    while (esPtr->nextPtr) {
	esPtr = esPtr->nextPtr;
    }
    esPtr->tosPtr = &esPtr->stackWords[-1];
    while (esPtr->prevPtr) {
	ExecStack *tmpPtr = esPtr->prevPtr;
	if (tmpPtr->tosPtr == &tmpPtr->stackWords[-1]) {
	    DeleteExecStack(tmpPtr);
	} else {
	    break;
	}
    }
    if (esPtr->prevPtr) {
	eePtr->execStackPtr = esPtr->prevPtr;
    } else {
	eePtr->execStackPtr = esPtr;
    }
}

void *
TclStackAlloc(
    Tcl_Interp *interp,
    int numBytes)
{
    Interp *iPtr = (Interp *) interp;
    int numWords = (numBytes + (sizeof(Tcl_Obj *) - 1))/sizeof(Tcl_Obj *);

    if (iPtr == NULL || iPtr->execEnvPtr == NULL) {
	return (void *) Tcl_Alloc(numBytes);
    }

    return (void *) StackAllocWords(interp, numWords);
}

void *
TclStackRealloc(
    Tcl_Interp *interp,
    void *ptr,
    size_t numBytes)
{
    Interp *iPtr = (Interp *) interp;
    ExecEnv *eePtr;
    ExecStack *esPtr;
    Tcl_Obj **markerPtr;
    int numWords;

    if (iPtr == NULL || iPtr->execEnvPtr == NULL) {
	return (void *) Tcl_Realloc((char *) ptr, numBytes);
    }

    eePtr = iPtr->execEnvPtr;
    esPtr = eePtr->execStackPtr;
    markerPtr = esPtr->markerPtr;

    if (MEMSTART(markerPtr) != (Tcl_Obj **)ptr) {
	Tcl_Panic("TclStackRealloc: incorrect ptr. Call out of sequence?");
    }

    numWords = (numBytes + (sizeof(Tcl_Obj *) - 1))/sizeof(Tcl_Obj *);
    return (void *) StackReallocWords(interp, numWords);
}

/*
 *--------------------------------------------------------------
 *
 * Tcl_ExprObj --
 *
 *	Evaluate an expression in a Tcl_Obj.
 *
 * Results:
 *	A standard Tcl object result. If the result is other than TCL_OK, then
 *	the interpreter's result contains an error message. If the result is
 *	TCL_OK, then a pointer to the expression's result value object is
 *	stored in resultPtrPtr. In that case, the object's ref count is
 *	incremented to reflect the reference returned to the caller; the
 *	caller is then responsible for the resulting object and must, for
 *	example, decrement the ref count when it is finished with the object.
 *
 * Side effects:
 *	Any side effects caused by subcommands in the expression, if any. The
 *	interpreter result is not modified unless there is an error.
 *
 *--------------------------------------------------------------
 */

int
Tcl_ExprObj(
    Tcl_Interp *interp,		/* Context in which to evaluate the
				 * expression. */
    register Tcl_Obj *objPtr,	/* Points to Tcl object containing expression
				 * to evaluate. */
    Tcl_Obj **resultPtrPtr)	/* Where the Tcl_Obj* that is the expression
				 * result is stored if no errors occur. */
{
    NRE_callback *rootPtr = TOP_CB(interp);
    Tcl_Obj *resultPtr;

    TclNewObj(resultPtr);
    TclNRAddCallback(interp, CopyCallback, resultPtrPtr, resultPtr,
	    NULL, NULL);
    Tcl_NRExprObj(interp, objPtr, resultPtr);
    return TclNRRunCallbacks(interp, TCL_OK, rootPtr);
}

static int
CopyCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    Tcl_Obj **resultPtrPtr = data[0];
    Tcl_Obj *resultPtr = data[1];

    if (result == TCL_OK) {
	*resultPtrPtr = resultPtr;
	Tcl_IncrRefCount(resultPtr);
    } else {
	Tcl_DecrRefCount(resultPtr);
    }
    return result;
}

/*
 *--------------------------------------------------------------
 *
 * Tcl_NRExprObj --
 *
 *	Request evaluation of the expression in a Tcl_Obj by the NR stack.
 *
 * Results:
 *	Returns TCL_OK.
 *
 * Side effects:
 *	Compiles objPtr as a Tcl expression and places callbacks on the
 *	NR stack to execute the bytecode and store the result in resultPtr.
 *	If bytecode execution raises an exception, nothing is written
 *	to resultPtr, and the exceptional return code flows up the NR
 *	stack.  If the exception is TCL_ERROR, an error message is left
 *	in the interp result and the interp's return options dictionary
 *	holds additional error information too.  Execution of the bytecode
 *	may have other side effects, depending on the expression.
 *
 *--------------------------------------------------------------
 */

int
Tcl_NRExprObj(
    Tcl_Interp *interp,
    Tcl_Obj *objPtr,
    Tcl_Obj *resultPtr)
{
    ByteCode *codePtr;

    /* TODO: consider saving whole state? */
    Tcl_Obj *saveObjPtr = Tcl_GetObjResult(interp);

    Tcl_IncrRefCount(saveObjPtr);

    codePtr = CompileExprObj(interp, objPtr);

    /* TODO: Confirm reset not required? */
    /*Tcl_ResetResult(interp);*/
    Tcl_NRAddCallback(interp, ExprObjCallback, saveObjPtr, resultPtr,
	    NULL, NULL);
    return TclNRExecuteByteCode(interp, codePtr);
}

static int
ExprObjCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    Tcl_Obj *saveObjPtr = data[0];
    Tcl_Obj *resultPtr = data[1];

    if (result == TCL_OK) {
	TclSetDuplicateObj(resultPtr, Tcl_GetObjResult(interp));
	Tcl_SetObjResult(interp, saveObjPtr);
    }
    TclDecrRefCount(saveObjPtr);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CompileExprObj --
 *	Compile a Tcl expression value into ByteCode.
 *
 * Results:
 *	A (ByteCode *) is returned pointing to the resulting ByteCode.
 *	The caller must manage its refCount and arrange for a call to
 *	TclCleanupByteCode() when the last reference disappears.
 *
 * Side effects:
 *	The Tcl_ObjType of objPtr is changed to the "bytecode" type,
 *	and the ByteCode is kept in the internal rep (along with context
 *	data for checking validity) for faster operations the next time
 *	CompileExprObj is called on the same value.
 *
 *----------------------------------------------------------------------
 */

static ByteCode *
CompileExprObj(
    Tcl_Interp *interp,
    Tcl_Obj *objPtr)
{
    Interp *iPtr = (Interp *) interp;
    CompileEnv compEnv;		/* Compilation environment structure allocated
				 * in frame. */
    register ByteCode *codePtr = NULL;
				/* Tcl Internal type of bytecode. Initialized
				 * to avoid compiler warning. */

    /*
     * Get the expression ByteCode from the object. If it exists, make sure it
     * is valid in the current context.
     */
    if (objPtr->typePtr == &exprCodeType) {
	Namespace *namespacePtr = iPtr->varFramePtr->nsPtr;

	codePtr = objPtr->internalRep.otherValuePtr;
	if (((Interp *) *codePtr->interpHandle != iPtr)
		|| (codePtr->compileEpoch != iPtr->compileEpoch)
		|| (codePtr->nsPtr != namespacePtr)
		|| (codePtr->nsEpoch != namespacePtr->resolverEpoch)
		|| (codePtr->localCachePtr != iPtr->varFramePtr->localCachePtr)) {
	    FreeExprCodeInternalRep(objPtr);
	}
    }
    if (objPtr->typePtr != &exprCodeType) {
	/*
	 * TIP #280: No invoker (yet) - Expression compilation.
	 */

	size_t length;
	const char *string = TclGetStringFromObj(objPtr, &length);

	TclInitCompileEnv(interp, &compEnv, string, length, NULL, 0);
	TclCompileExpr(interp, string, length, &compEnv, 0);

	/*
	 * Successful compilation. If the expression yielded no instructions,
	 * push an zero object as the expression's result.
	 */

	if (compEnv.codeNext == compEnv.codeStart) {
	    TclEmitPush(TclRegisterNewLiteral(&compEnv, "0", 1),
		    &compEnv);
	}

	/*
	 * Add a "done" instruction as the last instruction and change the
	 * object into a ByteCode object. Ownership of the literal objects and
	 * aux data items is given to the ByteCode object.
	 */

	TclEmitOpcode(INST_DONE, &compEnv);
	TclInitByteCodeObj(objPtr, &compEnv);
	objPtr->typePtr = &exprCodeType;
	TclFreeCompileEnv(&compEnv);
	codePtr = objPtr->internalRep.otherValuePtr;
	if (iPtr->varFramePtr->localCachePtr) {
	    codePtr->localCachePtr = iPtr->varFramePtr->localCachePtr;
	    codePtr->localCachePtr->refCount++;
	}
#ifdef TCL_COMPILE_DEBUG
	if (tclTraceCompile == 2) {
	    TclPrintByteCodeObj(interp, objPtr);
	    fflush(stdout);
	}
#endif /* TCL_COMPILE_DEBUG */
    }
    return codePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * DupExprCodeInternalRep --
 *
 *	Part of the Tcl object type implementation for Tcl expression
 *	bytecode. We do not copy the bytecode intrep. Instead, we return
 *	without setting copyPtr->typePtr, so the copy is a plain string copy
 *	of the expression value, and if it is to be used as a compiled
 *	expression, it will just need a recompile.
 *
 *	This makes sense, because with Tcl's copy-on-write practices, the
 *	usual (only?) time Tcl_DuplicateObj() will be called is when the copy
 *	is about to be modified, which would invalidate any copied bytecode
 *	anyway. The only reason it might make sense to copy the bytecode is if
 *	we had some modifying routines that operated directly on the intrep,
 *	like we do for lists and dicts.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DupExprCodeInternalRep(
    Tcl_Obj *srcPtr,
    Tcl_Obj *copyPtr)
{
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeExprCodeInternalRep --
 *
 *	Part of the Tcl object type implementation for Tcl expression
 *	bytecode. Frees the storage allocated to hold the internal rep, unless
 *	ref counts indicate bytecode execution is still in progress.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May free allocated memory. Leaves objPtr untyped.
 *
 *----------------------------------------------------------------------
 */

static void
FreeExprCodeInternalRep(
    Tcl_Obj *objPtr)
{
    ByteCode *codePtr = objPtr->internalRep.otherValuePtr;

    objPtr->typePtr = NULL;
    objPtr->internalRep.otherValuePtr = NULL;
    codePtr->refCount--;
    if (codePtr->refCount <= 0) {
	TclCleanupByteCode(codePtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileObj --
 *
 *	This procedure compiles the script contained in a Tcl_Obj.
 *
 * Results:
 *	A pointer to the corresponding ByteCode, never NULL.
 *
 * Side effects:
 *	The object is shimmered to bytecode type.
 *
 *----------------------------------------------------------------------
 */

ByteCode *
TclCompileObj(
    Tcl_Interp *interp,
    Tcl_Obj *objPtr,
    const CmdFrame *invoker,
    int word)
{
    register Interp *iPtr = (Interp *) interp;
    register ByteCode *codePtr;	/* Tcl Internal type of bytecode. */
    Namespace *namespacePtr = iPtr->varFramePtr->nsPtr;

    /*
     * If the object is not already of tclByteCodeType, compile it (and reset
     * the compilation flags in the interpreter; this should be done after any
     * compilation). Otherwise, check that it is "fresh" enough.
     */

    if (objPtr->typePtr == &tclByteCodeType) {
	/*
	 * Make sure the Bytecode hasn't been invalidated by, e.g., someone
	 * redefining a command with a compile procedure (this might make the
	 * compiled code wrong). The object needs to be recompiled if it was
	 * compiled in/for a different interpreter, or for a different
	 * namespace, or for the same namespace but with different name
	 * resolution rules. Precompiled objects, however, are immutable and
	 * therefore they are not recompiled, even if the epoch has changed.
	 *
	 * To be pedantically correct, we should also check that the
	 * originating procPtr is the same as the current context procPtr
	 * (assuming one exists at all - none for global level). This code is
	 * #def'ed out because [info body] was changed to never return a
	 * bytecode type object, which should obviate us from the extra checks
	 * here.
	 */

	codePtr = objPtr->internalRep.otherValuePtr;
	if (((Interp *) *codePtr->interpHandle != iPtr)
		|| (codePtr->compileEpoch != iPtr->compileEpoch)
		|| (codePtr->nsPtr != namespacePtr)
		|| (codePtr->nsEpoch != namespacePtr->resolverEpoch)) {
	    if (!(codePtr->flags & TCL_BYTECODE_PRECOMPILED)) {
		goto recompileObj;
	    }
	    if ((Interp *) *codePtr->interpHandle != iPtr) {
		Tcl_Panic("Tcl_EvalObj: compiled script jumped interps");
	    }
	    codePtr->compileEpoch = iPtr->compileEpoch;
	}

	/*
	 * Check that any compiled locals do refer to the current proc
	 * environment! If not, recompile.
	 */

	if (!(codePtr->flags & TCL_BYTECODE_PRECOMPILED) &&
		(codePtr->procPtr == NULL) &&
		(codePtr->localCachePtr != iPtr->varFramePtr->localCachePtr)){
	    goto recompileObj;
	}

	/*
	 * #280.
	 * Literal sharing fix. This part of the fix is not required by 8.4
	 * nor 8.5, because they eval-direct any literals, so just saving the
	 * argument locations per command in bytecode is enough, embedded
	 * 'eval' commands, etc. get the correct information.
	 *
	 * But in 8.6 all the embedded script are compiled, and the resulting
	 * bytecode stored in the literal. Now the shared literal has bytecode
	 * with location data for _one_ particular location this literal is
	 * found at. If we get executed from a different location the bytecode
	 * has to be recompiled to get the correct locations. Not doing this
	 * will execute the saved bytecode with data for a different location,
	 * causing 'info frame' to point to the wrong place in the sources.
	 *
	 * Future optimizations ...
	 * (1) Save the location data (ExtCmdLoc) keyed by start line. In that
	 *     case we recompile once per location of the literal, but not
	 *     continously, because the moment we have all locations we do not
	 *     need to recompile any longer.
	 *
	 * (2) Alternative: Do not recompile, tell the execution engine the
	 *     offset between saved starting line and actual one. Then modify
	 *     the users to adjust the locations they have by this offset.
	 *
	 * (3) Alternative 2: Do not fully recompile, adjust just the location
	 *     information.
	 */

	if (invoker == NULL) {
	    return codePtr;
	} else {
	    Tcl_HashEntry *hePtr =
		    Tcl_FindHashEntry(iPtr->lineBCPtr, codePtr);
	    ExtCmdLoc *eclPtr;
	    CmdFrame *ctxCopyPtr;
	    int redo;

	    if (!hePtr) {
		return codePtr;
	    }

	    eclPtr = Tcl_GetHashValue(hePtr);
	    redo = 0;
	    ctxCopyPtr = TclStackAlloc(interp, sizeof(CmdFrame));
	    *ctxCopyPtr = *invoker;

	    if (invoker->type == TCL_LOCATION_BC) {
		/*
		 * Note: Type BC => ctx.data.eval.path    is not used.
		 *		    ctx.data.tebc.codePtr used instead
		 */

		TclGetSrcInfoForPc(ctxCopyPtr);
		if (ctxCopyPtr->type == TCL_LOCATION_SOURCE) {
		    /*
		     * The reference made by 'TclGetSrcInfoForPc' is dead.
		     */

		    Tcl_DecrRefCount(ctxCopyPtr->data.eval.path);
		    ctxCopyPtr->data.eval.path = NULL;
		}
	    }

	    if (word < ctxCopyPtr->nline) {
		/*
		 * Note: We do not care if the line[word] is -1. This is a
		 * difference and requires a recompile (location changed from
		 * absolute to relative, literal is used fixed and through
		 * variable)
		 *
		 * Example:
		 * test info-32.0 using literal of info-24.8
		 *     (dict with ... vs           set body ...).
		 */

		redo = ((eclPtr->type == TCL_LOCATION_SOURCE)
			    && (eclPtr->start != ctxCopyPtr->line[word]))
			|| ((eclPtr->type == TCL_LOCATION_BC)
			    && (ctxCopyPtr->type == TCL_LOCATION_SOURCE));
	    }

	    TclStackFree(interp, ctxCopyPtr);
	    if (!redo) {
		return codePtr;
	    }
	}
    }

  recompileObj:
    iPtr->errorLine = 1;

    /*
     * TIP #280. Remember the invoker for a moment in the interpreter
     * structures so that the byte code compiler can pick it up when
     * initializing the compilation environment, i.e. the extended location
     * information.
     */

    iPtr->invokeCmdFramePtr = invoker;
    iPtr->invokeWord = word;
    TclSetByteCodeFromAny(interp, objPtr, NULL, NULL);
    iPtr->invokeCmdFramePtr = NULL;
    codePtr = objPtr->internalRep.otherValuePtr;
    if (iPtr->varFramePtr->localCachePtr) {
	codePtr->localCachePtr = iPtr->varFramePtr->localCachePtr;
	codePtr->localCachePtr->refCount++;
    }
    return codePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclIncrObj --
 *
 *	Increment an integeral value in a Tcl_Obj by an integeral value held
 *	in another Tcl_Obj. Caller is responsible for making sure we can
 *	update the first object.
 *
 * Results:
 *	TCL_ERROR if either object is non-integer, and TCL_OK otherwise. On
 *	error, an error message is left in the interpreter (if it is not NULL,
 *	of course).
 *
 * Side effects:
 *	valuePtr gets the new incrmented value.
 *
 *----------------------------------------------------------------------
 */

int
TclIncrObj(
    Tcl_Interp *interp,
    Tcl_Obj *valuePtr,
    Tcl_Obj *incrPtr)
{
    ClientData ptr1, ptr2;
    int type1, type2;
    mp_int value, incr;

    if (Tcl_IsShared(valuePtr)) {
	Tcl_Panic("%s called with shared object", "TclIncrObj");
    }

    if (GetNumberFromObj(NULL, valuePtr, &ptr1, &type1) != TCL_OK) {
	/*
	 * Produce error message (reparse?!)
	 */

	return TclGetIntFromObj(interp, valuePtr, &type1);
    }
    if (GetNumberFromObj(NULL, incrPtr, &ptr2, &type2) != TCL_OK) {
	/*
	 * Produce error message (reparse?!)
	 */

	TclGetIntFromObj(interp, incrPtr, &type1);
	Tcl_AddErrorInfo(interp, "\n    (reading increment)");
	return TCL_ERROR;
    }

    if ((type1 == TCL_NUMBER_LONG) && (type2 == TCL_NUMBER_LONG)) {
	long augend = *((const long *) ptr1);
	long addend = *((const long *) ptr2);
	long sum = augend + addend;

	/*
	 * Overflow when (augend and sum have different sign) and (augend and
	 * addend have the same sign). This is encapsulated in the Overflowing
	 * macro.
	 */

	if (!Overflowing(augend, addend, sum)) {
	    TclSetLongObj(valuePtr, sum);
	    return TCL_OK;
	}
#ifndef NO_WIDE_TYPE
	{
	    Tcl_WideInt w1 = (Tcl_WideInt) augend;
	    Tcl_WideInt w2 = (Tcl_WideInt) addend;

	    /*
	     * We know the sum value is outside the long range, so we use the
	     * macro form that doesn't range test again.
	     */

	    TclSetWideIntObj(valuePtr, w1 + w2);
	    return TCL_OK;
	}
#endif
    }

    if ((type1 == TCL_NUMBER_DOUBLE) || (type1 == TCL_NUMBER_NAN)) {
	/*
	 * Produce error message (reparse?!)
	 */

	return TclGetIntFromObj(interp, valuePtr, &type1);
    }
    if ((type2 == TCL_NUMBER_DOUBLE) || (type2 == TCL_NUMBER_NAN)) {
	/*
	 * Produce error message (reparse?!)
	 */

	TclGetIntFromObj(interp, incrPtr, &type1);
	Tcl_AddErrorInfo(interp, "\n    (reading increment)");
	return TCL_ERROR;
    }

#ifndef NO_WIDE_TYPE
    if ((type1 != TCL_NUMBER_BIG) && (type2 != TCL_NUMBER_BIG)) {
	Tcl_WideInt w1, w2, sum;

	TclGetWideIntFromObj(NULL, valuePtr, &w1);
	TclGetWideIntFromObj(NULL, incrPtr, &w2);
	sum = w1 + w2;

	/*
	 * Check for overflow.
	 */

	if (!Overflowing(w1, w2, sum)) {
	    Tcl_SetWideIntObj(valuePtr, sum);
	    return TCL_OK;
	}
    }
#endif

    Tcl_TakeBignumFromObj(interp, valuePtr, &value);
    Tcl_GetBignumFromObj(interp, incrPtr, &incr);
    mp_add(&value, &incr, &value);
    mp_clear(&incr);
    Tcl_SetBignumObj(valuePtr, &value);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclNRExecuteByteCode --
 *
 *	This procedure executes the instructions of a ByteCode structure. It
 *	returns when a "done" instruction is executed or an error occurs.
 *
 * Results:
 *	The return value is one of the return codes defined in tcl.h (such as
 *	TCL_OK), and interp->objResultPtr refers to a Tcl object that either
 *	contains the result of executing the code or an error message.
 *
 * Side effects:
 *	Almost certainly, depending on the ByteCode's instructions.
 *
 *----------------------------------------------------------------------
 */
#define	bcFramePtr	(&TD->cmdFrame)
#define	initCatchTop	((ptrdiff_t *) (&TD->stack[-1]))
#define	initTosPtr	((Tcl_Obj **) (initCatchTop+codePtr->maxExceptDepth))
#define esPtr		(iPtr->execEnvPtr->execStackPtr)

int
TclNRExecuteByteCode(
    Tcl_Interp *interp,		/* Token for command interpreter. */
    ByteCode *codePtr)		/* The bytecode sequence to interpret. */
{
    Interp *iPtr = (Interp *) interp;
    TEBCdata *TD;
    int size = sizeof(TEBCdata) - 1
	    + (codePtr->maxStackDepth + codePtr->maxExceptDepth)
		* sizeof(void *);
    int numWords = (size + sizeof(Tcl_Obj *) - 1) / sizeof(Tcl_Obj *);

    if (iPtr->execEnvPtr->rewind) {
	return TCL_ERROR;
    }

    codePtr->refCount++;

    /*
     * Reserve the stack, setup the TEBCdataPtr (TD) and CallFrame
     *
     * The execution uses a unified stack: first a TEBCdata, immediately
     * above it a CmdFrame, then the catch stack, then the execution stack.
     *
     * Make sure the catch stack is large enough to hold the maximum number of
     * catch commands that could ever be executing at the same time (this will
     * be no more than the exception range array's depth). Make sure the
     * execution stack is large enough to execute this ByteCode.
     */

    TD = (TEBCdata *) GrowEvaluationStack(iPtr->execEnvPtr, numWords, 0);
    esPtr->tosPtr = initTosPtr;

    TD->codePtr     = codePtr;
    TD->pc	    = codePtr->codeStart;
    TD->catchTop    = initCatchTop;
    TD->cleanup     = 0;
    TD->auxObjList  = NULL;
    TD->checkInterp = 0;

    /*
     * TIP #280: Initialize the frame. Do not push it yet: it will be pushed
     * every time that we call out from this TD, popped when we return to it.
     */

    bcFramePtr->type = ((codePtr->flags & TCL_BYTECODE_PRECOMPILED)
	    ? TCL_LOCATION_PREBC : TCL_LOCATION_BC);
    bcFramePtr->level = (iPtr->cmdFramePtr ? iPtr->cmdFramePtr->level+1 : 1);
    bcFramePtr->numLevels = iPtr->numLevels;
    bcFramePtr->framePtr = iPtr->framePtr;
    bcFramePtr->nextPtr = iPtr->cmdFramePtr;
    bcFramePtr->nline = 0;
    bcFramePtr->line = NULL;
    bcFramePtr->litarg = NULL;
    bcFramePtr->data.tebc.codePtr = codePtr;
    bcFramePtr->data.tebc.pc = NULL;
    bcFramePtr->cmd.str.cmd = NULL;
    bcFramePtr->cmd.str.len = 0;

#ifdef TCL_COMPILE_STATS
    iPtr->stats.numExecutions++;
#endif

    /*
     * Push the callback for bytecode execution
     */

    TclNRAddCallback(interp, TEBCresume, TD, /*resume*/ INT2PTR(0),
	    NULL, NULL);
    return TCL_OK;
}

static int
TEBCresume(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    /*
     * Compiler cast directive - not a real variable.
     *	   Interp *iPtr = (Interp *) interp;
     */
#define iPtr ((Interp *) interp)

    /*
     * Check just the read-traced/write-traced bit of a variable.
     */

#define ReadTraced(varPtr) ((varPtr)->flags & VAR_TRACED_READ)
#define WriteTraced(varPtr) ((varPtr)->flags & VAR_TRACED_WRITE)
#define UnsetTraced(varPtr) ((varPtr)->flags & VAR_TRACED_UNSET)

    /*
     * Bottom of allocated stack holds the NR data
     */

    /*
     * Constants: variables that do not change during the execution, used
     * sporadically: no special need for speed.
     */

    int instructionCount = 0;	/* Counter that is used to work out when to
				 * call Tcl_AsyncReady() */
    const char *curInstName;
#ifdef TCL_COMPILE_DEBUG
    int traceInstructions;	/* Whether we are doing instruction-level
				 * tracing or not. */
#endif

    Var *compiledLocals = iPtr->varFramePtr->compiledLocals;
    Tcl_Obj **constants = &iPtr->execEnvPtr->constants[0];

#define LOCAL(i)	(&compiledLocals[(i)])
#define TCONST(i)	(constants[(i)])

    /*
     * These macros are just meant to save some global variables that are not
     * used too frequently
     */

    TEBCdata *TD = data[0];
#define auxObjList	(TD->auxObjList)
#define catchTop	(TD->catchTop)
#define codePtr		(TD->codePtr)
#define checkInterp	(TD->checkInterp)
			/* Indicates when a check of interp readyness is
			 * necessary. Set by CACHE_STACK_INFO() */

    /*
     * Globals: variables that store state, must remain valid at all times.
     */

    Tcl_Obj **tosPtr;		/* Cached pointer to top of evaluation
				 * stack. */
    const unsigned char *pc;	/* The current program counter. */

    /*
     * Transfer variables - needed only between opcodes, but not while
     * executing an instruction.
     */

    int cleanup = 0;
    Tcl_Obj *objResultPtr;

    /*
     * Locals - variables that are used within opcodes or bounded sections of
     * the file (jumps between opcodes within a family).
     * NOTE: These are now mostly defined locally where needed.
     */

    Tcl_Obj *objPtr, *valuePtr, *value2Ptr, *part1Ptr, *part2Ptr, *tmpPtr;
    Tcl_Obj **objv;
    size_t objc = 0;
    int opnd, pcAdjustment;
    size_t length;
    Var *varPtr, *arrayPtr;
#ifdef TCL_COMPILE_DEBUG
    char cmdNameBuf[21];
#endif

#ifdef TCL_COMPILE_DEBUG
    traceInstructions = (tclTraceExec == 3);
#endif

    TEBC_DATA_DIG();

#ifdef TCL_COMPILE_DEBUG
    if (!data[1] && (tclTraceExec >= 2)) {
	PrintByteCodeInfo(codePtr);
	fprintf(stdout, "  Starting stack top=%d\n", (int) CURR_DEPTH);
	fflush(stdout);
    }
#endif

    if (data[1] /* resume from invocation */) {
	if (iPtr->execEnvPtr->rewind) {
	    result = TCL_ERROR;
	}
	NRE_ASSERT(iPtr->cmdFramePtr == bcFramePtr);
	iPtr->cmdFramePtr = bcFramePtr->nextPtr;
	if (iPtr->flags & INTERP_DEBUG_FRAME) {
	    TclArgumentBCRelease((Tcl_Interp *) iPtr, bcFramePtr);
	}
	if (codePtr->flags & TCL_BYTECODE_RECOMPILE) {
	    iPtr->flags |= ERR_ALREADY_LOGGED;
	    codePtr->flags &= ~TCL_BYTECODE_RECOMPILE;
	}

	CACHE_STACK_INFO();
	if (result == TCL_OK) {
#ifndef TCL_COMPILE_DEBUG
	    if (*pc == INST_POP) {
		NEXT_INST_V(1, cleanup, 0);
	    }
#endif
	    /*
	     * Push the call's object result and continue execution with the
	     * next instruction.
	     */

	    TRACE_WITH_OBJ(("%u => ... after \"%.20s\": TCL_OK, result=",
		    objc, cmdNameBuf), Tcl_GetObjResult(interp));

	    objResultPtr = Tcl_GetObjResult(interp);

	    /*
	     * Reset the interp's result to avoid possible duplications of
	     * large objects [Bug 781585]. We do not call Tcl_ResetResult to
	     * avoid any side effects caused by the resetting of errorInfo and
	     * errorCode [Bug 804681], which are not needed here. We chose
	     * instead to manipulate the interp's object result directly.
	     *
	     * Note that the result object is now in objResultPtr, it keeps
	     * the refCount it had in its role of iPtr->objResultPtr.
	     */

	    TclNewObj(objPtr);
	    Tcl_IncrRefCount(objPtr);
	    iPtr->objResultPtr = objPtr;
	    NEXT_INST_V(0, cleanup, -1);
	}

	/*
	 * Result not TCL_OK: fall through
	 */
    }

    if (iPtr->execEnvPtr->rewind) {
	result = TCL_ERROR;
	goto abnormalReturn;
    }

    if (result != TCL_OK) {
	pc--;
	goto processExceptionReturn;
    }

    /*
     * Loop executing instructions until a "done" instruction, a TCL_RETURN,
     * or some error.
     */

    goto cleanup0;

    /*
     * Targets for standard instruction endings; unrolled for speed in the
     * most frequent cases (instructions that consume up to two stack
     * elements).
     *
     * This used to be a "for(;;)" loop, with each instruction doing its own
     * cleanup.
     */

  cleanupV_pushObjResultPtr:
    switch (cleanup) {
    case 0:
	*(++tosPtr) = (objResultPtr);
	goto cleanup0;
    default:
	cleanup -= 2;
	while (cleanup--) {
	    objPtr = POP_OBJECT();
	    TclDecrRefCount(objPtr);
	}
    case 2:
    cleanup2_pushObjResultPtr:
	objPtr = POP_OBJECT();
	TclDecrRefCount(objPtr);
    case 1:
    cleanup1_pushObjResultPtr:
	objPtr = OBJ_AT_TOS;
	TclDecrRefCount(objPtr);
    }
    OBJ_AT_TOS = objResultPtr;
    goto cleanup0;

  cleanupV:
    switch (cleanup) {
    default:
	cleanup -= 2;
	while (cleanup--) {
	    objPtr = POP_OBJECT();
	    TclDecrRefCount(objPtr);
	}
    case 2:
    cleanup2:
	objPtr = POP_OBJECT();
	TclDecrRefCount(objPtr);
    case 1:
    cleanup1:
	objPtr = POP_OBJECT();
	TclDecrRefCount(objPtr);
    case 0:
	/*
	 * We really want to do nothing now, but this is needed for some
	 * compilers (SunPro CC).
	 */

	break;
    }
  cleanup0:

#ifdef TCL_COMPILE_DEBUG
    /*
     * Skip the stack depth check if an expansion is in progress.
     */

    ValidatePcAndStackTop(codePtr, pc, CURR_DEPTH, 0,
	    /*checkStack*/ auxObjList == NULL);
    if (traceInstructions) {
	fprintf(stdout, "%2d: %2d ", iPtr->numLevels, (int) CURR_DEPTH);
	TclPrintInstruction(codePtr, pc);
	fflush(stdout);
    }
#endif /* TCL_COMPILE_DEBUG */

#ifdef TCL_COMPILE_STATS
    iPtr->stats.instructionCount[*pc]++;
#endif

    /*
     * Check for asynchronous handlers [Bug 746722]; we do the check every
     * ASYNC_CHECK_COUNT_MASK instruction, of the form (2**n-1).
     */

    if ((instructionCount++ & ASYNC_CHECK_COUNT_MASK) == 0) {
	DECACHE_STACK_INFO();
	if (TclAsyncReady(iPtr)) {
	    result = Tcl_AsyncInvoke(interp, result);
	    if (result == TCL_ERROR) {
		CACHE_STACK_INFO();
		goto gotError;
	    }
	}

	if (TclCanceled(iPtr)) {
	    if (Tcl_Canceled(interp, TCL_LEAVE_ERR_MSG) == TCL_ERROR) {
		CACHE_STACK_INFO();
		goto gotError;
	    }
	}

	if (TclLimitReady(iPtr->limit)) {
	    if (Tcl_LimitCheck(interp) == TCL_ERROR) {
		CACHE_STACK_INFO();
		goto gotError;
	    }
	}
	CACHE_STACK_INFO();
    }

    TCL_DTRACE_INST_NEXT();

    /*
     * These two instructions account for 26% of all instructions (according
     * to measurements on tclbench by Ben Vitale
     * [http://www.cs.toronto.edu/syslab/pubs/tcl2005-vitale-zaleski.pdf]
     * Resolving them before the switch reduces the cost of branch
     * mispredictions, seems to improve runtime by 5% to 15%, and (amazingly!)
     * reduces total obj size.
     */

    if (*pc == INST_LOAD_SCALAR1) {
	goto instLoadScalar1;
    } else if (*pc == INST_PUSH1) {
	goto instPush1Peephole;
    }

    switch (*pc) {
    case INST_SYNTAX:
    case INST_RETURN_IMM: {
	int code = TclGetInt4AtPtr(pc+1);
	int level = TclGetUInt4AtPtr(pc+5);

	/*
	 * OBJ_AT_TOS is returnOpts, OBJ_UNDER_TOS is resultObjPtr.
	 */

	TRACE(("%u %u => ", code, level));
	result = TclProcessReturn(interp, code, level, OBJ_AT_TOS);
	if (result == TCL_OK) {
	    TRACE_APPEND(("continuing to next instruction (result=\"%.30s\")",
		    O2S(objResultPtr)));
	    NEXT_INST_F(9, 1, 0);
	}
	Tcl_SetObjResult(interp, OBJ_UNDER_TOS);
	if (*pc == INST_SYNTAX) {
	    iPtr->flags &= ~ERR_ALREADY_LOGGED;
	}
	cleanup = 2;
	goto processExceptionReturn;
    }

    case INST_RETURN_STK:
	TRACE(("=> "));
	objResultPtr = POP_OBJECT();
	result = Tcl_SetReturnOptions(interp, OBJ_AT_TOS);
	Tcl_DecrRefCount(OBJ_AT_TOS);
	OBJ_AT_TOS = objResultPtr;
	if (result == TCL_OK) {
	    TRACE_APPEND(("continuing to next instruction (result=\"%.30s\")",
		    O2S(objResultPtr)));
	    NEXT_INST_F(1, 0, 0);
	}
	Tcl_SetObjResult(interp, objResultPtr);
	cleanup = 1;
	goto processExceptionReturn;

    case INST_YIELD: {
	CoroutineData *corPtr = iPtr->execEnvPtr->corPtr;

	TRACE(("%.30s => ", O2S(OBJ_AT_TOS)));
	if (!corPtr) {
	    TRACE_APPEND(("ERROR: yield outside coroutine\n"));
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "yield can only be called in a coroutine", TCL_STRLEN));
	    Tcl_SetErrorCode(interp, "TCL", "COROUTINE", "ILLEGAL_YIELD",
		    NULL);
	    goto gotError;
	}

#ifdef TCL_COMPILE_DEBUG
	TRACE_WITH_OBJ(("yield, result="), iPtr->objResultPtr);
	if (traceInstructions) {
	    fprintf(stdout, "\n");
	}
#endif
	/* TIP #280: Record the last piece of info needed by
	 * 'TclGetSrcInfoForPc', and push the frame.
	 */
	
	bcFramePtr->data.tebc.pc = (char *) pc;
	iPtr->cmdFramePtr = bcFramePtr;

	if (iPtr->flags & INTERP_DEBUG_FRAME) {
	    TclArgumentBCEnter((Tcl_Interp *) iPtr, objv, objc,
		    codePtr, bcFramePtr, pc - codePtr->codeStart);
	}

	pc++;
	cleanup = 1;
	TEBC_YIELD();
	
	Tcl_SetObjResult(interp, OBJ_AT_TOS);
	TclNRAddCallback(interp, TclNRCoroutineActivateCallback, corPtr,
		INT2PTR(0), NULL, NULL);

	return TCL_OK;
    }

    case INST_TAILCALL: {
	Tcl_Obj *listPtr, *nsObjPtr;
        NRE_callback *tailcallPtr;

	opnd = TclGetUInt1AtPtr(pc+1);

	if (!(iPtr->varFramePtr->isProcCallFrame & 1)) {
	    TRACE(("%d => ERROR: tailcall in non-proc context\n", opnd));
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "tailcall can only be called from a proc or lambda",
		    TCL_STRLEN));
	    Tcl_SetErrorCode(interp, "TCL", "TAILCALL", "ILLEGAL", NULL);
	    goto gotError;
	}

#ifdef TCL_COMPILE_DEBUG
	{
	    register int i;

	    TRACE(("%d [", opnd));
	    for (i=opnd-1 ; i>=0 ; i--) {
		TRACE_APPEND(("\"%.30s\"", O2S(OBJ_AT_DEPTH(i))));
		if (i > 0) {
		    TRACE_APPEND((" "));
		}
	    }
	    TRACE_APPEND(("] => RETURN..."));
	}
#endif

	/*
	 * Push the evaluation of the called command into the NR callback
	 * stack.
	 */

	listPtr = Tcl_NewListObj(opnd, &OBJ_AT_DEPTH(opnd-1));
	nsObjPtr = Tcl_NewStringObj(iPtr->varFramePtr->nsPtr->fullName,
		TCL_STRLEN);
	Tcl_IncrRefCount(listPtr);
	Tcl_IncrRefCount(nsObjPtr);
	TclNRAddCallback(interp, TclNRTailcallEval, listPtr, nsObjPtr,
		NULL, NULL);

	/*
	 * Unstitch ourselves and do a [return].
	 */

	tailcallPtr = TOP_CB(interp);
	TOP_CB(interp) = tailcallPtr->nextPtr;
	iPtr->varFramePtr->tailcallPtr = tailcallPtr;
	result = TCL_RETURN;
	cleanup = opnd;
	goto processExceptionReturn;
    }

    case INST_DONE:
	if (tosPtr > initTosPtr) {
	    /*
	     * Set the interpreter's object result to point to the topmost
	     * object from the stack, and check for a possible [catch]. The
	     * stackTop's level and refCount will be handled by "processCatch"
	     * or "abnormalReturn".
	     */

	    Tcl_SetObjResult(interp, OBJ_AT_TOS);
#ifdef TCL_COMPILE_DEBUG
	    TRACE_WITH_OBJ(("=> return code=%d, result=", result),
		    iPtr->objResultPtr);
	    if (traceInstructions) {
		fprintf(stdout, "\n");
	    }
#endif
	    goto checkForCatch;
	}
	(void) POP_OBJECT();
	goto abnormalReturn;

    case INST_PUSH1:
    instPush1Peephole:
	PUSH_OBJECT(codePtr->objArrayPtr[TclGetUInt1AtPtr(pc+1)]);
	TRACE_WITH_OBJ(("%u => ", TclGetInt1AtPtr(pc+1)), OBJ_AT_TOS);
	pc += 2;
#if !TCL_COMPILE_DEBUG
	/*
	 * Runtime peephole optimisation: check if we are pushing again.
	 */

	if (*pc == INST_PUSH1) {
	    TCL_DTRACE_INST_NEXT();
	    goto instPush1Peephole;
	}
#endif
	NEXT_INST_F(0, 0, 0);

    case INST_PUSH4:
	objResultPtr = codePtr->objArrayPtr[TclGetUInt4AtPtr(pc+1)];
	TRACE_WITH_OBJ(("%u => ", TclGetUInt4AtPtr(pc+1)), objResultPtr);
	NEXT_INST_F(5, 0, 1);

    case INST_POP:
	TRACE_WITH_OBJ(("=> discarding "), OBJ_AT_TOS);
	objPtr = POP_OBJECT();
	TclDecrRefCount(objPtr);

	/*
	 * Runtime peephole optimisation: an INST_POP is scheduled at the end
	 * of most commands. If the next instruction is an INST_START_CMD,
	 * fall through to it.
	 */

	pc++;
#if !TCL_COMPILE_DEBUG
	if (*pc == INST_START_CMD) {
	    TCL_DTRACE_INST_NEXT();
	    goto instStartCmdPeephole;
	}
#endif
	NEXT_INST_F(0, 0, 0);

    case INST_START_CMD:
#if !TCL_COMPILE_DEBUG
    instStartCmdPeephole:
#endif
	/*
	 * Remark that if the interpreter is marked for deletion its
	 * compileEpoch is modified, so that the epoch check also verifies
	 * that the interp is not deleted. If no outside call has been made
	 * since the last check, it is safe to omit the check.
	 */

	iPtr->cmdCount += TclGetUInt4AtPtr(pc+5);
	if (!checkInterp) {
	    goto instStartCmdOK;
	} else if (((codePtr->compileEpoch == iPtr->compileEpoch)
		&& (codePtr->nsEpoch == iPtr->varFramePtr->nsPtr->resolverEpoch))
		|| (codePtr->flags & TCL_BYTECODE_PRECOMPILED)) {
	    checkInterp = 0;
	instStartCmdOK:
	    NEXT_INST_F(9, 0, 0);
	} else {
	    const char *bytes;

	    length = 0;

	    /*
	     * We used to switch to direct eval; for NRE-awareness we now
	     * compile and eval the command so that this evaluation does not
	     * add a new TEBC instance. [Bug 2910748]
	     */

	    if (TclInterpReady(interp) == TCL_ERROR) {
		goto gotError;
	    }

	    codePtr->flags |= TCL_BYTECODE_RECOMPILE;
	    bytes = GetSrcInfoForPc(pc, codePtr, &length, NULL);
	    opnd = TclGetUInt4AtPtr(pc+1);
	    pc += (opnd-1);
	    PUSH_OBJECT(Tcl_NewStringObj(bytes, length));
	    goto instEvalStk;
	}

    case INST_NOP:
	pc += 1;
	goto cleanup0;

    case INST_DUP:
	objResultPtr = OBJ_AT_TOS;
	TRACE_WITH_OBJ(("=> "), objResultPtr);
	NEXT_INST_F(1, 0, 1);

    case INST_OVER:
	opnd = TclGetUInt4AtPtr(pc+1);
	objResultPtr = OBJ_AT_DEPTH(opnd);
	TRACE_WITH_OBJ(("=> "), objResultPtr);
	NEXT_INST_F(5, 0, 1);

    case INST_REVERSE: {
	Tcl_Obj **a, **b;

	opnd = TclGetUInt4AtPtr(pc+1);
	a = tosPtr-(opnd-1);
	b = tosPtr;
	while (a<b) {
	    tmpPtr = *a;
	    *a = *b;
	    *b = tmpPtr;
	    a++; b--;
	}
	NEXT_INST_F(5, 0, 0);
    }

    case INST_CONCAT1: {
	int appendLen = 0;
	char *bytes, *p;
	Tcl_Obj **currPtr;
	int onlyb = 1;

	opnd = TclGetUInt1AtPtr(pc+1);

	/*
	 * Detect only-bytearray-or-null case.
	 */

	for (currPtr=&OBJ_AT_DEPTH(opnd-1); currPtr<=&OBJ_AT_TOS; currPtr++) {
	    if (((*currPtr)->typePtr != &tclByteArrayType)
		    && ((*currPtr)->bytes != tclEmptyStringRep)) {
		onlyb = 0;
		break;
	    } else if (((*currPtr)->typePtr == &tclByteArrayType) &&
		    ((*currPtr)->bytes != NULL)) {
		onlyb = 0;
		break;
	    }
	}

	/*
	 * Compute the length to be appended.
	 */

	if (onlyb) {
	    for (currPtr = &OBJ_AT_DEPTH(opnd-2);
		    appendLen >= 0 && currPtr <= &OBJ_AT_TOS; currPtr++) {
		if ((*currPtr)->bytes != tclEmptyStringRep) {
		    Tcl_GetByteArrayFromObj(*currPtr, &length);
		    appendLen += length;
		}
	    }
	} else {
	    for (currPtr = &OBJ_AT_DEPTH(opnd-2);
		    appendLen >= 0 && currPtr <= &OBJ_AT_TOS; currPtr++) {
		bytes = TclGetStringFromObj(*currPtr, &length);
		if (bytes != NULL) {
		    appendLen += length;
		}
	    }
	}

	if (appendLen < 0) {
	    /* TODO: convert panic to error ? */
	    Tcl_Panic("max size for a Tcl value (%d bytes) exceeded", INT_MAX);
	}

	/*
	 * If nothing is to be appended, just return the first object by
	 * dropping all the others from the stack; this saves both the
	 * computation and copy of the string rep of the first object,
	 * enabling the fast '$x[set x {}]' idiom for 'K $x [set x {}]'.
	 */

	if (appendLen == 0) {
	    TRACE_WITH_OBJ(("%u => ", opnd), objResultPtr);
	    NEXT_INST_V(2, (opnd-1), 0);
	}

	/*
	 * If the first object is shared, we need a new obj for the result;
	 * otherwise, we can reuse the first object. In any case, make sure it
	 * has enough room to accomodate all the concatenated bytes. Note that
	 * if it is unshared its bytes are copied by ckrealloc, so that we set
	 * the loop parameters to avoid copying them again: p points to the
	 * end of the already copied bytes, currPtr to the second object.
	 */

	objResultPtr = OBJ_AT_DEPTH(opnd-1);
	if (!onlyb) {
	    bytes = TclGetStringFromObj(objResultPtr, &length);
	    if (length + appendLen < 0) {
		/* TODO: convert panic to error ? */
		Tcl_Panic("max size for a Tcl value (%d bytes) exceeded",
			INT_MAX);
	    }
#if !TCL_COMPILE_DEBUG
	    if (bytes != tclEmptyStringRep && !Tcl_IsShared(objResultPtr)) {
		TclFreeIntRep(objResultPtr);
		objResultPtr->bytes = ckrealloc(bytes, length+appendLen+1);
		objResultPtr->length = length + appendLen;
		p = TclGetString(objResultPtr) + length;
		currPtr = &OBJ_AT_DEPTH(opnd - 2);
	    } else
#endif
	    {
		p = ckalloc(length + appendLen + 1);
		TclNewObj(objResultPtr);
		objResultPtr->bytes = p;
		objResultPtr->length = length + appendLen;
		currPtr = &OBJ_AT_DEPTH(opnd - 1);
	    }

	    /*
	     * Append the remaining characters.
	     */

	    for (; currPtr <= &OBJ_AT_TOS; currPtr++) {
		bytes = TclGetStringFromObj(*currPtr, &length);
		if (bytes != NULL) {
		    memcpy(p, bytes, (size_t) length);
		    p += length;
		}
	    }
	    *p = '\0';
	} else {
	    bytes = (char *) Tcl_GetByteArrayFromObj(objResultPtr, &length);
	    if (length + appendLen < 0) {
		/* TODO: convert panic to error ? */
		Tcl_Panic("max size for a Tcl value (%d bytes) exceeded",
			INT_MAX);
	    }
#if !TCL_COMPILE_DEBUG
	    if (!Tcl_IsShared(objResultPtr)) {
		bytes = (char *) Tcl_SetByteArrayLength(objResultPtr,
			length + appendLen);
		p = bytes + length;
		currPtr = &OBJ_AT_DEPTH(opnd - 2);
	    } else
#endif
	    {
		TclNewObj(objResultPtr);
		bytes = (char *) Tcl_SetByteArrayLength(objResultPtr,
			length + appendLen);
		p = bytes;
		currPtr = &OBJ_AT_DEPTH(opnd - 1);
	    }

	    /*
	     * Append the remaining characters.
	     */

	    for (; currPtr <= &OBJ_AT_TOS; currPtr++) {
		if ((*currPtr)->bytes != tclEmptyStringRep) {
		    bytes = (char *) Tcl_GetByteArrayFromObj(*currPtr,&length);
		    memcpy(p, bytes, (size_t) length);
		    p += length;
		}
	    }
	}

	TRACE_WITH_OBJ(("%u => ", opnd), objResultPtr);
	NEXT_INST_V(2, opnd, 1);
    }

    case INST_EXPAND_START:
	/*
	 * Push an element to the auxObjList. This records the current
	 * stack depth - i.e., the point in the stack where the expanded
	 * command starts.
	 *
	 * Use a Tcl_Obj as linked list element; slight mem waste, but faster
	 * allocation than ckalloc. This also abuses the Tcl_Obj structure, as
	 * we do not define a special tclObjType for it. It is not dangerous
	 * as the obj is never passed anywhere, so that all manipulations are
	 * performed here and in INST_INVOKE_EXPANDED (in case of an expansion
	 * error, also in INST_EXPAND_STKTOP).
	 */

	TclNewObj(objPtr);
	objPtr->internalRep.ptrAndLongRep.value = CURR_DEPTH;
	PUSH_TAUX_OBJ(objPtr);
	NEXT_INST_F(1, 0, 0);

    case INST_EXPAND_STKTOP: {
	int i;
	ptrdiff_t moved;

	/*
	 * Make sure that the element at stackTop is a list; if not, just
	 * leave with an error. Note that the element from the expand list
	 * will be removed at checkForCatch.
	 */

	objPtr = OBJ_AT_TOS;
	if (TclListObjGetElements(interp, objPtr, &objc, &objv) != TCL_OK) {
	    TRACE_WITH_OBJ(("%.30s => ERROR: ", O2S(objPtr)),
		    Tcl_GetObjResult(interp));
	    goto gotError;
	}
	(void) POP_OBJECT();

	/*
	 * Make sure there is enough room in the stack to expand this list
	 * *and* process the rest of the command (at least up to the next
	 * argument expansion or command end). The operand is the current
	 * stack depth, as seen by the compiler.
	 */

	length = objc + (codePtr->maxStackDepth - TclGetInt4AtPtr(pc+1));
	DECACHE_STACK_INFO();
	moved = GrowEvaluationStack(iPtr->execEnvPtr, length, 1)
		- (Tcl_Obj **) TD;
	if (moved) {
	    /*
	     * Change the global data to point to the new stack: move the
	     * TEBCdataPtr TD, recompute the position of every other
	     * stack-allocated parameter, update the stack pointers.
	     */

	    esPtr = iPtr->execEnvPtr->execStackPtr;
	    TD = (TEBCdata *) (((Tcl_Obj **)TD) + moved);

	    catchTop += moved;
	    tosPtr += moved;
	}

	/*
	 * Expand the list at stacktop onto the stack; free the list. Knowing
	 * that it has a freeIntRepProc we use Tcl_DecrRefCount().
	 */

	for (i = 0; i < objc; i++) {
	    PUSH_OBJECT(objv[i]);
	}

	Tcl_DecrRefCount(objPtr);
	NEXT_INST_F(5, 0, 0);
    }

    case INST_EXPR_STK: {
	ByteCode *newCodePtr;

	bcFramePtr->data.tebc.pc = (char *) pc;
	iPtr->cmdFramePtr = bcFramePtr;
	DECACHE_STACK_INFO();
	newCodePtr = CompileExprObj(interp, OBJ_AT_TOS);
	CACHE_STACK_INFO();
	cleanup = 1;
	pc++;
	TEBC_YIELD();
	return TclNRExecuteByteCode(interp, newCodePtr);
    }

	/*
	 * INVOCATION BLOCK
	 */

    instEvalStk:
    case INST_EVAL_STK:
	bcFramePtr->data.tebc.pc = (char *) pc;
	iPtr->cmdFramePtr = bcFramePtr;

	cleanup = 1;
	pc += 1;
	TEBC_YIELD();
	return TclNREvalObjEx(interp, OBJ_AT_TOS, 0, NULL, 0);

    case INST_INVOKE_EXPANDED:
	CLANG_ASSERT(auxObjList);
	objc = CURR_DEPTH - auxObjList->internalRep.ptrAndLongRep.value;
	POP_TAUX_OBJ();
	if (objc) {
	    pcAdjustment = 1;
	    goto doInvocation;
	}

	/*
	 * Nothing was expanded, return {}.
	 */

	TclNewObj(objResultPtr);
	NEXT_INST_F(1, 0, 1);

    case INST_INVOKE_STK4:
	objc = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	goto doInvocation;

    case INST_INVOKE_STK1:
	objc = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;

    doInvocation:
	objv = &OBJ_AT_DEPTH(objc-1);
	cleanup = objc;

#ifdef TCL_COMPILE_DEBUG
	if (tclTraceExec >= 2) {
	    int i;

	    if (traceInstructions) {
		strncpy(cmdNameBuf, TclGetString(objv[0]), 20);
		TRACE(("%u => call ", objc));
	    } else {
		fprintf(stdout, "%d: (%u) invoking ", iPtr->numLevels,
			(unsigned)(pc - codePtr->codeStart));
	    }
	    for (i = 0;  i < objc;  i++) {
		TclPrintObject(stdout, objv[i], 15);
		fprintf(stdout, " ");
	    }
	    fprintf(stdout, "\n");
	    fflush(stdout);
	}
#endif /*TCL_COMPILE_DEBUG*/

	/*
	 * Finally, let TclEvalObjv handle the command.
	 *
	 * TIP #280: Record the last piece of info needed by
	 * 'TclGetSrcInfoForPc', and push the frame.
	 */

	bcFramePtr->data.tebc.pc = (char *) pc;
	iPtr->cmdFramePtr = bcFramePtr;

	if (iPtr->flags & INTERP_DEBUG_FRAME) {
	    TclArgumentBCEnter((Tcl_Interp *) iPtr, objv, objc,
		    codePtr, bcFramePtr, pc - codePtr->codeStart);
	}

	DECACHE_STACK_INFO();

	pc += pcAdjustment;
	TEBC_YIELD();
	return TclNREvalObjv(interp, objc, objv,
		TCL_EVAL_NOERR, NULL);

#if TCL_SUPPORT_84_BYTECODE
    case INST_CALL_BUILTIN_FUNC1:
	/*
	 * Call one of the built-in pre-8.5 Tcl math functions. This
	 * translates to INST_INVOKE_STK1 with the first argument of
	 * ::tcl::mathfunc::$objv[0]. We need to insert the named math
	 * function into the stack.
	 */

	opnd = TclGetUInt1AtPtr(pc+1);
	if ((opnd < 0) || (opnd > LAST_BUILTIN_FUNC)) {
	    TRACE(("UNRECOGNIZED BUILTIN FUNC CODE %d\n", opnd));
	    Tcl_Panic("TclNRExecuteByteCode: unrecognized builtin function code %d", opnd);
	}

	TclNewLiteralStringObj(objPtr, "::tcl::mathfunc::");
	Tcl_AppendToObj(objPtr, tclBuiltinFuncTable[opnd].name, TCL_STRLEN);

	/*
	 * Only 0, 1 or 2 args.
	 */

	{
	    int numArgs = tclBuiltinFuncTable[opnd].numArgs;
	    Tcl_Obj *tmpPtr1, *tmpPtr2;

	    if (numArgs == 0) {
		PUSH_OBJECT(objPtr);
	    } else if (numArgs == 1) {
		tmpPtr1 = POP_OBJECT();
		PUSH_OBJECT(objPtr);
		PUSH_OBJECT(tmpPtr1);
		Tcl_DecrRefCount(tmpPtr1);
	    } else {
		tmpPtr2 = POP_OBJECT();
		tmpPtr1 = POP_OBJECT();
		PUSH_OBJECT(objPtr);
		PUSH_OBJECT(tmpPtr1);
		PUSH_OBJECT(tmpPtr2);
		Tcl_DecrRefCount(tmpPtr1);
		Tcl_DecrRefCount(tmpPtr2);
	    }
	    objc = numArgs + 1;
	}
	pcAdjustment = 2;
	goto doInvocation;

    case INST_CALL_FUNC1:
	/*
	 * Call a non-builtin Tcl math function previously registered by a
	 * call to Tcl_CreateMathFunc pre-8.5. This is essentially
	 * INST_INVOKE_STK1 converting the first arg to
	 * ::tcl::mathfunc::$objv[0].
	 */

	objc = TclGetUInt1AtPtr(pc+1);	/* Number of arguments. The function
					 * name is the 0-th argument. */

	objPtr = OBJ_AT_DEPTH(objc-1);
	TclNewLiteralStringObj(tmpPtr, "::tcl::mathfunc::");
	Tcl_AppendObjToObj(tmpPtr, objPtr);
	Tcl_DecrRefCount(objPtr);

	/*
	 * Variation of PUSH_OBJECT.
	 */

	OBJ_AT_DEPTH(objc-1) = tmpPtr;
	Tcl_IncrRefCount(tmpPtr);

	pcAdjustment = 2;
	goto doInvocation;
#else
    /*
     * INST_CALL_BUILTIN_FUNC1 and INST_CALL_FUNC1 were made obsolete by the
     * changes to add a ::tcl::mathfunc namespace in 8.5. Optional support
     * remains for existing bytecode precompiled files.
     */

    case INST_CALL_BUILTIN_FUNC1:
	Tcl_Panic("TclNRExecuteByteCode: obsolete INST_CALL_BUILTIN_FUNC1 found");
    case INST_CALL_FUNC1:
	Tcl_Panic("TclNRExecuteByteCode: obsolete INST_CALL_FUNC1 found");
#endif

    /*
     * -----------------------------------------------------------------
     *	   Start of INST_LOAD instructions.
     *
     * WARNING: more 'goto' here than your doctor recommended! The different
     * instructions set the value of some variables and then jump to some
     * common execution code.
     */

    case INST_LOAD_SCALAR1:
    instLoadScalar1:
	opnd = TclGetUInt1AtPtr(pc+1);
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%u => ", opnd));
	if (TclIsVarDirectReadable(varPtr)) {
	    /*
	     * No errors, no traces: just get the value.
	     */

	    objResultPtr = varPtr->value.objPtr;
	    TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	    NEXT_INST_F(2, 0, 1);
	}
	pcAdjustment = 2;
	cleanup = 0;
	arrayPtr = NULL;
	part1Ptr = part2Ptr = NULL;
	goto doCallPtrGetVar;

    case INST_LOAD_SCALAR4:
	opnd = TclGetUInt4AtPtr(pc+1);
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%u => ", opnd));
	if (TclIsVarDirectReadable(varPtr)) {
	    /*
	     * No errors, no traces: just get the value.
	     */

	    objResultPtr = varPtr->value.objPtr;
	    TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	    NEXT_INST_F(5, 0, 1);
	}
	pcAdjustment = 5;
	cleanup = 0;
	arrayPtr = NULL;
	part1Ptr = part2Ptr = NULL;
	goto doCallPtrGetVar;

    case INST_LOAD_ARRAY4:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	goto doLoadArray;

    case INST_LOAD_ARRAY1:
	opnd = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;

    doLoadArray:
	part1Ptr = NULL;
	part2Ptr = OBJ_AT_TOS;
	arrayPtr = LOCAL(opnd);
	while (TclIsVarLink(arrayPtr)) {
	    arrayPtr = arrayPtr->value.linkPtr;
	}
	TRACE(("%u \"%.30s\" => ", opnd, O2S(part2Ptr)));
	if (TclIsVarArray(arrayPtr) && !ReadTraced(arrayPtr)) {
	    varPtr = VarHashFindVar(arrayPtr->value.tablePtr, part2Ptr);
	    if (varPtr && TclIsVarDirectReadable(varPtr)) {
		/*
		 * No errors, no traces: just get the value.
		 */

		objResultPtr = varPtr->value.objPtr;
		TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
		NEXT_INST_F(pcAdjustment, 1, 1);
	    }
	}
	varPtr = TclLookupArrayElement(interp, part1Ptr, part2Ptr,
		TCL_LEAVE_ERR_MSG, "read", 0, 1, arrayPtr, opnd);
	if (varPtr == NULL) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	cleanup = 1;
	goto doCallPtrGetVar;

    case INST_LOAD_ARRAY_STK:
	cleanup = 2;
	part2Ptr = OBJ_AT_TOS;		/* element name */
	objPtr = OBJ_UNDER_TOS;		/* array name */
	TRACE(("\"%.30s(%.30s)\" => ", O2S(objPtr), O2S(part2Ptr)));
	goto doLoadStk;

    case INST_LOAD_STK:
    case INST_LOAD_SCALAR_STK:
	cleanup = 1;
	part2Ptr = NULL;
	objPtr = OBJ_AT_TOS;		/* variable name */
	TRACE(("\"%.30s\" => ", O2S(objPtr)));

    doLoadStk:
	part1Ptr = objPtr;
	varPtr = TclObjLookupVarEx(interp, part1Ptr, part2Ptr,
		TCL_LEAVE_ERR_MSG, "read", /*createPart1*/0, /*createPart2*/1,
		&arrayPtr);
	if (!varPtr) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}

	if (TclIsVarDirectReadable2(varPtr, arrayPtr)) {
	    /*
	     * No errors, no traces: just get the value.
	     */

	    objResultPtr = varPtr->value.objPtr;
	    TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	    NEXT_INST_V(1, cleanup, 1);
	}
	pcAdjustment = 1;
	opnd = -1;

    doCallPtrGetVar:
	/*
	 * There are either errors or the variable is traced: call
	 * TclPtrGetVar to process fully.
	 */

	DECACHE_STACK_INFO();
	objResultPtr = TclPtrGetVar(interp, varPtr, arrayPtr,
		part1Ptr, part2Ptr, TCL_LEAVE_ERR_MSG, opnd);
	CACHE_STACK_INFO();
	if (!objResultPtr) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_V(pcAdjustment, cleanup, 1);

    /*
     *	   End of INST_LOAD instructions.
     * -----------------------------------------------------------------
     *	   Start of INST_STORE and related instructions.
     *
     * WARNING: more 'goto' here than your doctor recommended! The different
     * instructions set the value of some variables and then jump to somme
     * common execution code.
     */

    {
	int storeFlags;

    case INST_STORE_ARRAY4:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	goto doStoreArrayDirect;

    case INST_STORE_ARRAY1:
	opnd = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;

    doStoreArrayDirect:
	valuePtr = OBJ_AT_TOS;
	part2Ptr = OBJ_UNDER_TOS;
	arrayPtr = LOCAL(opnd);
	TRACE(("%u \"%.30s\" <- \"%.30s\" => ", opnd, O2S(part2Ptr),
		O2S(valuePtr)));
	while (TclIsVarLink(arrayPtr)) {
	    arrayPtr = arrayPtr->value.linkPtr;
	}
	if (TclIsVarArray(arrayPtr) && !WriteTraced(arrayPtr)) {
	    varPtr = VarHashFindVar(arrayPtr->value.tablePtr, part2Ptr);
	    if (varPtr && TclIsVarDirectWritable(varPtr)) {
		tosPtr--;
		Tcl_DecrRefCount(OBJ_AT_TOS);
		OBJ_AT_TOS = valuePtr;
		goto doStoreVarDirect;
	    }
	}
	cleanup = 2;
	storeFlags = TCL_LEAVE_ERR_MSG;
	part1Ptr = NULL;
	goto doStoreArrayDirectFailed;

    case INST_STORE_SCALAR4:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	goto doStoreScalarDirect;

    case INST_STORE_SCALAR1:
	opnd = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;

    doStoreScalarDirect:
	valuePtr = OBJ_AT_TOS;
	varPtr = LOCAL(opnd);
	TRACE(("%u <- \"%.30s\" => ", opnd, O2S(valuePtr)));
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	if (!TclIsVarDirectWritable(varPtr)) {
	    storeFlags = TCL_LEAVE_ERR_MSG;
	    part1Ptr = NULL;
	    goto doStoreScalar;
	}

	/*
	 * No traces, no errors, plain 'set': we can safely inline. The value
	 * *will* be set to what's requested, so that the stack top remains
	 * pointing to the same Tcl_Obj.
	 */

    doStoreVarDirect:
	valuePtr = varPtr->value.objPtr;
	if (valuePtr != NULL) {
	    TclDecrRefCount(valuePtr);
	}
	objResultPtr = OBJ_AT_TOS;
	varPtr->value.objPtr = objResultPtr;
#ifndef TCL_COMPILE_DEBUG
	if (*(pc+pcAdjustment) == INST_POP) {
	    tosPtr--;
	    NEXT_INST_F((pcAdjustment+1), 0, 0);
	}
#else
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
#endif
	Tcl_IncrRefCount(objResultPtr);
	NEXT_INST_F(pcAdjustment, 0, 0);

    case INST_LAPPEND_STK:
	valuePtr = OBJ_AT_TOS; /* value to append */
	part2Ptr = NULL;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE
		| TCL_LIST_ELEMENT);
	goto doStoreStk;

    case INST_LAPPEND_ARRAY_STK:
	valuePtr = OBJ_AT_TOS; /* value to append */
	part2Ptr = OBJ_UNDER_TOS;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE
		| TCL_LIST_ELEMENT);
	goto doStoreStk;

    case INST_APPEND_STK:
	valuePtr = OBJ_AT_TOS; /* value to append */
	part2Ptr = NULL;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE);
	goto doStoreStk;

    case INST_APPEND_ARRAY_STK:
	valuePtr = OBJ_AT_TOS; /* value to append */
	part2Ptr = OBJ_UNDER_TOS;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE);
	goto doStoreStk;

    case INST_STORE_ARRAY_STK:
	valuePtr = OBJ_AT_TOS;
	part2Ptr = OBJ_UNDER_TOS;
	storeFlags = TCL_LEAVE_ERR_MSG;
	goto doStoreStk;

    case INST_STORE_STK:
    case INST_STORE_SCALAR_STK:
	valuePtr = OBJ_AT_TOS;
	part2Ptr = NULL;
	storeFlags = TCL_LEAVE_ERR_MSG;

    doStoreStk:
	objPtr = OBJ_AT_DEPTH(1 + (part2Ptr != NULL)); /* variable name */
	part1Ptr = objPtr;
#ifdef TCL_COMPILE_DEBUG
	if (part2Ptr == NULL) {
	    TRACE(("\"%.30s\" <- \"%.30s\" =>", O2S(part1Ptr),O2S(valuePtr)));
	} else {
	    TRACE(("\"%.30s(%.30s)\" <- \"%.30s\" => ",
		    O2S(part1Ptr), O2S(part2Ptr), O2S(valuePtr)));
	}
#endif
	varPtr = TclObjLookupVarEx(interp, objPtr,part2Ptr, TCL_LEAVE_ERR_MSG,
		"set", /*createPart1*/ 1, /*createPart2*/ 1, &arrayPtr);
	if (!varPtr) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	cleanup = ((part2Ptr == NULL)? 2 : 3);
	pcAdjustment = 1;
	opnd = -1;
	goto doCallPtrSetVar;

    case INST_LAPPEND_ARRAY4:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE
		| TCL_LIST_ELEMENT);
	goto doStoreArray;

    case INST_LAPPEND_ARRAY1:
	opnd = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE
		| TCL_LIST_ELEMENT);
	goto doStoreArray;

    case INST_APPEND_ARRAY4:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE);
	goto doStoreArray;

    case INST_APPEND_ARRAY1:
	opnd = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE);
	goto doStoreArray;

    doStoreArray:
	valuePtr = OBJ_AT_TOS;
	part2Ptr = OBJ_UNDER_TOS;
	arrayPtr = LOCAL(opnd);
	TRACE(("%u \"%.30s\" <- \"%.30s\" => ", opnd, O2S(part2Ptr),
		O2S(valuePtr)));
	while (TclIsVarLink(arrayPtr)) {
	    arrayPtr = arrayPtr->value.linkPtr;
	}
	cleanup = 2;
	part1Ptr = NULL;

    doStoreArrayDirectFailed:
	varPtr = TclLookupArrayElement(interp, part1Ptr, part2Ptr,
		TCL_LEAVE_ERR_MSG, "set", 1, 1, arrayPtr, opnd);
	if (!varPtr) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	goto doCallPtrSetVar;

    case INST_LAPPEND_SCALAR4:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE
		| TCL_LIST_ELEMENT);
	goto doStoreScalar;

    case INST_LAPPEND_SCALAR1:
	opnd = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE
		| TCL_LIST_ELEMENT);
	goto doStoreScalar;

    case INST_APPEND_SCALAR4:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE);
	goto doStoreScalar;

    case INST_APPEND_SCALAR1:
	opnd = TclGetUInt1AtPtr(pc+1);
	pcAdjustment = 2;
	storeFlags = (TCL_LEAVE_ERR_MSG | TCL_APPEND_VALUE);
	goto doStoreScalar;

    doStoreScalar:
	valuePtr = OBJ_AT_TOS;
	varPtr = LOCAL(opnd);
	TRACE(("%u <- \"%.30s\" => ", opnd, O2S(valuePtr)));
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	cleanup = 1;
	arrayPtr = NULL;
	part1Ptr = part2Ptr = NULL;

    doCallPtrSetVar:
	DECACHE_STACK_INFO();
	objResultPtr = TclPtrSetVar(interp, varPtr, arrayPtr,
		part1Ptr, part2Ptr, valuePtr, storeFlags, opnd);
	CACHE_STACK_INFO();
	if (!objResultPtr) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
#ifndef TCL_COMPILE_DEBUG
	if (*(pc+pcAdjustment) == INST_POP) {
	    NEXT_INST_V((pcAdjustment+1), cleanup, 0);
	}
#endif
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_V(pcAdjustment, cleanup, 1);
    }

    /*
     *	   End of INST_STORE and related instructions.
     * -----------------------------------------------------------------
     *	   Start of INST_INCR instructions.
     *
     * WARNING: more 'goto' here than your doctor recommended! The different
     * instructions set the value of some variables and then jump to somme
     * common execution code.
     */

/*TODO: Consider more untangling here; merge with LOAD and STORE ? */

    {
	Tcl_Obj *incrPtr;
#ifndef NO_WIDE_TYPE
	Tcl_WideInt w;
#endif
	long increment;

    case INST_INCR_SCALAR1:
    case INST_INCR_ARRAY1:
    case INST_INCR_ARRAY_STK:
    case INST_INCR_SCALAR_STK:
    case INST_INCR_STK:
	opnd = TclGetUInt1AtPtr(pc+1);
	incrPtr = POP_OBJECT();
	switch (*pc) {
	case INST_INCR_SCALAR1:
	    pcAdjustment = 2;
	    goto doIncrScalar;
	case INST_INCR_ARRAY1:
	    pcAdjustment = 2;
	    goto doIncrArray;
	default:
	    pcAdjustment = 1;
	    goto doIncrStk;
	}

    case INST_INCR_ARRAY_STK_IMM:
    case INST_INCR_SCALAR_STK_IMM:
    case INST_INCR_STK_IMM:
	increment = TclGetInt1AtPtr(pc+1);
	incrPtr = Tcl_NewIntObj(increment);
	Tcl_IncrRefCount(incrPtr);
	pcAdjustment = 2;

    doIncrStk:
	if ((*pc == INST_INCR_ARRAY_STK_IMM)
		|| (*pc == INST_INCR_ARRAY_STK)) {
	    part2Ptr = OBJ_AT_TOS;
	    objPtr = OBJ_UNDER_TOS;
	    TRACE(("\"%.30s(%.30s)\" (by %ld) => ",
		    O2S(objPtr), O2S(part2Ptr), increment));
	} else {
	    part2Ptr = NULL;
	    objPtr = OBJ_AT_TOS;
	    TRACE(("\"%.30s\" (by %ld) => ", O2S(objPtr), increment));
	}
	part1Ptr = objPtr;
	opnd = -1;
	varPtr = TclObjLookupVarEx(interp, objPtr, part2Ptr,
		TCL_LEAVE_ERR_MSG, "read", 1, 1, &arrayPtr);
	if (!varPtr) {
	    Tcl_AddObjErrorInfo(interp,
		    "\n    (reading value of variable to increment)", -1);
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    Tcl_DecrRefCount(incrPtr);
	    goto gotError;
	}
	cleanup = ((part2Ptr == NULL)? 1 : 2);
	goto doIncrVar;

    case INST_INCR_ARRAY1_IMM:
	opnd = TclGetUInt1AtPtr(pc+1);
	increment = TclGetInt1AtPtr(pc+2);
	incrPtr = Tcl_NewIntObj(increment);
	Tcl_IncrRefCount(incrPtr);
	pcAdjustment = 3;

    doIncrArray:
	part1Ptr = NULL;
	part2Ptr = OBJ_AT_TOS;
	arrayPtr = LOCAL(opnd);
	cleanup = 1;
	while (TclIsVarLink(arrayPtr)) {
	    arrayPtr = arrayPtr->value.linkPtr;
	}
	TRACE(("%u \"%.30s\" (by %ld) => ", opnd, O2S(part2Ptr), increment));
	varPtr = TclLookupArrayElement(interp, part1Ptr, part2Ptr,
		TCL_LEAVE_ERR_MSG, "read", 1, 1, arrayPtr, opnd);
	if (!varPtr) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    Tcl_DecrRefCount(incrPtr);
	    goto gotError;
	}
	goto doIncrVar;

    case INST_INCR_SCALAR1_IMM:
	opnd = TclGetUInt1AtPtr(pc+1);
	increment = TclGetInt1AtPtr(pc+2);
	pcAdjustment = 3;
	cleanup = 0;
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}

	if (TclIsVarDirectModifyable(varPtr)) {
	    ClientData ptr;
	    int type;

	    objPtr = varPtr->value.objPtr;
	    if (GetNumberFromObj(NULL, objPtr, &ptr, &type) == TCL_OK) {
		if (type == TCL_NUMBER_LONG) {
		    long augend = *((const long *)ptr);
		    long sum = augend + increment;

		    /*
		     * Overflow when (augend and sum have different sign) and
		     * (augend and increment have the same sign). This is
		     * encapsulated in the Overflowing macro.
		     */

		    if (!Overflowing(augend, increment, sum)) {
			TRACE(("%u %ld => ", opnd, increment));
			if (Tcl_IsShared(objPtr)) {
			    objPtr->refCount--;	/* We know it's shared. */
			    TclNewLongObj(objResultPtr, sum);
			    Tcl_IncrRefCount(objResultPtr);
			    varPtr->value.objPtr = objResultPtr;
			} else {
			    objResultPtr = objPtr;
			    TclSetLongObj(objPtr, sum);
			}
			goto doneIncr;
		    }
#ifndef NO_WIDE_TYPE
		    w = (Tcl_WideInt)augend;

		    TRACE(("%u %ld => ", opnd, increment));
		    if (Tcl_IsShared(objPtr)) {
			objPtr->refCount--;	/* We know it's shared. */
			objResultPtr = Tcl_NewWideIntObj(w+increment);
			Tcl_IncrRefCount(objResultPtr);
			varPtr->value.objPtr = objResultPtr;
		    } else {
			objResultPtr = objPtr;

			/*
			 * We know the sum value is outside the long range;
			 * use macro form that doesn't range test again.
			 */

			TclSetWideIntObj(objPtr, w+increment);
		    }
		    goto doneIncr;
#endif
		}	/* end if (type == TCL_NUMBER_LONG) */
#ifndef NO_WIDE_TYPE
		if (type == TCL_NUMBER_WIDE) {
		    Tcl_WideInt sum;

		    w = *((const Tcl_WideInt *) ptr);
		    sum = w + increment;

		    /*
		     * Check for overflow.
		     */

		    if (!Overflowing(w, increment, sum)) {
			TRACE(("%u %ld => ", opnd, increment));
			if (Tcl_IsShared(objPtr)) {
			    objPtr->refCount--;	/* We know it's shared. */
			    objResultPtr = Tcl_NewWideIntObj(sum);
			    Tcl_IncrRefCount(objResultPtr);
			    varPtr->value.objPtr = objResultPtr;
			} else {
			    objResultPtr = objPtr;

			    /*
			     * We *do not* know the sum value is outside the
			     * long range (wide + long can yield long); use
			     * the function call that checks range.
			     */

			    Tcl_SetWideIntObj(objPtr, sum);
			}
			goto doneIncr;
		    }
		}
#endif
	    }
	    if (Tcl_IsShared(objPtr)) {
		objPtr->refCount--;	/* We know it's shared */
		objResultPtr = Tcl_DuplicateObj(objPtr);
		Tcl_IncrRefCount(objResultPtr);
		varPtr->value.objPtr = objResultPtr;
	    } else {
		objResultPtr = objPtr;
	    }
	    TclNewLongObj(incrPtr, increment);
	    if (TclIncrObj(interp, objResultPtr, incrPtr) != TCL_OK) {
		Tcl_DecrRefCount(incrPtr);
		TRACE_APPEND(("ERROR: %.30s\n",
			O2S(Tcl_GetObjResult(interp))));
		goto gotError;
	    }
	    Tcl_DecrRefCount(incrPtr);
	    goto doneIncr;
	}

	/*
	 * All other cases, flow through to generic handling.
	 */

	TclNewLongObj(incrPtr, increment);
	Tcl_IncrRefCount(incrPtr);

    doIncrScalar:
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	arrayPtr = NULL;
	part1Ptr = part2Ptr = NULL;
	cleanup = 0;
	TRACE(("%u %ld => ", opnd, increment));

    doIncrVar:
	if (TclIsVarDirectModifyable2(varPtr, arrayPtr)) {
	    objPtr = varPtr->value.objPtr;
	    if (Tcl_IsShared(objPtr)) {
		objPtr->refCount--;	/* We know it's shared */
		objResultPtr = Tcl_DuplicateObj(objPtr);
		Tcl_IncrRefCount(objResultPtr);
		varPtr->value.objPtr = objResultPtr;
	    } else {
		objResultPtr = objPtr;
	    }
	    if (TclIncrObj(interp, objResultPtr, incrPtr) != TCL_OK) {
		Tcl_DecrRefCount(incrPtr);
		TRACE_APPEND(("ERROR: %.30s\n",
			O2S(Tcl_GetObjResult(interp))));
		goto gotError;
	    }
	    Tcl_DecrRefCount(incrPtr);
	} else {
	    DECACHE_STACK_INFO();
	    objResultPtr = TclPtrIncrObjVar(interp, varPtr, arrayPtr,
		    part1Ptr, part2Ptr, incrPtr, TCL_LEAVE_ERR_MSG, opnd);
	    CACHE_STACK_INFO();
	    Tcl_DecrRefCount(incrPtr);
	    if (objResultPtr == NULL) {
		TRACE_APPEND(("ERROR: %.30s\n",
			O2S(Tcl_GetObjResult(interp))));
		goto gotError;
	    }
	}
    doneIncr:
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
#ifndef TCL_COMPILE_DEBUG
	if (*(pc+pcAdjustment) == INST_POP) {
	    NEXT_INST_V((pcAdjustment+1), cleanup, 0);
	}
#endif
	NEXT_INST_V(pcAdjustment, cleanup, 1);
    }

    /*
     *	   End of INST_INCR instructions.
     * -----------------------------------------------------------------
     *	   Start of INST_EXIST instructions.
     */

    case INST_EXIST_SCALAR:
	opnd = TclGetUInt4AtPtr(pc+1);
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%u => ", opnd));
	if (ReadTraced(varPtr)) {
	    DECACHE_STACK_INFO();
	    TclObjCallVarTraces(iPtr, NULL, varPtr, NULL, NULL,
		    TCL_TRACE_READS, 0, opnd);
	    CACHE_STACK_INFO();
	    if (TclIsVarUndefined(varPtr)) {
		TclCleanupVar(varPtr, NULL);
		varPtr = NULL;
	    }
	}

	/*
	 * Tricky! Arrays always exist.
	 */

	objResultPtr = TCONST(!varPtr || TclIsVarUndefined(varPtr) ? 0 : 1);
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_F(5, 0, 1);

    case INST_EXIST_ARRAY:
	opnd = TclGetUInt4AtPtr(pc+1);
	part2Ptr = OBJ_AT_TOS;
	arrayPtr = LOCAL(opnd);
	while (TclIsVarLink(arrayPtr)) {
	    arrayPtr = arrayPtr->value.linkPtr;
	}
	TRACE(("%u \"%.30s\" => ", opnd, O2S(part2Ptr)));
	if (TclIsVarArray(arrayPtr) && !ReadTraced(arrayPtr)) {
	    varPtr = VarHashFindVar(arrayPtr->value.tablePtr, part2Ptr);
	    if (!varPtr || !ReadTraced(varPtr)) {
		goto doneExistArray;
	    }
	}
	varPtr = TclLookupArrayElement(interp, NULL, part2Ptr, 0, "access",
		0, 1, arrayPtr, opnd);
	if (varPtr) {
	    if (ReadTraced(varPtr) || (arrayPtr && ReadTraced(arrayPtr))) {
		DECACHE_STACK_INFO();
		TclObjCallVarTraces(iPtr, arrayPtr, varPtr, NULL, part2Ptr,
			TCL_TRACE_READS, 0, opnd);
		CACHE_STACK_INFO();
	    }
	    if (TclIsVarUndefined(varPtr)) {
		TclCleanupVar(varPtr, arrayPtr);
		varPtr = NULL;
	    }
	}
    doneExistArray:
	objResultPtr = TCONST(!varPtr || TclIsVarUndefined(varPtr) ? 0 : 1);
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_F(5, 1, 1);

    case INST_EXIST_ARRAY_STK:
	cleanup = 2;
	part2Ptr = OBJ_AT_TOS;		/* element name */
	part1Ptr = OBJ_UNDER_TOS;	/* array name */
	TRACE(("\"%.30s(%.30s)\" => ", O2S(part1Ptr), O2S(part2Ptr)));
	goto doExistStk;

    case INST_EXIST_STK:
	cleanup = 1;
	part2Ptr = NULL;
	part1Ptr = OBJ_AT_TOS;		/* variable name */
	TRACE(("\"%.30s\" => ", O2S(part1Ptr)));

    doExistStk:
	varPtr = TclObjLookupVarEx(interp, part1Ptr, part2Ptr, 0, "access",
		/*createPart1*/0, /*createPart2*/1, &arrayPtr);
	if (varPtr) {
	    if (ReadTraced(varPtr) || (arrayPtr && ReadTraced(arrayPtr))) {
		DECACHE_STACK_INFO();
		TclObjCallVarTraces(iPtr, arrayPtr, varPtr, part1Ptr,part2Ptr,
			TCL_TRACE_READS, 0, -1);
		CACHE_STACK_INFO();
	    }
	    if (TclIsVarUndefined(varPtr)) {
		TclCleanupVar(varPtr, arrayPtr);
		varPtr = NULL;
	    }
	}
	objResultPtr = TCONST(!varPtr || TclIsVarUndefined(varPtr) ? 0 : 1);
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_V(1, cleanup, 1);

    /*
     *	   End of INST_EXIST instructions.
     * -----------------------------------------------------------------
     *	   Start of INST_UNSET instructions.
     */

    {
	int flags;

    case INST_UNSET_SCALAR:
	flags = TclGetUInt1AtPtr(pc+1) ? TCL_LEAVE_ERR_MSG : 0;
	opnd = TclGetUInt4AtPtr(pc+2);
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%s %u\n", (flags?"normal":"noerr"), opnd));
	if (TclIsVarDirectUnsettable(varPtr) && !TclIsVarInHash(varPtr)) {
	    /*
	     * No errors, no traces, no searches: just make the variable cease
	     * to exist.
	     */

	    if (!TclIsVarUndefined(varPtr)) {
		TclDecrRefCount(varPtr->value.objPtr);
	    } else if (flags & TCL_LEAVE_ERR_MSG) {
		goto slowUnsetScalar;
	    }
	    varPtr->value.objPtr = NULL;
	    NEXT_INST_F(6, 0, 0);
	}

    slowUnsetScalar:
	DECACHE_STACK_INFO();
	if (TclPtrUnsetVar(interp, varPtr, NULL, NULL, NULL, flags,
		opnd) != TCL_OK && flags) {
	    goto errorInUnset;
	}
	CACHE_STACK_INFO();
	NEXT_INST_F(6, 0, 0);

    case INST_UNSET_ARRAY:
	flags = TclGetUInt1AtPtr(pc+1) ? TCL_LEAVE_ERR_MSG : 0;
	opnd = TclGetUInt4AtPtr(pc+2);
	part2Ptr = OBJ_AT_TOS;
	arrayPtr = LOCAL(opnd);
	while (TclIsVarLink(arrayPtr)) {
	    arrayPtr = arrayPtr->value.linkPtr;
	}
	TRACE(("%s %u \"%.30s\"\n",
		(flags ? "normal" : "noerr"), opnd, O2S(part2Ptr)));
	if (TclIsVarArray(arrayPtr) && !UnsetTraced(arrayPtr)) {
	    varPtr = VarHashFindVar(arrayPtr->value.tablePtr, part2Ptr);
	    if (varPtr && TclIsVarDirectUnsettable(varPtr)) {
		/*
		 * No nasty traces and element exists, so we can proceed to
		 * unset it. Might still not exist though...
		 */

		if (!TclIsVarUndefined(varPtr)) {
		    TclDecrRefCount(varPtr->value.objPtr);
		} else if (flags & TCL_LEAVE_ERR_MSG) {
		    goto slowUnsetArray;
		}
		varPtr->value.objPtr = NULL;
		NEXT_INST_F(6, 1, 0);
	    } else if (!varPtr && !(flags & TCL_LEAVE_ERR_MSG)) {
		/*
		 * Don't need to do anything here.
		 */

		NEXT_INST_F(6, 1, 0);
	    }
	}
    slowUnsetArray:
	DECACHE_STACK_INFO();
	varPtr = TclLookupArrayElement(interp, NULL, part2Ptr, flags, "unset",
		0, 0, arrayPtr, opnd);
	if (!varPtr) {
	    if (flags & TCL_LEAVE_ERR_MSG) {
		goto errorInUnset;
	    }
	} else if (TclPtrUnsetVar(interp, varPtr, arrayPtr, NULL, part2Ptr,
		flags, opnd) != TCL_OK && (flags & TCL_LEAVE_ERR_MSG)) {
	    goto errorInUnset;
	}
	CACHE_STACK_INFO();
	NEXT_INST_F(6, 1, 0);

    case INST_UNSET_ARRAY_STK:
	flags = TclGetUInt1AtPtr(pc+1) ? TCL_LEAVE_ERR_MSG : 0;
	cleanup = 2;
	part2Ptr = OBJ_AT_TOS;		/* element name */
	part1Ptr = OBJ_UNDER_TOS;	/* array name */
	TRACE(("%s \"%.30s(%.30s)\"\n", (flags?"normal":"noerr"),
		O2S(part1Ptr), O2S(part2Ptr)));
	goto doUnsetStk;

    case INST_UNSET_STK:
	flags = TclGetUInt1AtPtr(pc+1) ? TCL_LEAVE_ERR_MSG : 0;
	cleanup = 1;
	part2Ptr = NULL;
	part1Ptr = OBJ_AT_TOS;		/* variable name */
	TRACE(("%s \"%.30s\"\n", (flags?"normal":"noerr"), O2S(part1Ptr)));

    doUnsetStk:
	DECACHE_STACK_INFO();
	if (TclObjUnsetVar2(interp, part1Ptr, part2Ptr, flags) != TCL_OK
		&& (flags & TCL_LEAVE_ERR_MSG)) {
	    goto errorInUnset;
	}
	CACHE_STACK_INFO();
	NEXT_INST_V(2, cleanup, 0);

    errorInUnset:
	CACHE_STACK_INFO();
	TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	goto gotError;

	/*
	 * This is really an unset operation these days. Do not issue.
	 */

    case INST_DICT_DONE:
	opnd = TclGetUInt4AtPtr(pc+1);
	TRACE(("%u\n", opnd));
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	if (TclIsVarDirectUnsettable(varPtr) && !TclIsVarInHash(varPtr)) {
	    if (!TclIsVarUndefined(varPtr)) {
		TclDecrRefCount(varPtr->value.objPtr);
	    }
	    varPtr->value.objPtr = NULL;
	} else {
	    DECACHE_STACK_INFO();
	    TclPtrUnsetVar(interp, varPtr, NULL, NULL, NULL, 0, opnd);
	    CACHE_STACK_INFO();
	}
	NEXT_INST_F(5, 0, 0);
    }

    /*
     *	   End of INST_UNSET instructions.
     * -----------------------------------------------------------------
     *	   Start of INST_ARRAY instructions.
     */

    case INST_ARRAY_EXISTS_IMM:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	cleanup = 0;
	part1Ptr = NULL;
	arrayPtr = NULL;
	TRACE(("%u => ", opnd));
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	goto doArrayExists;
    case INST_ARRAY_EXISTS_STK:
	opnd = -1;
	pcAdjustment = 1;
	cleanup = 1;
	part1Ptr = OBJ_AT_TOS;
	TRACE(("\"%.30s\" => ", O2S(part1Ptr)));
	varPtr = TclObjLookupVarEx(interp, part1Ptr, NULL, 0, NULL,
		/*createPart1*/0, /*createPart2*/0, &arrayPtr);
    doArrayExists:
	if (varPtr && (varPtr->flags & VAR_TRACED_ARRAY)
		&& (TclIsVarArray(varPtr) || TclIsVarUndefined(varPtr))) {
	    DECACHE_STACK_INFO();
	    result = TclObjCallVarTraces(iPtr, arrayPtr, varPtr, part1Ptr,
		    NULL, (TCL_LEAVE_ERR_MSG|TCL_NAMESPACE_ONLY|
		    TCL_GLOBAL_ONLY|TCL_TRACE_ARRAY), 1, opnd);
	    CACHE_STACK_INFO();
	    if (result == TCL_ERROR) {
		TRACE_APPEND(("ERROR: %.30s\n",
			O2S(Tcl_GetObjResult(interp))));
		goto gotError;
	    }
	}
	if (varPtr && TclIsVarArray(varPtr) && !TclIsVarUndefined(varPtr)) {
	    objResultPtr = TCONST(1);
	} else {
	    objResultPtr = TCONST(0);
	}
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_V(pcAdjustment, cleanup, 1);

    case INST_ARRAY_MAKE_IMM:
	opnd = TclGetUInt4AtPtr(pc+1);
	pcAdjustment = 5;
	cleanup = 0;
	part1Ptr = NULL;
	arrayPtr = NULL;
	TRACE(("%u => ", opnd));
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	goto doArrayMake;
    case INST_ARRAY_MAKE_STK:
	opnd = -1;
	pcAdjustment = 1;
	cleanup = 1;
	part1Ptr = OBJ_AT_TOS;
	TRACE(("\"%.30s\" => ", O2S(part1Ptr)));
	varPtr = TclObjLookupVarEx(interp, part1Ptr, NULL, TCL_LEAVE_ERR_MSG,
		"set", /*createPart1*/1, /*createPart2*/0, &arrayPtr);
	if (varPtr == NULL) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
    doArrayMake:
	if (varPtr && !TclIsVarArray(varPtr)) {
	    if (TclIsVarArrayElement(varPtr) || !TclIsVarUndefined(varPtr)) {
		/*
		 * Either an array element, or a scalar: lose!
		 */

		TclObjVarErrMsg(interp, part1Ptr, NULL, "array set",
			"variable isn't array", opnd);
		Tcl_SetErrorCode(interp, "TCL", "WRITE", "ARRAY", NULL);
		TRACE_APPEND(("ERROR: bad array ref: %.30s\n",
			O2S(Tcl_GetObjResult(interp))));
		goto gotError;
	    }
	    TclSetVarArray(varPtr);
	    varPtr->value.tablePtr = ckalloc(sizeof(TclVarHashTable));
	    TclInitVarHashTable(varPtr->value.tablePtr,
		    TclGetVarNsPtr(varPtr));
#ifdef TCL_COMPILE_DEBUG
	    TRACE_APPEND(("done\n"));
	} else {
	    TRACE_APPEND(("nothing to do\n"));
#endif
	}
	NEXT_INST_V(pcAdjustment, cleanup, 0);

    /*
     *	   End of INST_ARRAY instructions.
     * -----------------------------------------------------------------
     *	   Start of variable linking instructions.
     */

    {
	Var *otherPtr;
	CallFrame *framePtr, *savedFramePtr;
	Tcl_Namespace *nsPtr;
	Namespace *savedNsPtr;

    case INST_UPVAR:
	TRACE_WITH_OBJ(("upvar "), OBJ_UNDER_TOS);

	if (TclObjGetFrame(interp, OBJ_UNDER_TOS, &framePtr) == -1) {
	    goto gotError;
	}

	/*
	 * Locate the other variable.
	 */

	savedFramePtr = iPtr->varFramePtr;
	iPtr->varFramePtr = framePtr;
	otherPtr = TclObjLookupVarEx(interp, OBJ_AT_TOS, NULL,
		TCL_LEAVE_ERR_MSG, "access", /*createPart1*/ 1,
		/*createPart2*/ 1, &varPtr);
	iPtr->varFramePtr = savedFramePtr;
	if (!otherPtr) {
	    goto gotError;
	}
	goto doLinkVars;

    case INST_NSUPVAR:
	TRACE_WITH_OBJ(("nsupvar "), OBJ_UNDER_TOS);
	if (TclGetNamespaceFromObj(interp, OBJ_UNDER_TOS, &nsPtr) != TCL_OK) {
	    goto gotError;
	}

	/*
	 * Locate the other variable.
	 */

	savedNsPtr = iPtr->varFramePtr->nsPtr;
	iPtr->varFramePtr->nsPtr = (Namespace *) nsPtr;
	otherPtr = TclObjLookupVarEx(interp, OBJ_AT_TOS, NULL,
		(TCL_NAMESPACE_ONLY | TCL_LEAVE_ERR_MSG), "access",
		/*createPart1*/ 1, /*createPart2*/ 1, &varPtr);
	iPtr->varFramePtr->nsPtr = savedNsPtr;
	if (!otherPtr) {
	    goto gotError;
	}
	goto doLinkVars;

    case INST_VARIABLE:
	TRACE(("variable "));
	otherPtr = TclObjLookupVarEx(interp, OBJ_AT_TOS, NULL,
		(TCL_NAMESPACE_ONLY | TCL_LEAVE_ERR_MSG), "access",
		/*createPart1*/ 1, /*createPart2*/ 1, &varPtr);
	if (!otherPtr) {
	    goto gotError;
	}

	/*
	 * Do the [variable] magic.
	 */

	TclSetVarNamespaceVar(otherPtr);

    doLinkVars:

	/*
	 * If we are here, the local variable has already been created: do the
	 * little work of TclPtrMakeUpvar that remains to be done right here
	 * if there are no errors; otherwise, let it handle the case.
	 */

	opnd = TclGetInt4AtPtr(pc+1);;
	varPtr = LOCAL(opnd);
	if ((varPtr != otherPtr) && !TclIsVarTraced(varPtr)
		&& (TclIsVarUndefined(varPtr) || TclIsVarLink(varPtr))) {
	    if (!TclIsVarUndefined(varPtr)) {
		/*
		 * Then it is a defined link.
		 */

		Var *linkPtr = varPtr->value.linkPtr;

		if (linkPtr == otherPtr) {
		    NEXT_INST_F(5, 1, 0);
		}
		if (TclIsVarInHash(linkPtr)) {
		    VarHashRefCount(linkPtr)--;
		    if (TclIsVarUndefined(linkPtr)) {
			TclCleanupVar(linkPtr, NULL);
		    }
		}
	    }
	    TclSetVarLink(varPtr);
	    varPtr->value.linkPtr = otherPtr;
	    if (TclIsVarInHash(otherPtr)) {
		VarHashRefCount(otherPtr)++;
	    }
	} else if (TclPtrObjMakeUpvar(interp, otherPtr, NULL, 0,
		opnd) != TCL_OK) {
	    goto gotError;
	}

	/*
	 * Do not pop the namespace or frame index, it may be needed for other
	 * variables - and [variable] did not push it at all.
	 */

	NEXT_INST_F(5, 1, 0);
    }

    /*
     *	   End of variable linking instructions.
     * -----------------------------------------------------------------
     */

    case INST_JUMP1:
	opnd = TclGetInt1AtPtr(pc+1);
	TRACE(("%d => new pc %u\n", opnd,
		(unsigned)(pc + opnd - codePtr->codeStart)));
	NEXT_INST_F(opnd, 0, 0);

    case INST_JUMP4:
	opnd = TclGetInt4AtPtr(pc+1);
	TRACE(("%d => new pc %u\n", opnd,
		(unsigned)(pc + opnd - codePtr->codeStart)));
	NEXT_INST_F(opnd, 0, 0);

    {
	int jmpOffset[2], b;

	/* TODO: consider rewrite so we don't compute the offset we're not
	 * going to take. */
    case INST_JUMP_FALSE4:
	jmpOffset[0] = TclGetInt4AtPtr(pc+1);	/* FALSE offset */
	jmpOffset[1] = 5;			/* TRUE offset */
	goto doCondJump;

    case INST_JUMP_TRUE4:
	jmpOffset[0] = 5;
	jmpOffset[1] = TclGetInt4AtPtr(pc+1);
	goto doCondJump;

    case INST_JUMP_FALSE1:
	jmpOffset[0] = TclGetInt1AtPtr(pc+1);
	jmpOffset[1] = 2;
	goto doCondJump;

    case INST_JUMP_TRUE1:
	jmpOffset[0] = 2;
	jmpOffset[1] = TclGetInt1AtPtr(pc+1);

    doCondJump:
	valuePtr = OBJ_AT_TOS;

	/* TODO - check claim that taking address of b harms performance */
	/* TODO - consider optimization search for constants */
	if (TclGetBooleanFromObj(interp, valuePtr, &b) != TCL_OK) {
	    TRACE_WITH_OBJ(("%d => ERROR: ", jmpOffset[
		    ((*pc == INST_JUMP_FALSE1) || (*pc == INST_JUMP_FALSE4))
		    ? 0 : 1]), Tcl_GetObjResult(interp));
	    goto gotError;
	}

#ifdef TCL_COMPILE_DEBUG
	if (b) {
	    if ((*pc == INST_JUMP_TRUE1) || (*pc == INST_JUMP_TRUE4)) {
		TRACE(("%d => %.20s true, new pc %u\n", jmpOffset[1],
			O2S(valuePtr),
			(unsigned)(pc + jmpOffset[1] - codePtr->codeStart)));
	    } else {
		TRACE(("%d => %.20s true\n", jmpOffset[0], O2S(valuePtr)));
	    }
	} else {
	    if ((*pc == INST_JUMP_TRUE1) || (*pc == INST_JUMP_TRUE4)) {
		TRACE(("%d => %.20s false\n", jmpOffset[0], O2S(valuePtr)));
	    } else {
		TRACE(("%d => %.20s false, new pc %u\n", jmpOffset[0],
			O2S(valuePtr),
			(unsigned)(pc + jmpOffset[1] - codePtr->codeStart)));
	    }
	}
#endif
	NEXT_INST_F(jmpOffset[b], 1, 0);
    }

    case INST_JUMP_TABLE: {
	Tcl_HashEntry *hPtr;
	JumptableInfo *jtPtr;

	/*
	 * Jump to location looked up in a hashtable; fall through to next
	 * instr if lookup fails.
	 */

	opnd = TclGetInt4AtPtr(pc+1);
	jtPtr = (JumptableInfo *) codePtr->auxDataArrayPtr[opnd].clientData;
	TRACE(("%d => %.20s ", opnd, O2S(OBJ_AT_TOS)));
	hPtr = Tcl_FindHashEntry(&jtPtr->hashTable, TclGetString(OBJ_AT_TOS));
	if (hPtr != NULL) {
	    int jumpOffset = PTR2INT(Tcl_GetHashValue(hPtr));

	    TRACE_APPEND(("found in table, new pc %u\n",
		    (unsigned)(pc - codePtr->codeStart + jumpOffset)));
	    NEXT_INST_F(jumpOffset, 1, 0);
	} else {
	    TRACE_APPEND(("not found in table\n"));
	    NEXT_INST_F(5, 1, 0);
	}
    }

    /*
     * These two instructions are now redundant: the complete logic of the LOR
     * and LAND is now handled by the expression compiler.
     */

    case INST_LOR:
    case INST_LAND: {
	/*
	 * Operands must be boolean or numeric. No int->double conversions are
	 * performed.
	 */

	int i1, i2, iResult;

	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;
	if (TclGetBooleanFromObj(NULL, valuePtr, &i1) != TCL_OK) {
	    TRACE(("\"%.20s\" => ILLEGAL TYPE %s \n", O2S(valuePtr),
		    (valuePtr->typePtr? valuePtr->typePtr->name : "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, valuePtr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}

	if (TclGetBooleanFromObj(NULL, value2Ptr, &i2) != TCL_OK) {
	    TRACE(("\"%.20s\" => ILLEGAL TYPE %s \n", O2S(value2Ptr),
		    (value2Ptr->typePtr? value2Ptr->typePtr->name : "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, value2Ptr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}

	if (*pc == INST_LOR) {
	    iResult = (i1 || i2);
	} else {
	    iResult = (i1 && i2);
	}
	objResultPtr = TCONST(iResult);
	TRACE(("%.20s %.20s => %d\n", O2S(valuePtr),O2S(value2Ptr),iResult));
	NEXT_INST_F(1, 2, 1);
    }

    /*
     * -----------------------------------------------------------------
     *	   Start of general introspector instructions.
     */

    case INST_NS_CURRENT: {
	Namespace *currNsPtr = (Namespace *) TclGetCurrentNamespace(interp);

	if (currNsPtr == (Namespace *) TclGetGlobalNamespace(interp)) {
	    TclNewLiteralStringObj(objResultPtr, "::");
	} else {
	    TclNewStringObj(objResultPtr, currNsPtr->fullName,
		    strlen(currNsPtr->fullName));
	}
	TRACE_WITH_OBJ(("=> "), objResultPtr);
	NEXT_INST_F(1, 0, 1);
    }
    case INST_COROUTINE_NAME: {
	CoroutineData *corPtr = iPtr->execEnvPtr->corPtr;

	TclNewObj(objResultPtr);
	if (corPtr && !(corPtr->cmdPtr->flags & CMD_IS_DELETED)) {
	    Tcl_GetCommandFullName(interp, (Tcl_Command) corPtr->cmdPtr,
		    objResultPtr);
	}
	TRACE_WITH_OBJ(("=> "), objResultPtr);
	NEXT_INST_F(1, 0, 1);
    }
    case INST_INFO_LEVEL_NUM:
	TclNewIntObj(objResultPtr, iPtr->varFramePtr->level);
	TRACE_WITH_OBJ(("=> "), objResultPtr);
	NEXT_INST_F(1, 0, 1);
    case INST_INFO_LEVEL_ARGS: {
	int level;
	register CallFrame *framePtr = iPtr->varFramePtr;
	register CallFrame *rootFramePtr = iPtr->rootFramePtr;

	valuePtr = OBJ_AT_TOS;
	if (TclGetIntFromObj(interp, valuePtr, &level) != TCL_OK) {
	    TRACE_WITH_OBJ(("%.30s => ERROR: ", O2S(valuePtr)),
		    Tcl_GetObjResult(interp));
	    goto gotError;
	}
	TRACE(("%d => ", level));
	if (level <= 0) {
	    level += framePtr->level;
	}
	for (; (framePtr->level!=level) && (framePtr!=rootFramePtr) ;
		framePtr = framePtr->callerVarPtr) {
	    /* Empty loop body */
	}
	if (framePtr == rootFramePtr) {
	    Tcl_AppendResult(interp, "bad level \"", TclGetString(valuePtr),
		    "\"", NULL);
	    TRACE_APPEND(("ERROR: bad level\n"));
	    Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "STACK_LEVEL",
		    TclGetString(valuePtr), NULL);
	    goto gotError;
	}
	objResultPtr = Tcl_NewListObj(framePtr->objc, framePtr->objv);
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_F(1, 1, 1);
    }
    case INST_RESOLVE_COMMAND: {
	Tcl_Command cmd = Tcl_GetCommandFromObj(interp, OBJ_AT_TOS);

	TclNewObj(objResultPtr);
	if (cmd != NULL) {
	    Tcl_GetCommandFullName(interp, cmd, objResultPtr);
	}
	TRACE_WITH_OBJ(("\"%.20s\" => ", O2S(OBJ_AT_TOS)), objResultPtr);
	NEXT_INST_F(1, 1, 1);
    }
    case INST_TCLOO_SELF: {
	CallFrame *framePtr = iPtr->varFramePtr;
	CallContext *contextPtr;

	if (framePtr == NULL ||
		!(framePtr->isProcCallFrame & FRAME_IS_METHOD)) {
	    TRACE(("=> ERROR: no TclOO call context\n"));
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "self may only be called from inside a method",
		    TCL_STRLEN));
	    Tcl_SetErrorCode(interp, "TCL", "OO", "CONTEXT_REQUIRED", NULL);
	    goto gotError;
	}
	contextPtr = framePtr->clientData;

	/*
	 * Call out to get the name; it's expensive to compute but cached.
	 */

	objResultPtr = TclOOObjectName(interp, contextPtr->oPtr);
	TRACE_WITH_OBJ(("=> "), objResultPtr);
	NEXT_INST_F(1, 0, 1);
    }
    {
	Object *oPtr;

    case INST_TCLOO_IS_OBJECT:
	oPtr = (Object *) Tcl_GetObjectFromObj(interp, OBJ_AT_TOS);
	objResultPtr = TCONST(oPtr != NULL ? 1 : 0);
	TRACE_WITH_OBJ(("%.30s => ", O2S(OBJ_AT_TOS)), objResultPtr);
	NEXT_INST_F(1, 1, 1);
    case INST_TCLOO_CLASS:
	oPtr = (Object *) Tcl_GetObjectFromObj(interp, OBJ_AT_TOS);
	if (oPtr == NULL) {
	    TRACE(("%.30s => ERROR: not object\n", O2S(OBJ_AT_TOS)));
	    goto gotError;
	}
	objResultPtr = TclOOObjectName(interp, oPtr->selfCls->thisPtr);
	TRACE_WITH_OBJ(("%.30s => ", O2S(OBJ_AT_TOS)), objResultPtr);
	NEXT_INST_F(1, 1, 1);
    case INST_TCLOO_NS:
	oPtr = (Object *) Tcl_GetObjectFromObj(interp, OBJ_AT_TOS);
	if (oPtr == NULL) {
	    TRACE(("%.30s => ERROR: not object\n", O2S(OBJ_AT_TOS)));
	    goto gotError;
	}

	/*
	 * TclOO objects *never* have the global namespace as their NS.
	 */

	TclNewStringObj(objResultPtr, oPtr->namespacePtr->fullName,
		strlen(oPtr->namespacePtr->fullName));
	TRACE_WITH_OBJ(("%.30s => ", O2S(OBJ_AT_TOS)), objResultPtr);
	NEXT_INST_F(1, 1, 1);
    }

    /*
     * -----------------------------------------------------------------
     *	   Start of INST_LIST and related instructions.
     */

    {
	ssize_t index, fromIdx, toIdx;
	int nocase, match, cflags;
	size_t numIndices, length2, s1len, s2len;
	const char *s1, *s2;

    case INST_LIST:
	/*
	 * Pop the opnd (objc) top stack elements into a new list obj and then
	 * decrement their ref counts.
	 */

	opnd = TclGetUInt4AtPtr(pc+1);
	objResultPtr = Tcl_NewListObj(opnd, &OBJ_AT_DEPTH(opnd-1));
	TRACE_WITH_OBJ(("%u => ", opnd), objResultPtr);
	NEXT_INST_V(5, opnd, 1);

    case INST_LIST_LENGTH:
	valuePtr = OBJ_AT_TOS;
	if (TclListObjLength(interp, valuePtr, &length) != TCL_OK) {
	    TRACE_WITH_OBJ(("%.30s => ERROR: ", O2S(valuePtr)),
		    Tcl_GetObjResult(interp));
	    goto gotError;
	}
	TclNewIntObj(objResultPtr, length);
	TRACE(("%.20s => %d\n", O2S(valuePtr), length));
	NEXT_INST_F(1, 1, 1);

    case INST_LIST_INDEX:	/* lindex with objc == 3 */
	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;

	/*
	 * Extract the desired list element.
	 */

	if ((TclListObjGetElements(interp, valuePtr, &objc, &objv) == TCL_OK)
		&& (value2Ptr->typePtr != &tclListType)
		&& (TclGetIntForIndexM(NULL , value2Ptr, objc-1,
			&index) == TCL_OK)) {
	    TclDecrRefCount(value2Ptr);
	    tosPtr--;
	    pcAdjustment = 1;
	    goto lindexFastPath;
	}

	objResultPtr = TclLindexList(interp, valuePtr, value2Ptr);
	if (!objResultPtr) {
	    TRACE_WITH_OBJ(("%.30s %.30s => ERROR: ", O2S(valuePtr),
		    O2S(value2Ptr)), Tcl_GetObjResult(interp));
	    goto gotError;
	}

	/*
	 * Stash the list element on the stack.
	 */

	TRACE(("%.20s %.20s => %s\n",
		O2S(valuePtr), O2S(value2Ptr), O2S(objResultPtr)));
	NEXT_INST_F(1, 2, -1);	/* Already has the correct refCount */

    case INST_LIST_INDEX_IMM:	/* lindex with objc==3 and index in bytecode
				 * stream */

	/*
	 * Pop the list and get the index.
	 */

	valuePtr = OBJ_AT_TOS;
	opnd = TclGetInt4AtPtr(pc+1);

	/*
	 * Get the contents of the list, making sure that it really is a list
	 * in the process.
	 */

	if (TclListObjGetElements(interp, valuePtr, &objc, &objv) != TCL_OK) {
	    TRACE_WITH_OBJ(("\"%.30s\" %d => ERROR: ", O2S(valuePtr), opnd),
		    Tcl_GetObjResult(interp));
	    goto gotError;
	}

	/*
	 * Select the list item based on the index. Negative operand means
	 * end-based indexing.
	 */

	if (opnd < -1) {
	    index = opnd+1 + objc;
	} else {
	    index = opnd;
	}
	pcAdjustment = 5;

    lindexFastPath:
	if (index >= 0 && index < objc) {
	    objResultPtr = objv[index];
	} else {
	    TclNewObj(objResultPtr);
	}

	TRACE_WITH_OBJ(("\"%.30s\" %d => ", O2S(valuePtr), opnd),
		objResultPtr);
	NEXT_INST_F(pcAdjustment, 1, 1);

    case INST_LIST_INDEX_MULTI:	/* 'lindex' with multiple index args */
	/*
	 * Determine the count of index args.
	 */

	opnd = TclGetUInt4AtPtr(pc+1);
	numIndices = opnd-1;

	/*
	 * Do the 'lindex' operation.
	 */

	objResultPtr = TclLindexFlat(interp, OBJ_AT_DEPTH(numIndices),
		numIndices, &OBJ_AT_DEPTH(numIndices - 1));
	if (!objResultPtr) {
	    TRACE_WITH_OBJ(("%d => ERROR: ", opnd), Tcl_GetObjResult(interp));
	    goto gotError;
	}

	/*
	 * Set result.
	 */

	TRACE(("%d => %s\n", opnd, O2S(objResultPtr)));
	NEXT_INST_V(5, opnd, -1);

    case INST_LSET_FLAT:
	/*
	 * Lset with 3, 5, or more args. Get the number of index args.
	 */

	opnd = TclGetUInt4AtPtr(pc + 1);
	numIndices = opnd - 2;

	/*
	 * Get the old value of variable, and remove the stack ref. This is
	 * safe because the variable still references the object; the ref
	 * count will never go zero here - we can use the smaller macro
	 * Tcl_DecrRefCount.
	 */

	valuePtr = POP_OBJECT();
	Tcl_DecrRefCount(valuePtr); /* This one should be done here */

	/*
	 * Compute the new variable value.
	 */

	objResultPtr = TclLsetFlat(interp, valuePtr, numIndices,
		&OBJ_AT_DEPTH(numIndices), OBJ_AT_TOS);
	if (!objResultPtr) {
	    TRACE_WITH_OBJ(("%d => ERROR: ", opnd), Tcl_GetObjResult(interp));
	    goto gotError;
	}

	/*
	 * Set result.
	 */

	TRACE(("%d => %s\n", opnd, O2S(objResultPtr)));
	NEXT_INST_V(5, numIndices+1, -1);

    case INST_LSET_LIST:	/* 'lset' with 4 args */
	/*
	 * Get the old value of variable, and remove the stack ref. This is
	 * safe because the variable still references the object; the ref
	 * count will never go zero here - we can use the smaller macro
	 * Tcl_DecrRefCount.
	 */

	objPtr = POP_OBJECT();
	Tcl_DecrRefCount(objPtr);	/* This one should be done here. */

	/*
	 * Get the new element value, and the index list.
	 */

	valuePtr = OBJ_AT_TOS;
	value2Ptr = OBJ_UNDER_TOS;

	/*
	 * Compute the new variable value.
	 */

	objResultPtr = TclLsetList(interp, objPtr, value2Ptr, valuePtr);
	if (!objResultPtr) {
	    TRACE_WITH_OBJ(("\"%.30s\" => ERROR: ", O2S(value2Ptr)),
		    Tcl_GetObjResult(interp));
	    goto gotError;
	}

	/*
	 * Set result.
	 */

	TRACE(("=> %s\n", O2S(objResultPtr)));
	NEXT_INST_F(1, 2, -1);

    case INST_LIST_RANGE_IMM:	/* lrange with objc==4 and both indices in
				 * bytecode stream */

	/*
	 * Pop the list and get the indices.
	 */

	valuePtr = OBJ_AT_TOS;
	fromIdx = TclGetInt4AtPtr(pc+1);
	toIdx = TclGetInt4AtPtr(pc+5);

	/*
	 * Get the contents of the list, making sure that it really is a list
	 * in the process.
	 */

	if (TclListObjGetElements(interp, valuePtr, &objc, &objv) != TCL_OK) {
	    TRACE_WITH_OBJ(("\"%.30s\" %d %d => ERROR: ", O2S(valuePtr),
		    fromIdx, toIdx), Tcl_GetObjResult(interp));
	    goto gotError;
	}

	/*
	 * Skip a lot of work if we're about to throw the result away (common
	 * with uses of [lassign]).
	 */

#ifndef TCL_COMPILE_DEBUG
	if (*(pc+9) == INST_POP) {
	    NEXT_INST_F(10, 1, 0);
	}
#endif

	/*
	 * Adjust the indices for end-based handling.
	 */

	if (fromIdx < -1) {
	    fromIdx += 1+objc;
	    if (fromIdx < -1) {
		fromIdx = -1;
	    }
	} else if (fromIdx > objc) {
	    fromIdx = objc;
	}
	if (toIdx < -1) {
	    toIdx += 1 + objc;
	    if (toIdx < -1) {
		toIdx = -1;
	    }
	} else if (toIdx > objc) {
	    toIdx = objc;
	}

	/*
	 * Check if we are referring to a valid, non-empty list range, and if
	 * so, build the list of elements in that range.
	 */

	if (fromIdx<=toIdx && fromIdx<objc && toIdx>=0) {
	    if (fromIdx < 0) {
		fromIdx = 0;
	    }
	    if (toIdx >= objc) {
		toIdx = objc-1;
	    }
	    if (fromIdx == 0 && toIdx != objc-1 && !Tcl_IsShared(valuePtr)) {
		/*
		 * BEWARE! This is looking inside the implementation of the
		 * list type.
		 */

		List *listPtr = valuePtr->internalRep.twoPtrValue.ptr1;

		if (listPtr->refCount == 1) {
		    TRACE(("\"%.30s\" %d %d => ", O2S(valuePtr),
			    TclGetInt4AtPtr(pc+1), TclGetInt4AtPtr(pc+5)));
		    for (index=toIdx+1 ; index<objc-1 ; index++) {
			TclDecrRefCount(objv[index]);
		    }
		    listPtr->elemCount = toIdx+1;
		    listPtr->canonicalFlag = 1;
		    TclInvalidateStringRep(valuePtr);
		    TRACE_APPEND(("%.30s\n", O2S(valuePtr)));
		    NEXT_INST_F(9, 0, 0);
		}
	    }
	    objResultPtr = Tcl_NewListObj(toIdx-fromIdx+1, objv+fromIdx);
	} else {
	    TclNewObj(objResultPtr);
	}

	TRACE_WITH_OBJ(("\"%.30s\" %d %d => ", O2S(valuePtr),
		TclGetInt4AtPtr(pc+1), TclGetInt4AtPtr(pc+5)), objResultPtr);
	NEXT_INST_F(9, 1, 1);

    case INST_LIST_IN:
    case INST_LIST_NOT_IN:	/* Basic list containment operators. */
	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;

	s1 = TclGetStringFromObj(valuePtr, &s1len);
	if (TclListObjLength(interp, value2Ptr, &length) != TCL_OK) {
	    TRACE_WITH_OBJ(("\"%.30s\" \"%.30s\" => ERROR: ", O2S(valuePtr),
		    O2S(value2Ptr)), Tcl_GetObjResult(interp));
	    goto gotError;
	}
	match = 0;
	if (length > 0) {
	    int i = 0;
	    Tcl_Obj *o;

	    /*
	     * An empty list doesn't match anything.
	     */

	    do {
		Tcl_ListObjIndex(NULL, value2Ptr, i, &o);
		if (o != NULL) {
		    s2 = TclGetStringFromObj(o, &s2len);
		} else {
		    s2 = "";
		    s2len = 0;
		}
		if (s1len == s2len) {
		    match = (memcmp(s1, s2, s1len) == 0);
		}
		i++;
	    } while (i < length && match == 0);
	}

	if (*pc == INST_LIST_NOT_IN) {
	    match = !match;
	}

	TRACE(("%.20s %.20s => %d\n", O2S(valuePtr), O2S(value2Ptr), match));

	/*
	 * Peep-hole optimisation: if you're about to jump, do jump from here.
	 * We're saving the effort of pushing a boolean value only to pop it
	 * for branching.
	 */

	pc++;
#ifndef TCL_COMPILE_DEBUG
	switch (*pc) {
	case INST_JUMP_FALSE1:
	    NEXT_INST_F((match ? 2 : TclGetInt1AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE1:
	    NEXT_INST_F((match ? TclGetInt1AtPtr(pc+1) : 2), 2, 0);
	case INST_JUMP_FALSE4:
	    NEXT_INST_F((match ? 5 : TclGetInt4AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE4:
	    NEXT_INST_F((match ? TclGetInt4AtPtr(pc+1) : 5), 2, 0);
	}
#endif
	objResultPtr = TCONST(match);
	NEXT_INST_F(0, 2, 1);

    /*
     *	   End of INST_LIST and related instructions.
     * -----------------------------------------------------------------
     *	   Start of string-related instructions.
     */

    case INST_STR_EQ:
    case INST_STR_NEQ:		/* String (in)equality check */
    case INST_STR_CMP:		/* String compare. */
    stringCompare:
	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;

	if (valuePtr == value2Ptr) {
	    match = 0;
	} else {
	    /*
	     * We only need to check (in)equality when we have equal length
	     * strings.  We can use memcmp in all (n)eq cases because we
	     * don't need to worry about lexical LE/BE variance.
	     */

	    typedef int (*memCmpFn_t)(const void*, const void*, size_t);
	    memCmpFn_t memCmpFn;
	    int checkEq = ((*pc == INST_EQ) || (*pc == INST_NEQ)
		    || (*pc == INST_STR_EQ) || (*pc == INST_STR_NEQ));

	    if (TclIsPureByteArray(valuePtr)
		    && TclIsPureByteArray(value2Ptr)) {
		s1 = (char *) Tcl_GetByteArrayFromObj(valuePtr, &s1len);
		s2 = (char *) Tcl_GetByteArrayFromObj(value2Ptr, &s2len);
		memCmpFn = memcmp;
	    } else if (((valuePtr->typePtr == &tclStringType)
		    && (value2Ptr->typePtr == &tclStringType))) {
		/*
		 * Do a unicode-specific comparison if both of the args are of
		 * String type. If the char length == byte length, we can do a
		 * memcmp. In benchmark testing this proved the most efficient
		 * check between the unicode and string comparison operations.
		 */

		s1len = Tcl_GetCharLength(valuePtr);
		s2len = Tcl_GetCharLength(value2Ptr);
		if ((s1len == valuePtr->length)
			&& (s2len == value2Ptr->length)) {
		    s1 = valuePtr->bytes;
		    s2 = value2Ptr->bytes;
		    memCmpFn = memcmp;
		} else {
		    s1 = (char *) Tcl_GetUnicode(valuePtr);
		    s2 = (char *) Tcl_GetUnicode(value2Ptr);
		    if (
#ifdef WORDS_BIGENDIAN
			1
#else
			checkEq
#endif
			) {
			memCmpFn = memcmp;
			s1len *= sizeof(Tcl_UniChar);
			s2len *= sizeof(Tcl_UniChar);
		    } else {
			memCmpFn = (memCmpFn_t) Tcl_UniCharNcmp;
		    }
		}
	    } else {
		/*
		 * strcmp can't do a simple memcmp in order to handle the
		 * special Tcl \xC0\x80 null encoding for utf-8.
		 */

		s1 = TclGetStringFromObj(valuePtr, &s1len);
		s2 = TclGetStringFromObj(value2Ptr, &s2len);
		if (checkEq) {
		    memCmpFn = memcmp;
		} else {
		    memCmpFn = (memCmpFn_t) TclpUtfNcmp2;
		}
	    }

	    if (checkEq && (s1len != s2len)) {
		match = 1;
	    } else {
		/*
		 * The comparison function should compare up to the minimum
		 * byte length only.
		 */
		match = memCmpFn(s1, s2,
			(size_t) ((s1len < s2len) ? s1len : s2len));
		if (match == 0) {
		    match = s1len - s2len;
		}
	    }
	}

	/*
	 * Make sure only -1,0,1 is returned
	 * TODO: consider peephole opt.
	 */

	if (*pc != INST_STR_CMP) {
	    /*
	     * Take care of the opcodes that goto'ed into here.
	     */

	    switch (*pc) {
	    case INST_STR_EQ:
	    case INST_EQ:
		match = (match == 0);
		break;
	    case INST_STR_NEQ:
	    case INST_NEQ:
		match = (match != 0);
		break;
	    case INST_LT:
		match = (match < 0);
		break;
	    case INST_GT:
		match = (match > 0);
		break;
	    case INST_LE:
		match = (match <= 0);
		break;
	    case INST_GE:
		match = (match >= 0);
		break;
	    }
	}
	if (match < 0) {
	    TclNewIntObj(objResultPtr, -1);
	} else {
	    objResultPtr = TCONST(match > 0);
	}
	TRACE(("%.20s %.20s => %s\n", O2S(valuePtr), O2S(value2Ptr),
		O2S(objResultPtr)));
	NEXT_INST_F(1, 2, 1);

    case INST_STR_LEN:
	valuePtr = OBJ_AT_TOS;
	length = Tcl_GetCharLength(valuePtr);
	TclNewIntObj(objResultPtr, length);
	TRACE(("%.20s => %d\n", O2S(valuePtr), length));
	NEXT_INST_F(1, 1, 1);

    case INST_STR_INDEX:
	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;

	/*
	 * Get char length to calulate what 'end' means.
	 */

	length = Tcl_GetCharLength(valuePtr);
	if (TclGetIntForIndexM(interp, value2Ptr, length-1, &index)!=TCL_OK) {
	    goto gotError;
	}

	if ((index < 0) || (index >= length)) {
	    TclNewObj(objResultPtr);
	} else if (TclIsPureByteArray(valuePtr)) {
	    objResultPtr = Tcl_NewByteArrayObj(
		    Tcl_GetByteArrayFromObj(valuePtr, &length)+index, 1);
	} else if (valuePtr->bytes && length == valuePtr->length) {
	    objResultPtr = Tcl_NewStringObj((const char *)
		    valuePtr->bytes+index, 1);
	} else {
	    char buf[TCL_UTF_MAX];
	    Tcl_UniChar ch = Tcl_GetUniChar(valuePtr, index);

	    /*
	     * This could be: Tcl_NewUnicodeObj((const Tcl_UniChar *)&ch, 1)
	     * but creating the object as a string seems to be faster in
	     * practical use.
	     */

	    length = Tcl_UniCharToUtf(ch, buf);
	    objResultPtr = Tcl_NewStringObj(buf, length);
	}

	TRACE(("%.20s %.20s => %s\n", O2S(valuePtr), O2S(value2Ptr),
		O2S(objResultPtr)));
	NEXT_INST_F(1, 2, 1);

    case INST_STR_RANGE:
	TRACE(("\"%.20s\" %s %s =>",
		O2S(OBJ_AT_DEPTH(2)), O2S(OBJ_UNDER_TOS), O2S(OBJ_AT_TOS)));
	length = Tcl_GetCharLength(OBJ_AT_DEPTH(2)) - 1;
	if (TclGetIntForIndexM(interp, OBJ_UNDER_TOS, length,
		    &fromIdx) != TCL_OK
	    || TclGetIntForIndexM(interp, OBJ_AT_TOS, length,
		    &toIdx) != TCL_OK) {
	    goto gotError;
	}

	if (fromIdx < 0) {
	    fromIdx = 0;
	}
	if (toIdx >= length && toIdx > 0) {
	    toIdx = length;
	}
	if (toIdx >= fromIdx) {
	    objResultPtr = Tcl_GetRange(OBJ_AT_DEPTH(2), fromIdx, toIdx);
	} else {
	    TclNewObj(objResultPtr);
	}
	TRACE_APPEND(("\"%.30s\"\n", O2S(objResultPtr)));
	NEXT_INST_V(1, 3, 1);

    case INST_STR_RANGE_IMM:
	valuePtr = OBJ_AT_TOS;
	fromIdx = TclGetInt4AtPtr(pc+1);
	toIdx = TclGetInt4AtPtr(pc+5);
	length = Tcl_GetCharLength(valuePtr);
	TRACE(("\"%.20s\" %d %d => ", O2S(valuePtr), fromIdx, toIdx));

	/*
	 * Adjust indices for end-based indexing.
	 */

	if (fromIdx < -1) {
	    fromIdx += 1 + length;
	    if (fromIdx < 0) {
		fromIdx = 0;
	    }
	} else if (fromIdx >= length) {
	    fromIdx = length;
	}
	if (toIdx < -1) {
	    toIdx += 1 + length;
	} else if (toIdx >= length) {
	    toIdx = length - 1;
	}

	/*
	 * Check if we can do a sane substring.
	 */

	if (fromIdx <= toIdx) {
	    objResultPtr = Tcl_GetRange(valuePtr, fromIdx, toIdx);
	} else {
	    TclNewObj(objResultPtr);
	}
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_F(9, 1, 1);

    {
	Tcl_UniChar *ustring1, *ustring2, *ustring3, *end, *p;
	size_t length3;
	Tcl_Obj *value3Ptr;

    case INST_STR_MAP:
	valuePtr = OBJ_AT_TOS;		/* "Main" string. */
	value3Ptr = OBJ_UNDER_TOS;	/* "Target" string. */
	value2Ptr = OBJ_AT_DEPTH(2);	/* "Source" string. */
	if (value3Ptr == value2Ptr) {
	    objResultPtr = valuePtr;
	    NEXT_INST_V(1, 3, 1);
	} else if (valuePtr == value2Ptr) {
	    objResultPtr = value3Ptr;
	    NEXT_INST_V(1, 3, 1);
	}
	ustring1 = Tcl_GetUnicodeFromObj(valuePtr, &length);
	if (length == 0) {
	    objResultPtr = valuePtr;
	    NEXT_INST_V(1, 3, 1);
	}
	ustring2 = Tcl_GetUnicodeFromObj(value2Ptr, &length2);
	if (length2 > length || length2 == 0) {
	    objResultPtr = valuePtr;
	    NEXT_INST_V(1, 3, 1);
	} else if (length2 == length) {
	    if (memcmp(ustring1, ustring2, sizeof(Tcl_UniChar) * length)) {
		objResultPtr = valuePtr;
	    } else {
		objResultPtr = value3Ptr;
	    }
	    NEXT_INST_V(1, 3, 1);
	}
	ustring3 = Tcl_GetUnicodeFromObj(value3Ptr, &length3);

	objResultPtr = Tcl_NewUnicodeObj(ustring1, 0);
	p = ustring1;
	end = ustring1 + length;
	for (; ustring1 < end; ustring1++) {
	    if ((*ustring1 == *ustring2) && (length2==1 ||
		    memcmp(ustring1, ustring2, sizeof(Tcl_UniChar) * length2)
			    == 0)) {
		if (p != ustring1) {
		    Tcl_AppendUnicodeToObj(objResultPtr, p, ustring1-p);
		    p = ustring1 + length2;
		} else {
		    p += length2;
		}
		ustring1 = p - 1;

		Tcl_AppendUnicodeToObj(objResultPtr, ustring3, length3);
	    }
	}
	if (p != ustring1) {
	    /*
	     * Put the rest of the unmapped chars onto result.
	     */

	    Tcl_AppendUnicodeToObj(objResultPtr, p, ustring1 - p);
	}
	TRACE_WITH_OBJ(("%.20s %.20s %.20s => ",
		O2S(value2Ptr), O2S(value3Ptr), O2S(valuePtr)), objResultPtr);
	NEXT_INST_V(1, 3, 1);

    case INST_STR_FIND:
	ustring1 = Tcl_GetUnicodeFromObj(OBJ_AT_TOS, &length);	/* Haystack */
	ustring2 = Tcl_GetUnicodeFromObj(OBJ_UNDER_TOS, &length2);/* Needle */

	match = -1;
	if (length2 > 0 && length2 <= length) {
	    end = ustring1 + length - length2 + 1;
	    for (p=ustring1 ; p<end ; p++) {
		if ((*p == *ustring2) &&
			memcmp(ustring2,p,sizeof(Tcl_UniChar)*length2) == 0) {
		    match = p - ustring1;
		    break;
		}
	    }
	}

	TRACE(("%.20s %.20s => %d\n",
		O2S(OBJ_UNDER_TOS), O2S(OBJ_AT_TOS), match));

	TclNewIntObj(objResultPtr, match);
	NEXT_INST_F(1, 2, 1);

    case INST_STR_FIND_LAST:
	ustring1 = Tcl_GetUnicodeFromObj(OBJ_AT_TOS, &length);	/* Haystack */
	ustring2 = Tcl_GetUnicodeFromObj(OBJ_UNDER_TOS, &length2);/* Needle */

	match = -1;
	if (length2 > 0 && length2 <= length) {
	    for (p=ustring1+length-length2 ; p>=ustring1 ; p--) {
		if ((*p == *ustring2) &&
			memcmp(ustring2,p,sizeof(Tcl_UniChar)*length2) == 0) {
		    match = p - ustring1;
		    break;
		}
	    }
	}

	TRACE(("%.20s %.20s => %d\n",
		O2S(OBJ_UNDER_TOS), O2S(OBJ_AT_TOS), match));

	TclNewIntObj(objResultPtr, match);
	NEXT_INST_F(1, 2, 1);
    }

    case INST_STR_MATCH:
	nocase = TclGetInt1AtPtr(pc+1);
	valuePtr = OBJ_AT_TOS;		/* String */
	value2Ptr = OBJ_UNDER_TOS;	/* Pattern */

	/*
	 * Check that at least one of the objects is Unicode before promoting
	 * both.
	 */

	if ((valuePtr->typePtr == &tclStringType)
		|| (value2Ptr->typePtr == &tclStringType)) {
	    Tcl_UniChar *ustring1, *ustring2;

	    ustring1 = Tcl_GetUnicodeFromObj(valuePtr, &length);
	    ustring2 = Tcl_GetUnicodeFromObj(value2Ptr, &length2);
	    match = TclUniCharMatch(ustring1, length, ustring2, length2,
		    nocase);
	} else if (TclIsPureByteArray(valuePtr) && !nocase) {
	    unsigned char *bytes1, *bytes2;

	    bytes1 = Tcl_GetByteArrayFromObj(valuePtr, &length);
	    bytes2 = Tcl_GetByteArrayFromObj(value2Ptr, &length2);
	    match = TclByteArrayMatch(bytes1, length, bytes2, length2, 0);
	} else {
	    match = Tcl_StringCaseMatch(TclGetString(valuePtr),
		    TclGetString(value2Ptr), nocase);
	}

	/*
	 * Reuse value2Ptr object already on stack if possible. Adjustment is
	 * 2 due to the nocase byte
	 */

	TRACE(("%.20s %.20s => %d\n", O2S(valuePtr), O2S(value2Ptr), match));

	/*
	 * Peep-hole optimisation: if you're about to jump, do jump from here.
	 */

	pc += 2;
#ifndef TCL_COMPILE_DEBUG
	switch (*pc) {
	case INST_JUMP_FALSE1:
	    NEXT_INST_F((match? 2 : TclGetInt1AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE1:
	    NEXT_INST_F((match? TclGetInt1AtPtr(pc+1) : 2), 2, 0);
	case INST_JUMP_FALSE4:
	    NEXT_INST_F((match? 5 : TclGetInt4AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE4:
	    NEXT_INST_F((match? TclGetInt4AtPtr(pc+1) : 5), 2, 0);
	}
#endif
	objResultPtr = TCONST(match);
	NEXT_INST_F(0, 2, 1);

    case INST_REGEXP:
	cflags = TclGetInt1AtPtr(pc+1); /* RE compile flages like NOCASE */
	valuePtr = OBJ_AT_TOS;		/* String */
	value2Ptr = OBJ_UNDER_TOS;	/* Pattern */

	/*
	 * Compile and match the regular expression.
	 */

	{
	    Tcl_RegExp regExpr =
		    Tcl_GetRegExpFromObj(interp, value2Ptr, cflags);

	    if (regExpr == NULL) {
		goto regexpFailure;
	    }

	    match = Tcl_RegExpExecObj(interp, regExpr, valuePtr, 0, 0, 0);

	    if (match < 0) {
	    regexpFailure:
#ifdef TCL_COMPILE_DEBUG
		objResultPtr = Tcl_GetObjResult(interp);
		TRACE_WITH_OBJ(("%.20s %.20s => ERROR: ",
			O2S(valuePtr), O2S(value2Ptr)), objResultPtr);
#endif
		goto gotError;
	    }
	}

	TRACE(("%.20s %.20s => %d\n", O2S(valuePtr), O2S(value2Ptr), match));

	/*
	 * Peep-hole optimisation: if you're about to jump, do jump from here.
	 * Adjustment is 2 due to the nocase byte.
	 */

	pc += 2;
#ifndef TCL_COMPILE_DEBUG
	switch (*pc) {
	case INST_JUMP_FALSE1:
	    NEXT_INST_F((match? 2 : TclGetInt1AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE1:
	    NEXT_INST_F((match? TclGetInt1AtPtr(pc+1) : 2), 2, 0);
	case INST_JUMP_FALSE4:
	    NEXT_INST_F((match? 5 : TclGetInt4AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE4:
	    NEXT_INST_F((match? TclGetInt4AtPtr(pc+1) : 5), 2, 0);
	}
#endif
	objResultPtr = TCONST(match);
	NEXT_INST_F(0, 2, 1);
    }

    /*
     *	   End of string-related instructions.
     * -----------------------------------------------------------------
     *	   Start of numeric operator instructions.
     */

    {
	ClientData ptr1, ptr2;
	int type1, type2;
	long l1, l2, lResult;

    case INST_EQ:
    case INST_NEQ:
    case INST_LT:
    case INST_GT:
    case INST_LE:
    case INST_GE: {
	int iResult = 0, compare = 0;

	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;

	if (GetNumberFromObj(NULL, valuePtr, &ptr1, &type1) != TCL_OK) {
	    /*
	     * At least one non-numeric argument - compare as strings.
	     */

	    goto stringCompare;
	}
	if (type1 == TCL_NUMBER_NAN) {
	    /*
	     * NaN first arg: NaN != to everything, other compares are false.
	     */

	    iResult = (*pc == INST_NEQ);
	    goto foundResult;
	}
	if (valuePtr == value2Ptr) {
	    compare = MP_EQ;
	    goto convertComparison;
	}
	if (GetNumberFromObj(NULL, value2Ptr, &ptr2, &type2) != TCL_OK) {
	    /*
	     * At least one non-numeric argument - compare as strings.
	     */

	    goto stringCompare;
	}
	if (type2 == TCL_NUMBER_NAN) {
	    /*
	     * NaN 2nd arg: NaN != to everything, other compares are false.
	     */

	    iResult = (*pc == INST_NEQ);
	    goto foundResult;
	}
	if ((type1 == TCL_NUMBER_LONG) && (type2 == TCL_NUMBER_LONG)) {
	    l1 = *((const long *)ptr1);
	    l2 = *((const long *)ptr2);
	    compare = (l1 < l2) ? MP_LT : ((l1 > l2) ? MP_GT : MP_EQ);
	} else {
	    compare = TclCompareTwoNumbers(valuePtr, value2Ptr);
	}

	/*
	 * Turn comparison outcome into appropriate result for opcode.
	 */

    convertComparison:
	switch (*pc) {
	case INST_EQ:
	    iResult = (compare == MP_EQ);
	    break;
	case INST_NEQ:
	    iResult = (compare != MP_EQ);
	    break;
	case INST_LT:
	    iResult = (compare == MP_LT);
	    break;
	case INST_GT:
	    iResult = (compare == MP_GT);
	    break;
	case INST_LE:
	    iResult = (compare != MP_GT);
	    break;
	case INST_GE:
	    iResult = (compare != MP_LT);
	    break;
	}

	/*
	 * Peep-hole optimisation: if you're about to jump, do jump from here.
	 */

    foundResult:
	pc++;
#ifndef TCL_COMPILE_DEBUG
	switch (*pc) {
	case INST_JUMP_FALSE1:
	    NEXT_INST_F((iResult? 2 : TclGetInt1AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE1:
	    NEXT_INST_F((iResult? TclGetInt1AtPtr(pc+1) : 2), 2, 0);
	case INST_JUMP_FALSE4:
	    NEXT_INST_F((iResult? 5 : TclGetInt4AtPtr(pc+1)), 2, 0);
	case INST_JUMP_TRUE4:
	    NEXT_INST_F((iResult? TclGetInt4AtPtr(pc+1) : 5), 2, 0);
	}
#endif
	objResultPtr = TCONST(iResult);
	NEXT_INST_F(0, 2, 1);
    }

    case INST_MOD:
    case INST_LSHIFT:
    case INST_RSHIFT:
    case INST_BITOR:
    case INST_BITXOR:
    case INST_BITAND:
	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;

	if ((GetNumberFromObj(NULL, valuePtr, &ptr1, &type1) != TCL_OK)
		|| (type1==TCL_NUMBER_DOUBLE) || (type1==TCL_NUMBER_NAN)) {
	    TRACE(("%.20s %.20s => ILLEGAL 1st TYPE %s\n", O2S(valuePtr),
		    O2S(value2Ptr), (valuePtr->typePtr?
		    valuePtr->typePtr->name : "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, valuePtr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}

	if ((GetNumberFromObj(NULL, value2Ptr, &ptr2, &type2) != TCL_OK)
		|| (type2==TCL_NUMBER_DOUBLE) || (type2==TCL_NUMBER_NAN)) {
	    TRACE(("%.20s %.20s => ILLEGAL 2nd TYPE %s\n", O2S(valuePtr),
		    O2S(value2Ptr), (value2Ptr->typePtr?
		    value2Ptr->typePtr->name : "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, value2Ptr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}

	/*
	 * Check for common, simple case.
	 */

	if ((type1 == TCL_NUMBER_LONG) && (type2 == TCL_NUMBER_LONG)) {
	    l1 = *((const long *)ptr1);
	    l2 = *((const long *)ptr2);

	    switch (*pc) {
	    case INST_MOD:
		if (l2 == 0) {
		    TRACE(("%s %s => DIVIDE BY ZERO\n", O2S(valuePtr),
			    O2S(value2Ptr)));
		    goto divideByZero;
		} else if ((l2 == 1) || (l2 == -1)) {
		    /*
		     * Div. by |1| always yields remainder of 0.
		     */

		    TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
		    objResultPtr = TCONST(0);
		    TRACE(("%s\n", O2S(objResultPtr)));
		    NEXT_INST_F(1, 2, 1);
		} else if (l1 == 0) {
		    /*
		     * 0 % (non-zero) always yields remainder of 0.
		     */

		    TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
		    objResultPtr = TCONST(0);
		    TRACE(("%s\n", O2S(objResultPtr)));
		    NEXT_INST_F(1, 2, 1);
		} else {
		    lResult = l1 / l2;

		    /*
		     * Force Tcl's integer division rules.
		     * TODO: examine for logic simplification
		     */

		    if ((lResult < 0 || (lResult == 0 &&
			    ((l1 < 0 && l2 > 0) || (l1 > 0 && l2 < 0)))) &&
			    (lResult * l2 != l1)) {
			lResult -= 1;
		    }
		    lResult = l1 - l2*lResult;
		    goto longResultOfArithmetic;
		}

	    case INST_RSHIFT:
		if (l2 < 0) {
		    Tcl_SetObjResult(interp, Tcl_NewStringObj(
			    "negative shift argument", TCL_STRLEN));
#if 0
		    DECACHE_STACK_INFO();
		    Tcl_SetErrorCode(interp, "ARITH", "DOMAIN",
			    "domain error: argument not in valid range",
			    NULL);
		    CACHE_STACK_INFO();
#endif
		    goto gotError;
		} else if (l1 == 0) {
		    TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
		    objResultPtr = TCONST(0);
		    TRACE(("%s\n", O2S(objResultPtr)));
		    NEXT_INST_F(1, 2, 1);
		} else {
		    /*
		     * Quickly force large right shifts to 0 or -1.
		     */

		    if (l2 >= (long)(CHAR_BIT*sizeof(long))) {
			/*
			 * We assume that INT_MAX is much larger than the
			 * number of bits in a long. This is a pretty safe
			 * assumption, given that the former is usually around
			 * 4e9 and the latter 32 or 64...
			 */

			TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
			if (l1 > 0L) {
			    objResultPtr = TCONST(0);
			} else {
			    TclNewIntObj(objResultPtr, -1);
			}
			TRACE(("%s\n", O2S(objResultPtr)));
			NEXT_INST_F(1, 2, 1);
		    }

		    /*
		     * Handle shifts within the native long range.
		     */

		    lResult = l1 >> ((int) l2);
		    goto longResultOfArithmetic;
		}

	    case INST_LSHIFT:
		if (l2 < 0) {
		    Tcl_SetObjResult(interp, Tcl_NewStringObj(
			    "negative shift argument", TCL_STRLEN));
#if 0
		    DECACHE_STACK_INFO();
		    Tcl_SetErrorCode(interp, "ARITH", "DOMAIN",
			    "domain error: argument not in valid range",
			    NULL);
		    CACHE_STACK_INFO();
#endif
		    goto gotError;
		} else if (l1 == 0) {
		    TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
		    objResultPtr = TCONST(0);
		    TRACE(("%s\n", O2S(objResultPtr)));
		    NEXT_INST_F(1, 2, 1);
		} else if (l2 > (long) INT_MAX) {
		    /*
		     * Technically, we could hold the value (1 << (INT_MAX+1))
		     * in an mp_int, but since we're using mp_mul_2d() to do
		     * the work, and it takes only an int argument, that's a
		     * good place to draw the line.
		     */

		    Tcl_SetObjResult(interp, Tcl_NewStringObj(
			    "integer value too large to represent",
			    TCL_STRLEN));
#if 0
		    DECACHE_STACK_INFO();
		    Tcl_SetErrorCode(interp, "ARITH", "IOVERFLOW",
			    "integer value too large to represent", NULL);
		    CACHE_STACK_INFO();
#endif
		    goto gotError;
		} else {
		    int shift = (int) l2;

		    /*
		     * Handle shifts within the native long range.
		     */

		    if ((size_t) shift < CHAR_BIT*sizeof(long) && (l1 != 0)
			    && !((l1>0 ? l1 : ~l1) &
				-(1L<<(CHAR_BIT*sizeof(long) - 1 - shift)))) {
			lResult = l1 << shift;
			goto longResultOfArithmetic;
		    }
		}

		/*
		 * Too large; need to use the broken-out function.
		 */

		TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
		break;

	    case INST_BITAND:
		lResult = l1 & l2;
		goto longResultOfArithmetic;
	    case INST_BITOR:
		lResult = l1 | l2;
		goto longResultOfArithmetic;
	    case INST_BITXOR:
		lResult = l1 ^ l2;
	    longResultOfArithmetic:
		TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
		if (Tcl_IsShared(valuePtr)) {
		    TclNewLongObj(objResultPtr, lResult);
		    TRACE(("%s\n", O2S(objResultPtr)));
		    NEXT_INST_F(1, 2, 1);
		}
		TclSetLongObj(valuePtr, lResult);
		TRACE(("%s\n", O2S(valuePtr)));
		NEXT_INST_F(1, 1, 0);
	    }
	}

	/*
	 * DO NOT MERGE THIS WITH THE EQUIVALENT SECTION LATER! That would
	 * encourage the compiler to inline ExecuteExtendedBinaryMathOp, which
	 * is highly undesirable due to the overall impact on size.
	 */

	TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
	objResultPtr = ExecuteExtendedBinaryMathOp(interp, *pc, &TCONST(0),
		valuePtr, value2Ptr);
	if (objResultPtr == DIVIDED_BY_ZERO) {
	    TRACE_APPEND(("DIVIDE BY ZERO\n"));
	    goto divideByZero;
	} else if (objResultPtr == GENERAL_ARITHMETIC_ERROR) {
	    TRACE_APPEND(("ERROR: %s\n",
		    TclGetString(Tcl_GetObjResult(interp))));
	    goto gotError;
	} else if (objResultPtr == NULL) {
	    TRACE_APPEND(("%s\n", O2S(valuePtr)));
	    NEXT_INST_F(1, 1, 0);
	} else {
	    TRACE_APPEND(("%s\n", O2S(objResultPtr)));
	    NEXT_INST_F(1, 2, 1);
	}

    case INST_EXPON:
    case INST_ADD:
    case INST_SUB:
    case INST_DIV:
    case INST_MULT:
	value2Ptr = OBJ_AT_TOS;
	valuePtr = OBJ_UNDER_TOS;

	if ((GetNumberFromObj(NULL, valuePtr, &ptr1, &type1) != TCL_OK)
		|| IsErroringNaNType(type1)) {
	    TRACE(("%.20s %.20s => ILLEGAL 1st TYPE %s\n",
		    O2S(value2Ptr), O2S(valuePtr),
		    (valuePtr->typePtr? valuePtr->typePtr->name: "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, valuePtr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}

#ifdef ACCEPT_NAN
	if (type1 == TCL_NUMBER_NAN) {
	    /*
	     * NaN first argument -> result is also NaN.
	     */

	    NEXT_INST_F(1, 1, 0);
	}
#endif

	if ((GetNumberFromObj(NULL, value2Ptr, &ptr2, &type2) != TCL_OK)
		|| IsErroringNaNType(type2)) {
	    TRACE(("%.20s %.20s => ILLEGAL 2nd TYPE %s\n",
		    O2S(value2Ptr), O2S(valuePtr),
		    (value2Ptr->typePtr? value2Ptr->typePtr->name: "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, value2Ptr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}

#ifdef ACCEPT_NAN
	if (type2 == TCL_NUMBER_NAN) {
	    /*
	     * NaN second argument -> result is also NaN.
	     */

	    objResultPtr = value2Ptr;
	    NEXT_INST_F(1, 2, 1);
	}
#endif

	/*
	 * Handle (long,long) arithmetic as best we can without going out to
	 * an external function.
	 */

	if ((type1 == TCL_NUMBER_LONG) && (type2 == TCL_NUMBER_LONG)) {
	    Tcl_WideInt w1, w2, wResult;

	    l1 = *((const long *)ptr1);
	    l2 = *((const long *)ptr2);

	    switch (*pc) {
	    case INST_ADD:
		w1 = (Tcl_WideInt) l1;
		w2 = (Tcl_WideInt) l2;
		wResult = w1 + w2;
#ifdef NO_WIDE_TYPE
		/*
		 * Check for overflow.
		 */

		if (Overflowing(w1, w2, wResult)) {
		    goto overflow;
		}
#endif
		goto wideResultOfArithmetic;

	    case INST_SUB:
		w1 = (Tcl_WideInt) l1;
		w2 = (Tcl_WideInt) l2;
		wResult = w1 - w2;
#ifdef NO_WIDE_TYPE
		/*
		 * Must check for overflow. The macro tests for overflows in
		 * sums by looking at the sign bits. As we have a subtraction
		 * here, we are adding -w2. As -w2 could in turn overflow, we
		 * test with ~w2 instead: it has the opposite sign bit to w2
		 * so it does the job. Note that the only "bad" case (w2==0)
		 * is irrelevant for this macro, as in that case w1 and
		 * wResult have the same sign and there is no overflow anyway.
		 */

		if (Overflowing(w1, ~w2, wResult)) {
		    goto overflow;
		}
#endif
	    wideResultOfArithmetic:
		TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
		if (Tcl_IsShared(valuePtr)) {
		    objResultPtr = Tcl_NewWideIntObj(wResult);
		    TRACE(("%s\n", O2S(objResultPtr)));
		    NEXT_INST_F(1, 2, 1);
		}
		Tcl_SetWideIntObj(valuePtr, wResult);
		TRACE(("%s\n", O2S(valuePtr)));
		NEXT_INST_F(1, 1, 0);

	    case INST_DIV:
		if (l2 == 0) {
		    TRACE(("%s %s => DIVIDE BY ZERO\n",
			    O2S(valuePtr), O2S(value2Ptr)));
		    goto divideByZero;
		} else if ((l1 == LONG_MIN) && (l2 == -1)) {
		    /*
		     * Can't represent (-LONG_MIN) as a long.
		     */

		    goto overflow;
		}
		lResult = l1 / l2;

		/*
		 * Force Tcl's integer division rules.
		 * TODO: examine for logic simplification
		 */

		if (((lResult < 0) || ((lResult == 0) &&
			((l1 < 0 && l2 > 0) || (l1 > 0 && l2 < 0)))) &&
			((lResult * l2) != l1)) {
		    lResult -= 1;
		}
		goto longResultOfArithmetic;

	    case INST_MULT:
		if (((sizeof(long) >= 2*sizeof(int))
			&& (l1 <= INT_MAX) && (l1 >= INT_MIN)
			&& (l2 <= INT_MAX) && (l2 >= INT_MIN))
			|| ((sizeof(long) >= 2*sizeof(short))
			&& (l1 <= SHRT_MAX) && (l1 >= SHRT_MIN)
			&& (l2 <= SHRT_MAX) && (l2 >= SHRT_MIN))) {
		    lResult = l1 * l2;
		    goto longResultOfArithmetic;
		}
	    }

	    /*
	     * Fall through with INST_EXPON, INST_DIV and large multiplies.
	     */
	}

    overflow:
	TRACE(("%s %s => ", O2S(valuePtr), O2S(value2Ptr)));
	objResultPtr = ExecuteExtendedBinaryMathOp(interp, *pc, &TCONST(0),
		valuePtr, value2Ptr);
	if (objResultPtr == DIVIDED_BY_ZERO) {
	    TRACE_APPEND(("DIVIDE BY ZERO\n"));
	    goto divideByZero;
	} else if (objResultPtr == EXPONENT_OF_ZERO) {
	    TRACE_APPEND(("EXPONENT OF ZERO\n"));
	    goto exponOfZero;
	} else if (objResultPtr == GENERAL_ARITHMETIC_ERROR) {
	    TRACE_APPEND(("ERROR: %s\n",
		    TclGetString(Tcl_GetObjResult(interp))));
	    goto gotError;
	} else if (objResultPtr == NULL) {
	    TRACE_APPEND(("%s\n", O2S(valuePtr)));
	    NEXT_INST_F(1, 1, 0);
	} else {
	    TRACE_APPEND(("%s\n", O2S(objResultPtr)));
	    NEXT_INST_F(1, 2, 1);
	}

    case INST_LNOT: {
	int b;

	valuePtr = OBJ_AT_TOS;

	/* TODO - check claim that taking address of b harms performance */
	/* TODO - consider optimization search for constants */
	if (TclGetBooleanFromObj(NULL, valuePtr, &b) != TCL_OK) {
	    TRACE(("\"%.20s\" => ILLEGAL TYPE %s\n", O2S(valuePtr),
		    (valuePtr->typePtr? valuePtr->typePtr->name : "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, valuePtr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}
	/* TODO: Consider peephole opt. */
	objResultPtr = TCONST(!b);
	NEXT_INST_F(1, 1, 1);
    }

    case INST_BITNOT:
	valuePtr = OBJ_AT_TOS;
	if ((GetNumberFromObj(NULL, valuePtr, &ptr1, &type1) != TCL_OK)
		|| (type1==TCL_NUMBER_NAN) || (type1==TCL_NUMBER_DOUBLE)) {
	    /*
	     * ... ~$NonInteger => raise an error.
	     */

	    TRACE(("\"%.20s\" => ILLEGAL TYPE %s \n", O2S(valuePtr),
		    (valuePtr->typePtr? valuePtr->typePtr->name : "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, valuePtr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}
	if (type1 == TCL_NUMBER_LONG) {
	    l1 = *((const long *) ptr1);
	    if (Tcl_IsShared(valuePtr)) {
		TclNewLongObj(objResultPtr, ~l1);
		NEXT_INST_F(1, 1, 1);
	    }
	    TclSetLongObj(valuePtr, ~l1);
	    NEXT_INST_F(1, 0, 0);
	}
	objResultPtr = ExecuteExtendedUnaryMathOp(*pc, valuePtr);
	if (objResultPtr != NULL) {
	    NEXT_INST_F(1, 1, 1);
	} else {
	    NEXT_INST_F(1, 0, 0);
	}

    case INST_UMINUS:
	valuePtr = OBJ_AT_TOS;
	if ((GetNumberFromObj(NULL, valuePtr, &ptr1, &type1) != TCL_OK)
		|| IsErroringNaNType(type1)) {
	    TRACE(("\"%.20s\" => ILLEGAL TYPE %s \n", O2S(valuePtr),
		    (valuePtr->typePtr? valuePtr->typePtr->name : "null")));
	    DECACHE_STACK_INFO();
	    IllegalExprOperandType(interp, pc, valuePtr);
	    CACHE_STACK_INFO();
	    goto gotError;
	}
	switch (type1) {
	case TCL_NUMBER_NAN:
	    /* -NaN => NaN */
	    NEXT_INST_F(1, 0, 0);
	case TCL_NUMBER_LONG:
	    l1 = *((const long *) ptr1);
	    if (l1 != LONG_MIN) {
		if (Tcl_IsShared(valuePtr)) {
		    TclNewLongObj(objResultPtr, -l1);
		    NEXT_INST_F(1, 1, 1);
		}
		TclSetLongObj(valuePtr, -l1);
		NEXT_INST_F(1, 0, 0);
	    }
	    /* FALLTHROUGH */
	}
	objResultPtr = ExecuteExtendedUnaryMathOp(*pc, valuePtr);
	if (objResultPtr != NULL) {
	    NEXT_INST_F(1, 1, 1);
	} else {
	    NEXT_INST_F(1, 0, 0);
	}

    case INST_UPLUS:
    case INST_TRY_CVT_TO_NUMERIC:
	/*
	 * Try to convert the topmost stack object to numeric object. This is
	 * done in order to support [expr]'s policy of interpreting operands
	 * if at all possible as numbers first, then strings.
	 */

	valuePtr = OBJ_AT_TOS;

	if (GetNumberFromObj(NULL, valuePtr, &ptr1, &type1) != TCL_OK) {
	    if (*pc == INST_UPLUS) {
		/*
		 * ... +$NonNumeric => raise an error.
		 */

		TRACE(("\"%.20s\" => ILLEGAL TYPE %s \n", O2S(valuePtr),
			(valuePtr->typePtr? valuePtr->typePtr->name:"null")));
		DECACHE_STACK_INFO();
		IllegalExprOperandType(interp, pc, valuePtr);
		CACHE_STACK_INFO();
		goto gotError;
	    }

	    /* ... TryConvertToNumeric($NonNumeric) is acceptable */
	    TRACE(("\"%.20s\" => not numeric\n", O2S(valuePtr)));
	    NEXT_INST_F(1, 0, 0);
	}
	if (IsErroringNaNType(type1)) {
	    if (*pc == INST_UPLUS) {
		/*
		 * ... +$NonNumeric => raise an error.
		 */

		TRACE(("\"%.20s\" => ILLEGAL TYPE %s \n", O2S(valuePtr),
			(valuePtr->typePtr? valuePtr->typePtr->name:"null")));
		DECACHE_STACK_INFO();
		IllegalExprOperandType(interp, pc, valuePtr);
		CACHE_STACK_INFO();
	    } else {
		/*
		 * Numeric conversion of NaN -> error.
		 */

		TRACE(("\"%.20s\" => IEEE FLOATING PT ERROR\n",
			O2S(objResultPtr)));
		DECACHE_STACK_INFO();
		TclExprFloatError(interp, *((const double *) ptr1));
		CACHE_STACK_INFO();
	    }
	    goto gotError;
	}

	/*
	 * Ensure that the numeric value has a string rep the same as the
	 * formatted version of its internal rep. This is used, e.g., to make
	 * sure that "expr {0001}" yields "1", not "0001". We implement this
	 * by _discarding_ the string rep since we know it will be
	 * regenerated, if needed later, by formatting the internal rep's
	 * value.
	 */

	if (valuePtr->bytes == NULL) {
	    TRACE(("\"%.20s\" => numeric, same Tcl_Obj\n", O2S(valuePtr)));
	    NEXT_INST_F(1, 0, 0);
	}
	if (Tcl_IsShared(valuePtr)) {
	    /*
	     * Here we do some surgery within the Tcl_Obj internals. We want
	     * to copy the intrep, but not the string, so we temporarily hide
	     * the string so we do not copy it.
	     */

	    char *savedString = valuePtr->bytes;

	    valuePtr->bytes = NULL;
	    objResultPtr = Tcl_DuplicateObj(valuePtr);
	    valuePtr->bytes = savedString;
	    TRACE(("\"%.20s\" => numeric, new Tcl_Obj\n", O2S(valuePtr)));
	    NEXT_INST_F(1, 1, 1);
	}
	TclInvalidateStringRep(valuePtr);
	TRACE(("\"%.20s\" => numeric, same Tcl_Obj\n", O2S(valuePtr)));
	NEXT_INST_F(1, 0, 0);
    }

    /*
     *	   End of numeric operator instructions.
     * -----------------------------------------------------------------
     */

    case INST_BREAK:
	/*
	DECACHE_STACK_INFO();
	Tcl_ResetResult(interp);
	CACHE_STACK_INFO();
	*/
	result = TCL_BREAK;
	cleanup = 0;
	goto processExceptionReturn;

    case INST_CONTINUE:
	/*
	DECACHE_STACK_INFO();
	Tcl_ResetResult(interp);
	CACHE_STACK_INFO();
	*/
	result = TCL_CONTINUE;
	cleanup = 0;
	goto processExceptionReturn;

    {
	ForeachInfo *infoPtr;
	Var *iterVarPtr, *listVarPtr;
	Tcl_Obj *oldValuePtr, *listPtr, **elements;
	ForeachVarList *varListPtr;
	int numLists, iterNum, listTmpIndex, numVars;
	int varIndex, valIndex, continueLoop, j, iterTmpIndex;
	long i;
	size_t listLen;

    case INST_FOREACH_START4:
	/*
	 * Initialize the temporary local var that holds the count of the
	 * number of iterations of the loop body to -1.
	 */

	opnd = TclGetUInt4AtPtr(pc+1);
	infoPtr = codePtr->auxDataArrayPtr[opnd].clientData;
	iterTmpIndex = infoPtr->loopCtTemp;
	iterVarPtr = LOCAL(iterTmpIndex);
	oldValuePtr = iterVarPtr->value.objPtr;

	if (oldValuePtr == NULL) {
	    TclNewLongObj(iterVarPtr->value.objPtr, -1);
	    Tcl_IncrRefCount(iterVarPtr->value.objPtr);
	} else {
	    TclSetLongObj(oldValuePtr, -1);
	}
	TRACE(("%u => loop iter count temp %d\n", opnd, iterTmpIndex));

#ifndef TCL_COMPILE_DEBUG
	/*
	 * Remark that the compiler ALWAYS sets INST_FOREACH_STEP4 immediately
	 * after INST_FOREACH_START4 - let us just fall through instead of
	 * jumping back to the top.
	 */

	pc += 5;
	TCL_DTRACE_INST_NEXT();
#else
	NEXT_INST_F(5, 0, 0);
#endif

    case INST_FOREACH_STEP4:
	/*
	 * "Step" a foreach loop (i.e., begin its next iteration) by assigning
	 * the next value list element to each loop var.
	 */

	opnd = TclGetUInt4AtPtr(pc+1);
	infoPtr = codePtr->auxDataArrayPtr[opnd].clientData;
	numLists = infoPtr->numLists;

	/*
	 * Increment the temp holding the loop iteration number.
	 */

	iterVarPtr = LOCAL(infoPtr->loopCtTemp);
	valuePtr = iterVarPtr->value.objPtr;
	iterNum = valuePtr->internalRep.longValue + 1;
	TclSetLongObj(valuePtr, iterNum);

	/*
	 * Check whether all value lists are exhausted and we should stop the
	 * loop.
	 */

	continueLoop = 0;
	listTmpIndex = infoPtr->firstValueTemp;
	for (i = 0;  i < numLists;  i++) {
	    varListPtr = infoPtr->varLists[i];
	    numVars = varListPtr->numVars;

	    listVarPtr = LOCAL(listTmpIndex);
	    listPtr = listVarPtr->value.objPtr;
	    if (TclListObjLength(interp, listPtr, &listLen) != TCL_OK) {
		TRACE_WITH_OBJ(("%u => ERROR converting list %ld, \"%s\": ",
			opnd, i, O2S(listPtr)), Tcl_GetObjResult(interp));
		goto gotError;
	    }
	    if (listLen > iterNum * numVars) {
		continueLoop = 1;
	    }
	    listTmpIndex++;
	}

	/*
	 * If some var in some var list still has a remaining list element
	 * iterate one more time. Assign to var the next element from its
	 * value list. We already checked above that each list temp holds a
	 * valid list object (by calling Tcl_ListObjLength), but cannot rely
	 * on that check remaining valid: one list could have been shimmered
	 * as a side effect of setting a traced variable.
	 */

	if (continueLoop) {
	    listTmpIndex = infoPtr->firstValueTemp;
	    for (i = 0;  i < numLists;  i++) {
		varListPtr = infoPtr->varLists[i];
		numVars = varListPtr->numVars;

		listVarPtr = LOCAL(listTmpIndex);
		listPtr = TclListObjCopy(NULL, listVarPtr->value.objPtr);
		TclListObjGetElements(interp, listPtr, &listLen, &elements);

		valIndex = (iterNum * numVars);
		for (j = 0;  j < numVars;  j++) {
		    if (valIndex >= listLen) {
			TclNewObj(valuePtr);
		    } else {
			valuePtr = elements[valIndex];
		    }

		    varIndex = varListPtr->varIndexes[j];
		    varPtr = LOCAL(varIndex);
		    while (TclIsVarLink(varPtr)) {
			varPtr = varPtr->value.linkPtr;
		    }
		    if (TclIsVarDirectWritable(varPtr)) {
			value2Ptr = varPtr->value.objPtr;
			if (valuePtr != value2Ptr) {
			    if (value2Ptr != NULL) {
				TclDecrRefCount(value2Ptr);
			    }
			    varPtr->value.objPtr = valuePtr;
			    Tcl_IncrRefCount(valuePtr);
			}
		    } else {
			DECACHE_STACK_INFO();
			if (TclPtrSetVar(interp, varPtr, NULL, NULL, NULL,
				valuePtr, TCL_LEAVE_ERR_MSG, varIndex)==NULL){
			    CACHE_STACK_INFO();
			    TRACE_WITH_OBJ((
				    "%u => ERROR init. index temp %d: ",
				    opnd,varIndex), Tcl_GetObjResult(interp));
			    TclDecrRefCount(listPtr);
			    goto gotError;
			}
			CACHE_STACK_INFO();
		    }
		    valIndex++;
		}
		TclDecrRefCount(listPtr);
		listTmpIndex++;
	    }
	}
	TRACE(("%u => %d lists, iter %d, %s loop\n", opnd, numLists,
		iterNum, (continueLoop? "continue" : "exit")));

	/*
	 * Run-time peep-hole optimisation: the compiler ALWAYS follows
	 * INST_FOREACH_STEP4 with an INST_JUMP_FALSE. We just skip that
	 * instruction and jump direct from here.
	 */

	pc += 5;
	if (*pc == INST_JUMP_FALSE1) {
	    NEXT_INST_F((continueLoop? 2 : TclGetInt1AtPtr(pc+1)), 0, 0);
	} else {
	    NEXT_INST_F((continueLoop? 5 : TclGetInt4AtPtr(pc+1)), 0, 0);
	}
    }

    case INST_BEGIN_CATCH4:
	/*
	 * Record start of the catch command with exception range index equal
	 * to the operand. Push the current stack depth onto the special catch
	 * stack.
	 */

	*(++catchTop) = CURR_DEPTH;
	TRACE(("%u => catchTop=%d, stackTop=%d\n",
		TclGetUInt4AtPtr(pc+1), (int) (catchTop - initCatchTop - 1),
		(int) CURR_DEPTH));
	NEXT_INST_F(5, 0, 0);

    case INST_END_CATCH:
	catchTop--;
	DECACHE_STACK_INFO();
	Tcl_ResetResult(interp);
	CACHE_STACK_INFO();
	result = TCL_OK;
	TRACE(("=> catchTop=%d\n", (int) (catchTop - initCatchTop - 1)));
	NEXT_INST_F(1, 0, 0);

    case INST_PUSH_RESULT:
	objResultPtr = Tcl_GetObjResult(interp);
	TRACE_WITH_OBJ(("=> "), objResultPtr);

	/*
	 * See the comments at INST_INVOKE_STK
	 */

	TclNewObj(objPtr);
	Tcl_IncrRefCount(objPtr);
	iPtr->objResultPtr = objPtr;
	NEXT_INST_F(1, 0, -1);

    case INST_PUSH_RETURN_CODE:
	TclNewIntObj(objResultPtr, result);
	TRACE(("=> %u\n", result));
	NEXT_INST_F(1, 0, 1);

    case INST_PUSH_RETURN_OPTIONS:
	DECACHE_STACK_INFO();
	objResultPtr = Tcl_GetReturnOptions(interp, result);
	CACHE_STACK_INFO();
	TRACE_WITH_OBJ(("=> "), objResultPtr);
	NEXT_INST_F(1, 0, 1);

    case INST_RETURN_CODE_BRANCH: {
	int code;

	if (TclGetIntFromObj(NULL, OBJ_AT_TOS, &code) != TCL_OK) {
	    Tcl_Panic("INST_RETURN_CODE_BRANCH: TOS not a return code!");
	}
	if (code == TCL_OK) {
	    Tcl_Panic("INST_RETURN_CODE_BRANCH: TOS is TCL_OK!");
	}
	if (code < TCL_ERROR || code > TCL_CONTINUE) {
	    code = TCL_CONTINUE + 1;
	}
	NEXT_INST_F(2*code -1, 1, 0);
    }

    /*
     * -----------------------------------------------------------------
     *	   Start of dictionary-related instructions.
     */

    {
	int opnd2, allocateDict, done, i, allocdict;
	Tcl_Obj *dictPtr, *statePtr, *keyPtr, *listPtr, *varNamePtr, *keysPtr;
	Tcl_Obj *emptyPtr, **keyPtrPtr;
	Tcl_DictSearch *searchPtr;
	DictUpdateInfo *duiPtr;
	size_t size;

    case INST_DICT_VERIFY:
	dictPtr = OBJ_AT_TOS;
	TRACE(("=> "));
	if (Tcl_DictObjSize(interp, dictPtr, &size) != TCL_OK) {
	    TRACE_APPEND(("ERROR verifying dictionary nature of \"%s\": %s\n",
		    O2S(OBJ_AT_DEPTH(opnd)), O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	TRACE_APPEND(("OK\n"));
	NEXT_INST_F(1, 1, 0);

    case INST_DICT_GET:
    case INST_DICT_EXISTS: {
	register Tcl_Interp *interp2 = interp;

	opnd = TclGetUInt4AtPtr(pc+1);
	TRACE(("%u => ", opnd));
	dictPtr = OBJ_AT_DEPTH(opnd);
	if (*pc == INST_DICT_EXISTS) {
	    interp2 = NULL;
	}
	if (opnd > 1) {
	    dictPtr = TclTraceDictPath(interp2, dictPtr, opnd-1,
		    &OBJ_AT_DEPTH(opnd-1), DICT_PATH_READ);
	    if (dictPtr == NULL) {
		if (*pc == INST_DICT_EXISTS) {
		    goto dictNotExists;
		}
		TRACE_WITH_OBJ((
			"ERROR tracing dictionary path into \"%s\": ",
			O2S(OBJ_AT_DEPTH(opnd))),
			Tcl_GetObjResult(interp));
		goto gotError;
	    }
	}
	if (Tcl_DictObjGet(interp2, dictPtr, OBJ_AT_TOS,
		&objResultPtr) == TCL_OK) {
	    if (*pc == INST_DICT_EXISTS) {
		objResultPtr = TCONST(objResultPtr ? 1 : 0);
		TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
		NEXT_INST_V(5, opnd+1, 1);
	    }
	    if (objResultPtr) {
		TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
		NEXT_INST_V(5, opnd+1, 1);
	    }
	    DECACHE_STACK_INFO();
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "key \"%s\" not known in dictionary",
		    TclGetString(OBJ_AT_TOS)));
	    Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "DICT",
		    TclGetString(OBJ_AT_TOS), NULL);
	    CACHE_STACK_INFO();
	    TRACE_WITH_OBJ(("%u => ERROR ", opnd), Tcl_GetObjResult(interp));
	} else {
	    if (*pc == INST_DICT_EXISTS) {
	    dictNotExists:
		objResultPtr = TCONST(0);
		TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
		NEXT_INST_V(5, opnd+1, 1);
	    }
	    TRACE_WITH_OBJ((
		    "%u => ERROR reading leaf dictionary key \"%s\": ",
		    opnd, O2S(dictPtr)), Tcl_GetObjResult(interp));
	}
	goto gotError;
    }

    case INST_DICT_SET:
    case INST_DICT_UNSET:
    case INST_DICT_INCR_IMM:
	opnd = TclGetUInt4AtPtr(pc+1);
	opnd2 = TclGetUInt4AtPtr(pc+5);

	varPtr = LOCAL(opnd2);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%u %u => ", opnd, opnd2));
	if (TclIsVarDirectReadable(varPtr)) {
	    dictPtr = varPtr->value.objPtr;
	} else {
	    DECACHE_STACK_INFO();
	    dictPtr = TclPtrGetVar(interp, varPtr, NULL,NULL,NULL, 0, opnd2);
	    CACHE_STACK_INFO();
	}
	if (dictPtr == NULL) {
	    TclNewObj(dictPtr);
	    allocateDict = 1;
	} else {
	    allocateDict = Tcl_IsShared(dictPtr);
	    if (allocateDict) {
		dictPtr = Tcl_DuplicateObj(dictPtr);
	    }
	}

	switch (*pc) {
	case INST_DICT_SET:
	    cleanup = opnd + 1;
	    result = Tcl_DictObjPutKeyList(interp, dictPtr, opnd,
		    &OBJ_AT_DEPTH(opnd), OBJ_AT_TOS);
	    break;
	case INST_DICT_INCR_IMM:
	    cleanup = 1;
	    opnd = TclGetInt4AtPtr(pc+1);
	    result = Tcl_DictObjGet(interp, dictPtr, OBJ_AT_TOS, &valuePtr);
	    if (result != TCL_OK) {
		break;
	    }
	    if (valuePtr == NULL) {
		Tcl_DictObjPut(NULL, dictPtr, OBJ_AT_TOS,Tcl_NewIntObj(opnd));
	    } else {
		value2Ptr = Tcl_NewIntObj(opnd);
		Tcl_IncrRefCount(value2Ptr);
		if (Tcl_IsShared(valuePtr)) {
		    valuePtr = Tcl_DuplicateObj(valuePtr);
		    Tcl_DictObjPut(NULL, dictPtr, OBJ_AT_TOS, valuePtr);
		}
		result = TclIncrObj(interp, valuePtr, value2Ptr);
		if (result == TCL_OK) {
		    TclInvalidateStringRep(dictPtr);
		}
		TclDecrRefCount(value2Ptr);
	    }
	    break;
	case INST_DICT_UNSET:
	    cleanup = opnd;
	    result = Tcl_DictObjRemoveKeyList(interp, dictPtr, opnd,
		    &OBJ_AT_DEPTH(opnd-1));
	    break;
	default:
	    cleanup = 0; /* stop compiler warning */
	    Tcl_Panic("Should not happen!");
	}

	if (result != TCL_OK) {
	    if (allocateDict) {
		TclDecrRefCount(dictPtr);
	    }
	    TRACE_WITH_OBJ(("%u %u => ERROR updating dictionary: ",
		    opnd, opnd2), Tcl_GetObjResult(interp));
	    goto checkForCatch;
	}

	if (TclIsVarDirectWritable(varPtr)) {
	    if (allocateDict) {
		value2Ptr = varPtr->value.objPtr;
		Tcl_IncrRefCount(dictPtr);
		if (value2Ptr != NULL) {
		    TclDecrRefCount(value2Ptr);
		}
		varPtr->value.objPtr = dictPtr;
	    }
	    objResultPtr = dictPtr;
	} else {
	    Tcl_IncrRefCount(dictPtr);
	    DECACHE_STACK_INFO();
	    objResultPtr = TclPtrSetVar(interp, varPtr, NULL, NULL, NULL,
		    dictPtr, TCL_LEAVE_ERR_MSG, opnd2);
	    CACHE_STACK_INFO();
	    TclDecrRefCount(dictPtr);
	    if (objResultPtr == NULL) {
		TRACE_APPEND(("ERROR: %.30s\n",
			O2S(Tcl_GetObjResult(interp))));
		goto gotError;
	    }
	}
#ifndef TCL_COMPILE_DEBUG
	if (*(pc+9) == INST_POP) {
	    NEXT_INST_V(10, cleanup, 0);
	}
#endif
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_V(9, cleanup, 1);

    case INST_DICT_APPEND:
    case INST_DICT_LAPPEND:
	opnd = TclGetUInt4AtPtr(pc+1);
	varPtr = LOCAL(opnd);
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%u => ", opnd));
	if (TclIsVarDirectReadable(varPtr)) {
	    dictPtr = varPtr->value.objPtr;
	} else {
	    DECACHE_STACK_INFO();
	    dictPtr = TclPtrGetVar(interp, varPtr, NULL, NULL, NULL, 0, opnd);
	    CACHE_STACK_INFO();
	}
	if (dictPtr == NULL) {
	    TclNewObj(dictPtr);
	    allocateDict = 1;
	} else {
	    allocateDict = Tcl_IsShared(dictPtr);
	    if (allocateDict) {
		dictPtr = Tcl_DuplicateObj(dictPtr);
	    }
	}

	if (Tcl_DictObjGet(interp, dictPtr, OBJ_UNDER_TOS,
		&valuePtr) != TCL_OK) {
	    if (allocateDict) {
		TclDecrRefCount(dictPtr);
	    }
	    goto gotError;
	}

	/*
	 * Note that a non-existent key results in a NULL valuePtr, which is a
	 * case handled separately below. What we *can* say at this point is
	 * that the write-back will always succeed.
	 */

	switch (*pc) {
	case INST_DICT_APPEND:
	    if (valuePtr == NULL) {
		Tcl_DictObjPut(NULL, dictPtr, OBJ_UNDER_TOS, OBJ_AT_TOS);
	    } else if (Tcl_IsShared(valuePtr)) {
		valuePtr = Tcl_DuplicateObj(valuePtr);
		Tcl_AppendObjToObj(valuePtr, OBJ_AT_TOS);
		Tcl_DictObjPut(NULL, dictPtr, OBJ_UNDER_TOS, valuePtr);
	    } else {
		Tcl_AppendObjToObj(valuePtr, OBJ_AT_TOS);

		/*
		 * Must invalidate the string representation of dictionary
		 * here because we have directly updated the internal
		 * representation; if we don't, callers could see the wrong
		 * string rep despite the internal version of the dictionary
		 * having the correct value. [Bug 3079830]
		 */

		TclInvalidateStringRep(dictPtr);
	    }
	    break;
	case INST_DICT_LAPPEND:
	    /*
	     * More complex because list-append can fail.
	     */

	    if (valuePtr == NULL) {
		Tcl_DictObjPut(NULL, dictPtr, OBJ_UNDER_TOS,
			Tcl_NewListObj(1, &OBJ_AT_TOS));
		break;
	    } else if (Tcl_IsShared(valuePtr)) {
		valuePtr = Tcl_DuplicateObj(valuePtr);
		if (Tcl_ListObjAppendElement(interp, valuePtr,
			OBJ_AT_TOS) != TCL_OK) {
		    TclDecrRefCount(valuePtr);
		    if (allocateDict) {
			TclDecrRefCount(dictPtr);
		    }
		    goto gotError;
		}
		Tcl_DictObjPut(NULL, dictPtr, OBJ_UNDER_TOS, valuePtr);
	    } else {
		if (Tcl_ListObjAppendElement(interp, valuePtr,
			OBJ_AT_TOS) != TCL_OK) {
		    if (allocateDict) {
			TclDecrRefCount(dictPtr);
		    }
		    goto gotError;
		}

		/*
		 * Must invalidate the string representation of dictionary
		 * here because we have directly updated the internal
		 * representation; if we don't, callers could see the wrong
		 * string rep despite the internal version of the dictionary
		 * having the correct value. [Bug 3079830]
		 */

		TclInvalidateStringRep(dictPtr);
	    }
	    break;
	default:
	    Tcl_Panic("Should not happen!");
	}

	if (TclIsVarDirectWritable(varPtr)) {
	    if (allocateDict) {
		value2Ptr = varPtr->value.objPtr;
		Tcl_IncrRefCount(dictPtr);
		if (value2Ptr != NULL) {
		    TclDecrRefCount(value2Ptr);
		}
		varPtr->value.objPtr = dictPtr;
	    }
	    objResultPtr = dictPtr;
	} else {
	    Tcl_IncrRefCount(dictPtr);
	    DECACHE_STACK_INFO();
	    objResultPtr = TclPtrSetVar(interp, varPtr, NULL, NULL, NULL,
		    dictPtr, TCL_LEAVE_ERR_MSG, opnd);
	    CACHE_STACK_INFO();
	    TclDecrRefCount(dictPtr);
	    if (objResultPtr == NULL) {
		TRACE_APPEND(("ERROR: %.30s\n",
			O2S(Tcl_GetObjResult(interp))));
		goto gotError;
	    }
	}
#ifndef TCL_COMPILE_DEBUG
	if (*(pc+5) == INST_POP) {
	    NEXT_INST_F(6, 2, 0);
	}
#endif
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_F(5, 2, 1);

    case INST_DICT_FIRST:
	opnd = TclGetUInt4AtPtr(pc+1);
	TRACE(("%u => ", opnd));
	dictPtr = POP_OBJECT();
	searchPtr = ckalloc(sizeof(Tcl_DictSearch));
	if (Tcl_DictObjFirst(interp, dictPtr, searchPtr, &keyPtr,
		&valuePtr, &done) != TCL_OK) {
	    ckfree(searchPtr);
	    goto gotError;
	}
	TclNewObj(statePtr);
	statePtr->typePtr = &dictIteratorType;
	statePtr->internalRep.twoPtrValue.ptr1 = searchPtr;
	statePtr->internalRep.twoPtrValue.ptr2 = dictPtr;
	varPtr = LOCAL(opnd);
	if (varPtr->value.objPtr) {
	    if (varPtr->value.objPtr->typePtr == &dictIteratorType) {
		Tcl_Panic("mis-issued dictFirst!");
	    }
	    TclDecrRefCount(varPtr->value.objPtr);
	}
	varPtr->value.objPtr = statePtr;
	Tcl_IncrRefCount(statePtr);
	goto pushDictIteratorResult;

    case INST_DICT_NEXT:
	opnd = TclGetUInt4AtPtr(pc+1);
	TRACE(("%u => ", opnd));
	statePtr = (*LOCAL(opnd)).value.objPtr;
	if (statePtr == NULL || statePtr->typePtr != &dictIteratorType) {
	    Tcl_Panic("mis-issued dictNext!");
	}
	searchPtr = statePtr->internalRep.twoPtrValue.ptr1;
	Tcl_DictObjNext(searchPtr, &keyPtr, &valuePtr, &done);
    pushDictIteratorResult:
	if (done) {
	    TclNewObj(emptyPtr);
	    PUSH_OBJECT(emptyPtr);
	    PUSH_OBJECT(emptyPtr);
	} else {
	    PUSH_OBJECT(valuePtr);
	    PUSH_OBJECT(keyPtr);
	}

#ifndef TCL_COMPILE_DEBUG
	/*
	 * The INST_DICT_FIRST and INST_DICT_NEXT instructsions are always
	 * followed by a conditional jump, so we can take advantage of this to
	 * do some peephole optimization (note that we're careful to not close
	 * out someone doing something else).
	 */

	pc += 5;
	switch (*pc) {
	case INST_JUMP_FALSE1:
	    NEXT_INST_F((done ? 2 : TclGetInt1AtPtr(pc+1)), 0, 0);
	case INST_JUMP_FALSE4:
	    NEXT_INST_F((done ? 5 : TclGetInt4AtPtr(pc+1)), 0, 0);
	case INST_JUMP_TRUE1:
	    NEXT_INST_F((done ? TclGetInt1AtPtr(pc+1) : 2), 0, 0);
	case INST_JUMP_TRUE4:
	    NEXT_INST_F((done ? TclGetInt4AtPtr(pc+1) : 5), 0, 0);
	default:
	    pc -= 5;
	    /* fall through to non-debug handling */
	}
#endif

	TRACE_APPEND(("\"%.30s\" \"%.30s\" %d",
		O2S(OBJ_UNDER_TOS), O2S(OBJ_AT_TOS), done));
	objResultPtr = TCONST(done);
	/* TODO: consider opt like INST_FOREACH_STEP4 */
	NEXT_INST_F(5, 0, 1);

    case INST_DICT_UPDATE_START:
	opnd = TclGetUInt4AtPtr(pc+1);
	opnd2 = TclGetUInt4AtPtr(pc+5);
	varPtr = LOCAL(opnd);
	duiPtr = codePtr->auxDataArrayPtr[opnd2].clientData;
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%u => ", opnd));
	if (TclIsVarDirectReadable(varPtr)) {
	    dictPtr = varPtr->value.objPtr;
	} else {
	    DECACHE_STACK_INFO();
	    dictPtr = TclPtrGetVar(interp, varPtr, NULL, NULL, NULL,
		    TCL_LEAVE_ERR_MSG, opnd);
	    CACHE_STACK_INFO();
	    if (dictPtr == NULL) {
		goto gotError;
	    }
	}
	if (TclListObjGetElements(interp, OBJ_AT_TOS, &length,
		&keyPtrPtr) != TCL_OK) {
	    goto gotError;
	}
	if (length != duiPtr->length) {
	    Tcl_Panic("dictUpdateStart argument length mismatch");
	}
	for (i=0 ; i<length ; i++) {
	    if (Tcl_DictObjGet(interp, dictPtr, keyPtrPtr[i],
		    &valuePtr) != TCL_OK) {
		goto gotError;
	    }
	    varPtr = LOCAL(duiPtr->varIndices[i]);
	    while (TclIsVarLink(varPtr)) {
		varPtr = varPtr->value.linkPtr;
	    }
	    DECACHE_STACK_INFO();
	    if (valuePtr == NULL) {
		TclObjUnsetVar2(interp,
			localName(iPtr->varFramePtr, duiPtr->varIndices[i]),
			NULL, 0);
	    } else if (TclPtrSetVar(interp, varPtr, NULL, NULL, NULL,
		    valuePtr, TCL_LEAVE_ERR_MSG,
		    duiPtr->varIndices[i]) == NULL) {
		CACHE_STACK_INFO();
		goto gotError;
	    }
	    CACHE_STACK_INFO();
	}
	NEXT_INST_F(9, 0, 0);

    case INST_DICT_UPDATE_END:
	opnd = TclGetUInt4AtPtr(pc+1);
	opnd2 = TclGetUInt4AtPtr(pc+5);
	varPtr = LOCAL(opnd);
	duiPtr = codePtr->auxDataArrayPtr[opnd2].clientData;
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	TRACE(("%u => ", opnd));
	if (TclIsVarDirectReadable(varPtr)) {
	    dictPtr = varPtr->value.objPtr;
	} else {
	    DECACHE_STACK_INFO();
	    dictPtr = TclPtrGetVar(interp, varPtr, NULL, NULL, NULL, 0, opnd);
	    CACHE_STACK_INFO();
	}
	if (dictPtr == NULL) {
	    NEXT_INST_F(9, 1, 0);
	}
	if (Tcl_DictObjSize(interp, dictPtr, &length) != TCL_OK
		|| TclListObjGetElements(interp, OBJ_AT_TOS, &length,
			&keyPtrPtr) != TCL_OK) {
	    goto gotError;
	}
	allocdict = Tcl_IsShared(dictPtr);
	if (allocdict) {
	    dictPtr = Tcl_DuplicateObj(dictPtr);
	}
	if (length > 0) {
	    TclInvalidateStringRep(dictPtr);
	}
	for (i=0 ; i<length ; i++) {
	    Var *var2Ptr = LOCAL(duiPtr->varIndices[i]);

	    while (TclIsVarLink(var2Ptr)) {
		var2Ptr = var2Ptr->value.linkPtr;
	    }
	    if (TclIsVarDirectReadable(var2Ptr)) {
		valuePtr = var2Ptr->value.objPtr;
	    } else {
		DECACHE_STACK_INFO();
		valuePtr = TclPtrGetVar(interp, var2Ptr, NULL, NULL, NULL, 0,
			duiPtr->varIndices[i]);
		CACHE_STACK_INFO();
	    }
	    if (valuePtr == NULL) {
		Tcl_DictObjRemove(interp, dictPtr, keyPtrPtr[i]);
	    } else if (dictPtr == valuePtr) {
		Tcl_DictObjPut(interp, dictPtr, keyPtrPtr[i],
			Tcl_DuplicateObj(valuePtr));
	    } else {
		Tcl_DictObjPut(interp, dictPtr, keyPtrPtr[i], valuePtr);
	    }
	}
	if (TclIsVarDirectWritable(varPtr)) {
	    Tcl_IncrRefCount(dictPtr);
	    TclDecrRefCount(varPtr->value.objPtr);
	    varPtr->value.objPtr = dictPtr;
	} else {
	    DECACHE_STACK_INFO();
	    objResultPtr = TclPtrSetVar(interp, varPtr, NULL, NULL, NULL,
		    dictPtr, TCL_LEAVE_ERR_MSG, opnd);
	    CACHE_STACK_INFO();
	    if (objResultPtr == NULL) {
		if (allocdict) {
		    TclDecrRefCount(dictPtr);
		}
		goto gotError;
	    }
	}
	NEXT_INST_F(9, 1, 0);

    case INST_DICT_EXPAND:
	dictPtr = OBJ_UNDER_TOS;
	listPtr = OBJ_AT_TOS;
	if (TclListObjGetElements(interp, listPtr, &objc, &objv) != TCL_OK) {
	    TRACE_WITH_OBJ(("%.30s %.30s => ERROR: ",
		    O2S(dictPtr), O2S(listPtr)), Tcl_GetObjResult(interp));
	    goto gotError;
	}
	objResultPtr = TclDictWithInit(interp, dictPtr, objc, objv);
	if (objResultPtr == NULL) {
	    TRACE_WITH_OBJ(("%.30s %.30s => ERROR: ",
		    O2S(dictPtr), O2S(listPtr)), Tcl_GetObjResult(interp));
	    goto gotError;
	}
	TRACE_APPEND(("%.30s\n", O2S(objResultPtr)));
	NEXT_INST_F(1, 2, 1);

    case INST_DICT_RECOMBINE_STK:
	keysPtr = POP_OBJECT();
	varNamePtr = OBJ_UNDER_TOS;
	listPtr = OBJ_AT_TOS;
	TRACE(("\"%.30s\" \"%.30s\" \"%.30s\" => ",
		O2S(varNamePtr), O2S(valuePtr), O2S(keysPtr)));
	if (TclListObjGetElements(interp, listPtr, &objc, &objv) != TCL_OK) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    TclDecrRefCount(keysPtr);
	    goto gotError;
	}
	varPtr = TclObjLookupVarEx(interp, varNamePtr, NULL,
		TCL_LEAVE_ERR_MSG, "set", 1, 1, &arrayPtr);
	if (varPtr == NULL) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    TclDecrRefCount(keysPtr);
	    goto gotError;
	}
	DECACHE_STACK_INFO();
	result = TclDictWithFinish(interp, varPtr,arrayPtr,varNamePtr,NULL,-1,
		objc, objv, keysPtr);
	CACHE_STACK_INFO();
	TclDecrRefCount(keysPtr);
	if (result != TCL_OK) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	TRACE_APPEND(("OK\n"));
	NEXT_INST_F(1, 2, 0);

    case INST_DICT_RECOMBINE_IMM:
	opnd = TclGetUInt4AtPtr(pc+1);
	listPtr = OBJ_UNDER_TOS;
	keysPtr = OBJ_AT_TOS;
	varPtr = LOCAL(opnd);
	TRACE(("%u <- \"%.30s\" \"%.30s\" => ", opnd, O2S(valuePtr),
		O2S(keysPtr)));
	if (TclListObjGetElements(interp, listPtr, &objc, &objv) != TCL_OK) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	while (TclIsVarLink(varPtr)) {
	    varPtr = varPtr->value.linkPtr;
	}
	DECACHE_STACK_INFO();
	result = TclDictWithFinish(interp, varPtr, NULL, NULL, NULL, opnd,
		objc, objv, keysPtr);
	CACHE_STACK_INFO();
	if (result != TCL_OK) {
	    TRACE_APPEND(("ERROR: %.30s\n", O2S(Tcl_GetObjResult(interp))));
	    goto gotError;
	}
	TRACE_APPEND(("OK\n"));
	NEXT_INST_F(5, 2, 0);
    }

    /*
     *	   End of dictionary-related instructions.
     * -----------------------------------------------------------------
     */

    default:
	Tcl_Panic("TclNRExecuteByteCode: unrecognized opCode %u", *pc);
    } /* end of switch on opCode */

    /*
     * Block for variables needed to process exception returns.
     */

    {
	ExceptionRange *rangePtr;
				/* Points to closest loop or catch exception
				 * range enclosing the pc. Used by various
				 * instructions and processCatch to process
				 * break, continue, and errors. */
	const char *bytes;

	/*
	 * An external evaluation (INST_INVOKE or INST_EVAL) returned
	 * something different from TCL_OK, or else INST_BREAK or
	 * INST_CONTINUE were called.
	 */

    processExceptionReturn:
#if TCL_COMPILE_DEBUG
	switch (*pc) {
	case INST_INVOKE_STK1:
	    opnd = TclGetUInt1AtPtr(pc+1);
	    TRACE(("%u => ... after \"%.20s\": ", opnd, cmdNameBuf));
	    break;
	case INST_INVOKE_STK4:
	    opnd = TclGetUInt4AtPtr(pc+1);
	    TRACE(("%u => ... after \"%.20s\": ", opnd, cmdNameBuf));
	    break;
	case INST_EVAL_STK:
	    /*
	     * Note that the object at stacktop has to be used before doing
	     * the cleanup.
	     */

	    TRACE(("\"%.30s\" => ", O2S(OBJ_AT_TOS)));
	    break;
	default:
	    TRACE(("=> "));
	}
#endif
	if ((result == TCL_CONTINUE) || (result == TCL_BREAK)) {
	    rangePtr = GetExceptRangeForPc(pc, /*catchOnly*/ 0, codePtr);
	    if (rangePtr == NULL) {
		TRACE_APPEND(("no encl. loop or catch, returning %s\n",
			StringForResultCode(result)));
		goto abnormalReturn;
	    }
	    if (rangePtr->type == CATCH_EXCEPTION_RANGE) {
		TRACE_APPEND(("%s ...\n", StringForResultCode(result)));
		goto processCatch;
	    }
	    while (cleanup--) {
		valuePtr = POP_OBJECT();
		TclDecrRefCount(valuePtr);
	    }
	    if (result == TCL_BREAK) {
		result = TCL_OK;
		pc = (codePtr->codeStart + rangePtr->breakOffset);
		TRACE_APPEND(("%s, range at %d, new pc %d\n",
			StringForResultCode(result),
			rangePtr->codeOffset, rangePtr->breakOffset));
		NEXT_INST_F(0, 0, 0);
	    }
	    if (rangePtr->continueOffset == -1) {
		TRACE_APPEND(("%s, loop w/o continue, checking for catch\n",
			StringForResultCode(result)));
		goto checkForCatch;
	    }
	    result = TCL_OK;
	    pc = (codePtr->codeStart + rangePtr->continueOffset);
	    TRACE_APPEND(("%s, range at %d, new pc %d\n",
		    StringForResultCode(result),
		    rangePtr->codeOffset, rangePtr->continueOffset));
	    NEXT_INST_F(0, 0, 0);
	}
#if TCL_COMPILE_DEBUG
	if (traceInstructions) {
	    objPtr = Tcl_GetObjResult(interp);
	    if ((result != TCL_ERROR) && (result != TCL_RETURN)) {
		TRACE_APPEND(("OTHER RETURN CODE %d, result= \"%s\"\n ",
			result, O2S(objPtr)));
	    } else {
		TRACE_APPEND(("%s, result= \"%s\"\n",
			StringForResultCode(result), O2S(objPtr)));
	    }
	}
#endif
	goto checkForCatch;

	/*
	 * Division by zero in an expression. Control only reaches this point
	 * by "goto divideByZero".
	 */

    divideByZero:
	DECACHE_STACK_INFO();
	Tcl_SetObjResult(interp, Tcl_NewStringObj("divide by zero",
		TCL_STRLEN));
	Tcl_SetErrorCode(interp, "ARITH", "DIVZERO", "divide by zero", NULL);
	CACHE_STACK_INFO();
	goto gotError;

	/*
	 * Exponentiation of zero by negative number in an expression. Control
	 * only reaches this point by "goto exponOfZero".
	 */

    exponOfZero:
	DECACHE_STACK_INFO();
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"exponentiation of zero by negative power", TCL_STRLEN));
	Tcl_SetErrorCode(interp, "ARITH", "DOMAIN",
		"exponentiation of zero by negative power", NULL);
	CACHE_STACK_INFO();

	/*
	 * Almost all error paths feed through here rather than assigning to
	 * result themselves (for a small but consistent saving).
	 */

    gotError:
	result = TCL_ERROR;

	/*
	 * Execution has generated an "exception" such as TCL_ERROR. If the
	 * exception is an error, record information about what was being
	 * executed when the error occurred. Find the closest enclosing catch
	 * range, if any. If no enclosing catch range is found, stop execution
	 * and return the "exception" code.
	 */

    checkForCatch:
	if (iPtr->execEnvPtr->rewind) {
	    goto abnormalReturn;
	}
	if ((result == TCL_ERROR) && !(iPtr->flags & ERR_ALREADY_LOGGED)) {
	    const unsigned char *pcBeg;

	    bytes = GetSrcInfoForPc(pc, codePtr, &length, &pcBeg);
	    DECACHE_STACK_INFO();
	    TclLogCommandInfo(interp, codePtr->source, bytes,
		    bytes ? length : 0, pcBeg, tosPtr);
	    CACHE_STACK_INFO();
	}
	iPtr->flags &= ~ERR_ALREADY_LOGGED;

	/*
	 * Clear all expansions that may have started after the last
	 * INST_BEGIN_CATCH.
	 */

	while (auxObjList) {
	    if ((catchTop != initCatchTop)
		    && (*catchTop > (ptrdiff_t)
			auxObjList->internalRep.ptrAndLongRep.value)) {
		break;
	    }
	    POP_TAUX_OBJ();
	}

	/*
	 * We must not catch if the script in progress has been canceled with
	 * the TCL_CANCEL_UNWIND flag. Instead, it blows outwards until we
	 * either hit another interpreter (presumably where the script in
	 * progress has not been canceled) or we get to the top-level. We do
	 * NOT modify the interpreter result here because we know it will
	 * already be set prior to vectoring down to this point in the code.
	 */

	if (TclCanceled(iPtr) && (Tcl_Canceled(interp, 0) == TCL_ERROR)) {
#ifdef TCL_COMPILE_DEBUG
	    if (traceInstructions) {
		fprintf(stdout, "   ... cancel with unwind, returning %s\n",
			StringForResultCode(result));
	    }
#endif
	    goto abnormalReturn;
	}

	/*
	 * We must not catch an exceeded limit. Instead, it blows outwards
	 * until we either hit another interpreter (presumably where the limit
	 * is not exceeded) or we get to the top-level.
	 */

	if (TclLimitExceeded(iPtr->limit)) {
#ifdef TCL_COMPILE_DEBUG
	    if (traceInstructions) {
		fprintf(stdout, "   ... limit exceeded, returning %s\n",
			StringForResultCode(result));
	    }
#endif
	    goto abnormalReturn;
	}
	if (catchTop == initCatchTop) {
#ifdef TCL_COMPILE_DEBUG
	    if (traceInstructions) {
		fprintf(stdout, "   ... no enclosing catch, returning %s\n",
			StringForResultCode(result));
	    }
#endif
	    goto abnormalReturn;
	}
	rangePtr = GetExceptRangeForPc(pc, /*catchOnly*/ 1, codePtr);
	if (rangePtr == NULL) {
	    /*
	     * This is only possible when compiling a [catch] that sends its
	     * script to INST_EVAL. Cannot correct the compiler without
	     * breaking compat with previous .tbc compiled scripts.
	     */

#ifdef TCL_COMPILE_DEBUG
	    if (traceInstructions) {
		fprintf(stdout, "   ... no enclosing catch, returning %s\n",
			StringForResultCode(result));
	    }
#endif
	    goto abnormalReturn;
	}

	/*
	 * A catch exception range (rangePtr) was found to handle an
	 * "exception". It was found either by checkForCatch just above or by
	 * an instruction during break, continue, or error processing. Jump to
	 * its catchOffset after unwinding the operand stack to the depth it
	 * had when starting to execute the range's catch command.
	 */

    processCatch:
	while (CURR_DEPTH > *catchTop) {
	    valuePtr = POP_OBJECT();
	    TclDecrRefCount(valuePtr);
	}
#ifdef TCL_COMPILE_DEBUG
	if (traceInstructions) {
	    fprintf(stdout, "  ... found catch at %d, catchTop=%d, "
		    "unwound to %ld, new pc %u\n",
		    rangePtr->codeOffset, (int) (catchTop - initCatchTop - 1),
		    (long) *catchTop, (unsigned) rangePtr->catchOffset);
	}
#endif
	pc = (codePtr->codeStart + rangePtr->catchOffset);
	NEXT_INST_F(0, 0, 0);	/* Restart the execution loop at pc. */

	/*
	 * end of infinite loop dispatching on instructions.
	 */

	/*
	 * Abnormal return code. Restore the stack to state it had when
	 * starting to execute the ByteCode. Panic if the stack is below the
	 * initial level.
	 */

    abnormalReturn:
	TCL_DTRACE_INST_LAST();

	/*
	 * Clear all expansions and same-level NR calls.
	 *
	 * Note that expansion markers have a NULL type; avoid removing other
	 * markers.
	 */

	while (auxObjList) {
	    POP_TAUX_OBJ();
	}
	while (tosPtr > initTosPtr) {
	    objPtr = POP_OBJECT();
	    Tcl_DecrRefCount(objPtr);
	}

	if (tosPtr < initTosPtr) {
	    fprintf(stderr,
		    "\nTclNRExecuteByteCode: abnormal return at pc %u: "
		    "stack top %d < entry stack top %d\n",
		    (unsigned)(pc - codePtr->codeStart),
		    (unsigned) CURR_DEPTH, (unsigned) 0);
	    Tcl_Panic("TclNRExecuteByteCode execution failure: end stack top < start stack top");
	}
	CLANG_ASSERT(bcFramePtr);
    }

    iPtr->cmdFramePtr = bcFramePtr->nextPtr;
    if (--codePtr->refCount <= 0) {
	TclCleanupByteCode(codePtr);
    }
    TclStackFree(interp, TD);	/* free my stack */

    return result;
}

#undef codePtr
#undef iPtr
#undef bcFramePtr
#undef initCatchTop
#undef initTosPtr
#undef auxObjList
#undef catchTop
#undef TCONST

/*
 *----------------------------------------------------------------------
 *
 * ExecuteExtendedBinaryMathOp, ExecuteExtendedUnaryMathOp --
 *
 *	These functions do advanced math for binary and unary operators
 *	respectively, so that the main TEBC code does not bear the cost of
 *	them.
 *
 * Results:
 *	A Tcl_Obj* result, or a NULL (in which case valuePtr is updated to
 *	hold the result value), or one of the special flag values
 *	GENERAL_ARITHMETIC_ERROR, EXPONENT_OF_ZERO or DIVIDED_BY_ZERO. The
 *	latter two signify a zero value raised to a negative power or a value
 *	divided by zero, respectively. With GENERAL_ARITHMETIC_ERROR, all
 *	error information will have already been reported in the interpreter
 *	result.
 *
 * Side effects:
 *	May update the Tcl_Obj indicated valuePtr if it is unshared. Will
 *	return a NULL when that happens.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
ExecuteExtendedBinaryMathOp(
    Tcl_Interp *interp,		/* Where to report errors. */
    int opcode,			/* What operation to perform. */
    Tcl_Obj **constants,	/* The execution environment's constants. */
    Tcl_Obj *valuePtr,		/* The first operand on the stack. */
    Tcl_Obj *value2Ptr)		/* The second operand on the stack. */
{
#define LONG_RESULT(l) \
    if (Tcl_IsShared(valuePtr)) {		\
	TclNewLongObj(objResultPtr, l);		\
	return objResultPtr;			\
    } else {					\
	Tcl_SetLongObj(valuePtr, l);		\
	return NULL;				\
    }
#define WIDE_RESULT(w) \
    if (Tcl_IsShared(valuePtr)) {		\
	return Tcl_NewWideIntObj(w);		\
    } else {					\
	Tcl_SetWideIntObj(valuePtr, w);		\
	return NULL;				\
    }
#define BIG_RESULT(b) \
    if (Tcl_IsShared(valuePtr)) {		\
	return Tcl_NewBignumObj(b);		\
    } else {					\
	Tcl_SetBignumObj(valuePtr, b);		\
	return NULL;				\
    }
#define DOUBLE_RESULT(d) \
    if (Tcl_IsShared(valuePtr)) {		\
	TclNewDoubleObj(objResultPtr, (d));	\
	return objResultPtr;			\
    } else {					\
	Tcl_SetDoubleObj(valuePtr, (d));	\
	return NULL;				\
    }

    int type1, type2;
    ClientData ptr1, ptr2;
    double d1, d2, dResult;
    long l1, l2, lResult;
    Tcl_WideInt w1, w2, wResult;
    mp_int big1, big2, bigResult, bigRemainder;
    Tcl_Obj *objResultPtr;
    int invalid, numPos, zero;
    long shift;

    (void) GetNumberFromObj(NULL, valuePtr, &ptr1, &type1);
    (void) GetNumberFromObj(NULL, value2Ptr, &ptr2, &type2);

    switch (opcode) {
    case INST_MOD:
	/* TODO: Attempts to re-use unshared operands on stack */

	l2 = 0;			/* silence gcc warning */
	if (type2 == TCL_NUMBER_LONG) {
	    l2 = *((const long *)ptr2);
	    if (l2 == 0) {
		return DIVIDED_BY_ZERO;
	    }
	    if ((l2 == 1) || (l2 == -1)) {
		/*
		 * Div. by |1| always yields remainder of 0.
		 */

		return constants[0];
	    }
	}
#ifndef NO_WIDE_TYPE
	if (type1 == TCL_NUMBER_WIDE) {
	    w1 = *((const Tcl_WideInt *)ptr1);
	    if (type2 != TCL_NUMBER_BIG) {
		Tcl_WideInt wQuotient, wRemainder;
		Tcl_GetWideIntFromObj(NULL, value2Ptr, &w2);
		wQuotient = w1 / w2;

		/*
		 * Force Tcl's integer division rules.
		 * TODO: examine for logic simplification
		 */

		if (((wQuotient < (Tcl_WideInt) 0)
			|| ((wQuotient == (Tcl_WideInt) 0)
			&& ((w1 < (Tcl_WideInt)0 && w2 > (Tcl_WideInt)0)
			|| (w1 > (Tcl_WideInt)0 && w2 < (Tcl_WideInt)0))))
			&& (wQuotient * w2 != w1)) {
		    wQuotient -= (Tcl_WideInt) 1;
		}
		wRemainder = w1 - w2*wQuotient;
		WIDE_RESULT(wRemainder);
	    }

	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);

	    /* TODO: internals intrusion */
	    if ((w1 > ((Tcl_WideInt) 0)) ^ (big2.sign == MP_ZPOS)) {
		/*
		 * Arguments are opposite sign; remainder is sum.
		 */

		TclBNInitBignumFromWideInt(&big1, w1);
		mp_add(&big2, &big1, &big2);
		mp_clear(&big1);
		BIG_RESULT(&big2);
	    }

	    /*
	     * Arguments are same sign; remainder is first operand.
	     */

	    mp_clear(&big2);
	    return NULL;
	}
#endif
	Tcl_GetBignumFromObj(NULL, valuePtr, &big1);
	Tcl_GetBignumFromObj(NULL, value2Ptr, &big2);
	mp_init(&bigResult);
	mp_init(&bigRemainder);
	mp_div(&big1, &big2, &bigResult, &bigRemainder);
	if (!mp_iszero(&bigRemainder) && (bigRemainder.sign != big2.sign)) {
	    /*
	     * Convert to Tcl's integer division rules.
	     */

	    mp_sub_d(&bigResult, 1, &bigResult);
	    mp_add(&bigRemainder, &big2, &bigRemainder);
	}
	mp_copy(&bigRemainder, &bigResult);
	mp_clear(&bigRemainder);
	mp_clear(&big1);
	mp_clear(&big2);
	BIG_RESULT(&bigResult);

    case INST_LSHIFT:
    case INST_RSHIFT: {
	/*
	 * Reject negative shift argument.
	 */

	switch (type2) {
	case TCL_NUMBER_LONG:
	    invalid = (*((const long *)ptr2) < 0L);
	    break;
#ifndef NO_WIDE_TYPE
	case TCL_NUMBER_WIDE:
	    invalid = (*((const Tcl_WideInt *)ptr2) < (Tcl_WideInt)0);
	    break;
#endif
	case TCL_NUMBER_BIG:
	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	    invalid = (mp_cmp_d(&big2, 0) == MP_LT);
	    mp_clear(&big2);
	    break;
	default:
	    /* Unused, here to silence compiler warning */
	    invalid = 0;
	}
	if (invalid) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "negative shift argument", TCL_STRLEN));
	    return GENERAL_ARITHMETIC_ERROR;
	}

	/*
	 * Zero shifted any number of bits is still zero.
	 */

	if ((type1==TCL_NUMBER_LONG) && (*((const long *)ptr1) == (long)0)) {
	    return constants[0];
	}

	if (opcode == INST_LSHIFT) {
	    /*
	     * Large left shifts create integer overflow.
	     *
	     * BEWARE! Can't use Tcl_GetIntFromObj() here because that
	     * converts values in the (unsigned) range to their signed int
	     * counterparts, leading to incorrect results.
	     */

	    if ((type2 != TCL_NUMBER_LONG)
		    || (*((const long *)ptr2) > (long) INT_MAX)) {
		/*
		 * Technically, we could hold the value (1 << (INT_MAX+1)) in
		 * an mp_int, but since we're using mp_mul_2d() to do the
		 * work, and it takes only an int argument, that's a good
		 * place to draw the line.
		 */

		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"integer value too large to represent", TCL_STRLEN));
		return GENERAL_ARITHMETIC_ERROR;
	    }
	    shift = (int)(*((const long *)ptr2));

	    /*
	     * Handle shifts within the native wide range.
	     */

	    if ((type1 != TCL_NUMBER_BIG)
		    && ((size_t)shift < CHAR_BIT*sizeof(Tcl_WideInt))) {
		TclGetWideIntFromObj(NULL, valuePtr, &w1);
		if (!((w1>0 ? w1 : ~w1)
			& -(((Tcl_WideInt)1)
			<< (CHAR_BIT*sizeof(Tcl_WideInt) - 1 - shift)))) {
		    WIDE_RESULT(w1 << shift);
		}
	    }
	} else {
	    /*
	     * Quickly force large right shifts to 0 or -1.
	     */

	    if ((type2 != TCL_NUMBER_LONG)
		    || (*(const long *)ptr2 > INT_MAX)) {
		/*
		 * Again, technically, the value to be shifted could be an
		 * mp_int so huge that a right shift by (INT_MAX+1) bits could
		 * not take us to the result of 0 or -1, but since we're using
		 * mp_div_2d to do the work, and it takes only an int
		 * argument, we draw the line there.
		 */

		switch (type1) {
		case TCL_NUMBER_LONG:
		    zero = (*(const long *)ptr1 > 0L);
		    break;
#ifndef NO_WIDE_TYPE
		case TCL_NUMBER_WIDE:
		    zero = (*(const Tcl_WideInt *)ptr1 > (Tcl_WideInt)0);
		    break;
#endif
		case TCL_NUMBER_BIG:
		    Tcl_TakeBignumFromObj(NULL, valuePtr, &big1);
		    zero = (mp_cmp_d(&big1, 0) == MP_GT);
		    mp_clear(&big1);
		    break;
		default:
		    /* Unused, here to silence compiler warning. */
		    zero = 0;
		}
		if (zero) {
		    return constants[0];
		}
		LONG_RESULT(-1);
	    }
	    shift = (int)(*(const long *)ptr2);

#ifndef NO_WIDE_TYPE
	    /*
	     * Handle shifts within the native wide range.
	     */

	    if (type1 == TCL_NUMBER_WIDE) {
		w1 = *(const Tcl_WideInt *)ptr1;
		if ((size_t)shift >= CHAR_BIT*sizeof(Tcl_WideInt)) {
		    if (w1 >= (Tcl_WideInt)0) {
			return constants[0];
		    }
		    LONG_RESULT(-1);
		}
		WIDE_RESULT(w1 >> shift);
	    }
#endif
	}

	Tcl_TakeBignumFromObj(NULL, valuePtr, &big1);

	mp_init(&bigResult);
	if (opcode == INST_LSHIFT) {
	    mp_mul_2d(&big1, shift, &bigResult);
	} else {
	    mp_init(&bigRemainder);
	    mp_div_2d(&big1, shift, &bigResult, &bigRemainder);
	    if (mp_cmp_d(&bigRemainder, 0) == MP_LT) {
		/*
		 * Convert to Tcl's integer division rules.
		 */

		mp_sub_d(&bigResult, 1, &bigResult);
	    }
	    mp_clear(&bigRemainder);
	}
	mp_clear(&big1);
	BIG_RESULT(&bigResult);
    }

    case INST_BITOR:
    case INST_BITXOR:
    case INST_BITAND:
	if ((type1 == TCL_NUMBER_BIG) || (type2 == TCL_NUMBER_BIG)) {
	    mp_int *First, *Second;

	    Tcl_TakeBignumFromObj(NULL, valuePtr, &big1);
	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);

	    /*
	     * Count how many positive arguments we have. If only one of the
	     * arguments is negative, store it in 'Second'.
	     */

	    if (mp_cmp_d(&big1, 0) != MP_LT) {
		numPos = 1 + (mp_cmp_d(&big2, 0) != MP_LT);
		First = &big1;
		Second = &big2;
	    } else {
		First = &big2;
		Second = &big1;
		numPos = (mp_cmp_d(First, 0) != MP_LT);
	    }
	    mp_init(&bigResult);

	    switch (opcode) {
	    case INST_BITAND:
		switch (numPos) {
		case 2:
		    /*
		     * Both arguments positive, base case.
		     */

		    mp_and(First, Second, &bigResult);
		    break;
		case 1:
		    /*
		     * First is positive; second negative:
		     * P & N = P & ~~N = P&~(-N-1) = P & (P ^ (-N-1))
		     */

		    mp_neg(Second, Second);
		    mp_sub_d(Second, 1, Second);
		    mp_xor(First, Second, &bigResult);
		    mp_and(First, &bigResult, &bigResult);
		    break;
		case 0:
		    /*
		     * Both arguments negative:
		     * a & b = ~ (~a | ~b) = -(-a-1|-b-1)-1
		     */

		    mp_neg(First, First);
		    mp_sub_d(First, 1, First);
		    mp_neg(Second, Second);
		    mp_sub_d(Second, 1, Second);
		    mp_or(First, Second, &bigResult);
		    mp_neg(&bigResult, &bigResult);
		    mp_sub_d(&bigResult, 1, &bigResult);
		    break;
		}
		break;

	    case INST_BITOR:
		switch (numPos) {
		case 2:
		    /*
		     * Both arguments positive, base case.
		     */

		    mp_or(First, Second, &bigResult);
		    break;
		case 1:
		    /*
		     * First is positive; second negative:
		     * N|P = ~(~N&~P) = ~((-N-1)&~P) = -((-N-1)&((-N-1)^P))-1
		     */

		    mp_neg(Second, Second);
		    mp_sub_d(Second, 1, Second);
		    mp_xor(First, Second, &bigResult);
		    mp_and(Second, &bigResult, &bigResult);
		    mp_neg(&bigResult, &bigResult);
		    mp_sub_d(&bigResult, 1, &bigResult);
		    break;
		case 0:
		    /*
		     * Both arguments negative:
		     * a | b = ~ (~a & ~b) = -(-a-1&-b-1)-1
		     */

		    mp_neg(First, First);
		    mp_sub_d(First, 1, First);
		    mp_neg(Second, Second);
		    mp_sub_d(Second, 1, Second);
		    mp_and(First, Second, &bigResult);
		    mp_neg(&bigResult, &bigResult);
		    mp_sub_d(&bigResult, 1, &bigResult);
		    break;
		}
		break;

	    case INST_BITXOR:
		switch (numPos) {
		case 2:
		    /*
		     * Both arguments positive, base case.
		     */

		    mp_xor(First, Second, &bigResult);
		    break;
		case 1:
		    /*
		     * First is positive; second negative:
		     * P^N = ~(P^~N) = -(P^(-N-1))-1
		     */

		    mp_neg(Second, Second);
		    mp_sub_d(Second, 1, Second);
		    mp_xor(First, Second, &bigResult);
		    mp_neg(&bigResult, &bigResult);
		    mp_sub_d(&bigResult, 1, &bigResult);
		    break;
		case 0:
		    /*
		     * Both arguments negative:
		     * a ^ b = (~a ^ ~b) = (-a-1^-b-1)
		     */

		    mp_neg(First, First);
		    mp_sub_d(First, 1, First);
		    mp_neg(Second, Second);
		    mp_sub_d(Second, 1, Second);
		    mp_xor(First, Second, &bigResult);
		    break;
		}
		break;
	    }

	    mp_clear(&big1);
	    mp_clear(&big2);
	    BIG_RESULT(&bigResult);
	}

#ifndef NO_WIDE_TYPE
	if ((type1 == TCL_NUMBER_WIDE) || (type2 == TCL_NUMBER_WIDE)) {
	    TclGetWideIntFromObj(NULL, valuePtr, &w1);
	    TclGetWideIntFromObj(NULL, value2Ptr, &w2);

	    switch (opcode) {
	    case INST_BITAND:
		wResult = w1 & w2;
		break;
	    case INST_BITOR:
		wResult = w1 | w2;
		break;
	    case INST_BITXOR:
		wResult = w1 ^ w2;
		break;
	    default:
		/* Unused, here to silence compiler warning. */
		wResult = 0;
	    }
	    WIDE_RESULT(wResult);
	}
#endif
	l1 = *((const long *)ptr1);
	l2 = *((const long *)ptr2);

	switch (opcode) {
	case INST_BITAND:
	    lResult = l1 & l2;
	    break;
	case INST_BITOR:
	    lResult = l1 | l2;
	    break;
	case INST_BITXOR:
	    lResult = l1 ^ l2;
	    break;
	default:
	    /* Unused, here to silence compiler warning. */
	    lResult = 0;
	}
	LONG_RESULT(lResult);

    case INST_EXPON: {
	int oddExponent = 0, negativeExponent = 0;
	unsigned short base;

	if ((type1 == TCL_NUMBER_DOUBLE) || (type2 == TCL_NUMBER_DOUBLE)) {
	    Tcl_GetDoubleFromObj(NULL, valuePtr, &d1);
	    Tcl_GetDoubleFromObj(NULL, value2Ptr, &d2);

	    if (d1==0.0 && d2<0.0) {
		return EXPONENT_OF_ZERO;
	    }
	    dResult = pow(d1, d2);
	    goto doubleResult;
	}
	l1 = l2 = 0;
	if (type2 == TCL_NUMBER_LONG) {
	    l2 = *((const long *) ptr2);
	    if (l2 == 0) {
		/*
		 * Anything to the zero power is 1.
		 */

		return constants[1];
	    } else if (l2 == 1) {
		/*
		 * Anything to the first power is itself
		 */

		return NULL;
	    }
	}

	switch (type2) {
	case TCL_NUMBER_LONG:
	    negativeExponent = (l2 < 0);
	    oddExponent = (int) (l2 & 1);
	    break;
#ifndef NO_WIDE_TYPE
	case TCL_NUMBER_WIDE:
	    w2 = *((const Tcl_WideInt *)ptr2);
	    negativeExponent = (w2 < 0);
	    oddExponent = (int) (w2 & (Tcl_WideInt)1);
	    break;
#endif
	case TCL_NUMBER_BIG:
	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	    negativeExponent = (mp_cmp_d(&big2, 0) == MP_LT);
	    mp_mod_2d(&big2, 1, &big2);
	    oddExponent = !mp_iszero(&big2);
	    mp_clear(&big2);
	    break;
	}

	if (type1 == TCL_NUMBER_LONG) {
	    l1 = *((const long *)ptr1);
	}
	if (negativeExponent) {
	    if (type1 == TCL_NUMBER_LONG) {
		switch (l1) {
		case 0:
		    /*
		     * Zero to a negative power is div by zero error.
		     */

		    return EXPONENT_OF_ZERO;
		case -1:
		    if (oddExponent) {
			LONG_RESULT(-1);
		    }
		    /* fallthrough */
		case 1:
		    /*
		     * 1 to any power is 1.
		     */

		    return constants[1];
		}
	    }

	    /*
	     * Integers with magnitude greater than 1 raise to a negative
	     * power yield the answer zero (see TIP 123).
	     */

	    return constants[0];
	}

	if (type1 == TCL_NUMBER_LONG) {
	    switch (l1) {
	    case 0:
		/*
		 * Zero to a positive power is zero.
		 */

		return constants[0];
	    case 1:
		/*
		 * 1 to any power is 1.
		 */

		return constants[1];
	    case -1:
		if (!oddExponent) {
		    return constants[1];
		}
		LONG_RESULT(-1);
	    }
	}

	/*
	 * We refuse to accept exponent arguments that exceed one mp_digit
	 * which means the max exponent value is 2**28-1 = 0x0fffffff =
	 * 268435455, which fits into a signed 32 bit int which is within the
	 * range of the long int type. This means any numeric Tcl_Obj value
	 * not using TCL_NUMBER_LONG type must hold a value larger than we
	 * accept.
	 */

	if (type2 != TCL_NUMBER_LONG) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "exponent too large", TCL_STRLEN));
	    return GENERAL_ARITHMETIC_ERROR;
	}

	if (type1 == TCL_NUMBER_LONG) {
	    if (l1 == 2) {
		/*
		 * Reduce small powers of 2 to shifts.
		 */

		if ((unsigned long) l2 < CHAR_BIT * sizeof(long) - 1) {
		    LONG_RESULT(1L << l2);
		}
#if !defined(TCL_WIDE_INT_IS_LONG)
		if ((unsigned long)l2 < CHAR_BIT*sizeof(Tcl_WideInt) - 1) {
		    WIDE_RESULT(((Tcl_WideInt) 1) << l2);
		}
#endif
		goto overflowExpon;
	    }
	    if (l1 == -2) {
		int signum = oddExponent ? -1 : 1;

		/*
		 * Reduce small powers of 2 to shifts.
		 */

		if ((unsigned long) l2 < CHAR_BIT * sizeof(long) - 1) {
		    LONG_RESULT(signum * (1L << l2));
		}
#if !defined(TCL_WIDE_INT_IS_LONG)
		if ((unsigned long)l2 < CHAR_BIT*sizeof(Tcl_WideInt) - 1){
		    WIDE_RESULT(signum * (((Tcl_WideInt) 1) << l2));
		}
#endif
		goto overflowExpon;
	    }
#if (LONG_MAX == 0x7fffffff)
	    if (l2 - 2 < (long)MaxBase32Size
		    && l1 <= MaxBase32[l2 - 2]
		    && l1 >= -MaxBase32[l2 - 2]) {
		/*
		 * Small powers of 32-bit integers.
		 */

		lResult = l1 * l1;		/* b**2 */
		switch (l2) {
		case 2:
		    break;
		case 3:
		    lResult *= l1;		/* b**3 */
		    break;
		case 4:
		    lResult *= lResult;		/* b**4 */
		    break;
		case 5:
		    lResult *= lResult;		/* b**4 */
		    lResult *= l1;		/* b**5 */
		    break;
		case 6:
		    lResult *= l1;		/* b**3 */
		    lResult *= lResult;		/* b**6 */
		    break;
		case 7:
		    lResult *= l1;		/* b**3 */
		    lResult *= lResult;		/* b**6 */
		    lResult *= l1;		/* b**7 */
		    break;
		case 8:
		    lResult *= lResult;		/* b**4 */
		    lResult *= lResult;		/* b**8 */
		    break;
		}
		LONG_RESULT(lResult);
	    }

	    if (l1 - 3 >= 0 && l1 -2 < (long)Exp32IndexSize
		    && l2 - 2 < (long)(Exp32ValueSize + MaxBase32Size)) {
		base = Exp32Index[l1 - 3]
			+ (unsigned short) (l2 - 2 - MaxBase32Size);
		if (base < Exp32Index[l1 - 2]) {
		    /*
		     * 32-bit number raised to intermediate power, done by
		     * table lookup.
		     */

		    LONG_RESULT(Exp32Value[base]);
		}
	    }
	    if (-l1 - 3 >= 0 && -l1 - 2 < (long)Exp32IndexSize
		    && l2 - 2 < (long)(Exp32ValueSize + MaxBase32Size)) {
		base = Exp32Index[-l1 - 3]
			+ (unsigned short) (l2 - 2 - MaxBase32Size);
		if (base < Exp32Index[-l1 - 2]) {
		    /*
		     * 32-bit number raised to intermediate power, done by
		     * table lookup.
		     */

		    lResult = (oddExponent) ?
			    -Exp32Value[base] : Exp32Value[base];
		    LONG_RESULT(lResult);
		}
	    }
#endif
	}
#if (LONG_MAX > 0x7fffffff) || !defined(TCL_WIDE_INT_IS_LONG)
	if (type1 == TCL_NUMBER_LONG) {
	    w1 = l1;
#ifndef NO_WIDE_TYPE
	} else if (type1 == TCL_NUMBER_WIDE) {
	    w1 = *((const Tcl_WideInt *) ptr1);
#endif
	} else {
	    goto overflowExpon;
	}
	if (l2 - 2 < (long)MaxBase64Size
		&& w1 <=  MaxBase64[l2 - 2]
		&& w1 >= -MaxBase64[l2 - 2]) {
	    /*
	     * Small powers of integers whose result is wide.
	     */

	    wResult = w1 * w1;		/* b**2 */
	    switch (l2) {
	    case 2:
		break;
	    case 3:
		wResult *= l1;		/* b**3 */
		break;
	    case 4:
		wResult *= wResult;	/* b**4 */
		break;
	    case 5:
		wResult *= wResult;	/* b**4 */
		wResult *= w1;		/* b**5 */
		break;
	    case 6:
		wResult *= w1;		/* b**3 */
		wResult *= wResult;	/* b**6 */
		break;
	    case 7:
		wResult *= w1;		/* b**3 */
		wResult *= wResult;	/* b**6 */
		wResult *= w1;		/* b**7 */
		break;
	    case 8:
		wResult *= wResult;	/* b**4 */
		wResult *= wResult;	/* b**8 */
		break;
	    case 9:
		wResult *= wResult;	/* b**4 */
		wResult *= wResult;	/* b**8 */
		wResult *= w1;		/* b**9 */
		break;
	    case 10:
		wResult *= wResult;	/* b**4 */
		wResult *= w1;		/* b**5 */
		wResult *= wResult;	/* b**10 */
		break;
	    case 11:
		wResult *= wResult;	/* b**4 */
		wResult *= w1;		/* b**5 */
		wResult *= wResult;	/* b**10 */
		wResult *= w1;		/* b**11 */
		break;
	    case 12:
		wResult *= w1;		/* b**3 */
		wResult *= wResult;	/* b**6 */
		wResult *= wResult;	/* b**12 */
		break;
	    case 13:
		wResult *= w1;		/* b**3 */
		wResult *= wResult;	/* b**6 */
		wResult *= wResult;	/* b**12 */
		wResult *= w1;		/* b**13 */
		break;
	    case 14:
		wResult *= w1;		/* b**3 */
		wResult *= wResult;	/* b**6 */
		wResult *= w1;		/* b**7 */
		wResult *= wResult;	/* b**14 */
		break;
	    case 15:
		wResult *= w1;		/* b**3 */
		wResult *= wResult;	/* b**6 */
		wResult *= w1;		/* b**7 */
		wResult *= wResult;	/* b**14 */
		wResult *= w1;		/* b**15 */
		break;
	    case 16:
		wResult *= wResult;	/* b**4 */
		wResult *= wResult;	/* b**8 */
		wResult *= wResult;	/* b**16 */
		break;
	    }
	    WIDE_RESULT(wResult);
	}

	/*
	 * Handle cases of powers > 16 that still fit in a 64-bit word by
	 * doing table lookup.
	 */

	if (w1 - 3 >= 0 && w1 - 2 < (long)Exp64IndexSize
		&& l2 - 2 < (long)(Exp64ValueSize + MaxBase64Size)) {
	    base = Exp64Index[w1 - 3]
		    + (unsigned short) (l2 - 2 - MaxBase64Size);
	    if (base < Exp64Index[w1 - 2]) {
		/*
		 * 64-bit number raised to intermediate power, done by
		 * table lookup.
		 */

		WIDE_RESULT(Exp64Value[base]);
	    }
	}

	if (-w1 - 3 >= 0 && -w1 - 2 < (long)Exp64IndexSize
		&& l2 - 2 < (long)(Exp64ValueSize + MaxBase64Size)) {
	    base = Exp64Index[-w1 - 3]
		    + (unsigned short) (l2 - 2 - MaxBase64Size);
	    if (base < Exp64Index[-w1 - 2]) {
		/*
		 * 64-bit number raised to intermediate power, done by
		 * table lookup.
		 */

		wResult = oddExponent ? -Exp64Value[base] : Exp64Value[base];
		WIDE_RESULT(wResult);
	    }
	}
#endif

    overflowExpon:
	Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	if (big2.used > 1) {
	    mp_clear(&big2);
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "exponent too large", TCL_STRLEN));
	    return GENERAL_ARITHMETIC_ERROR;
	}
	Tcl_TakeBignumFromObj(NULL, valuePtr, &big1);
	mp_init(&bigResult);
	mp_expt_d(&big1, big2.dp[0], &bigResult);
	mp_clear(&big1);
	mp_clear(&big2);
	BIG_RESULT(&bigResult);
    }

    case INST_ADD:
    case INST_SUB:
    case INST_MULT:
    case INST_DIV:
	if ((type1 == TCL_NUMBER_DOUBLE) || (type2 == TCL_NUMBER_DOUBLE)) {
	    /*
	     * At least one of the values is floating-point, so perform
	     * floating point calculations.
	     */

	    Tcl_GetDoubleFromObj(NULL, valuePtr, &d1);
	    Tcl_GetDoubleFromObj(NULL, value2Ptr, &d2);

	    switch (opcode) {
	    case INST_ADD:
		dResult = d1 + d2;
		break;
	    case INST_SUB:
		dResult = d1 - d2;
		break;
	    case INST_MULT:
		dResult = d1 * d2;
		break;
	    case INST_DIV:
#ifndef IEEE_FLOATING_POINT
		if (d2 == 0.0) {
		    return DIVIDED_BY_ZERO;
		}
#endif
		/*
		 * We presume that we are running with zero-divide unmasked if
		 * we're on an IEEE box. Otherwise, this statement might cause
		 * demons to fly out our noses.
		 */

		dResult = d1 / d2;
		break;
	    default:
		/* Unused, here to silence compiler warning. */
		dResult = 0;
	    }

	doubleResult:
#ifndef ACCEPT_NAN
	    /*
	     * Check now for IEEE floating-point error.
	     */

	    if (TclIsNaN(dResult)) {
		TclExprFloatError(interp, dResult);
		return GENERAL_ARITHMETIC_ERROR;
	    }
#endif
	    DOUBLE_RESULT(dResult);
	}
	if ((type1 != TCL_NUMBER_BIG) && (type2 != TCL_NUMBER_BIG)) {
	    TclGetWideIntFromObj(NULL, valuePtr, &w1);
	    TclGetWideIntFromObj(NULL, value2Ptr, &w2);

	    switch (opcode) {
	    case INST_ADD:
		wResult = w1 + w2;
#ifndef NO_WIDE_TYPE
		if ((type1 == TCL_NUMBER_WIDE) || (type2 == TCL_NUMBER_WIDE))
#endif
		{
		    /*
		     * Check for overflow.
		     */

		    if (Overflowing(w1, w2, wResult)) {
			goto overflowBasic;
		    }
		}
		break;

	    case INST_SUB:
		wResult = w1 - w2;
#ifndef NO_WIDE_TYPE
		if ((type1 == TCL_NUMBER_WIDE) || (type2 == TCL_NUMBER_WIDE))
#endif
		{
		    /*
		     * Must check for overflow. The macro tests for overflows
		     * in sums by looking at the sign bits. As we have a
		     * subtraction here, we are adding -w2. As -w2 could in
		     * turn overflow, we test with ~w2 instead: it has the
		     * opposite sign bit to w2 so it does the job. Note that
		     * the only "bad" case (w2==0) is irrelevant for this
		     * macro, as in that case w1 and wResult have the same
		     * sign and there is no overflow anyway.
		     */

		    if (Overflowing(w1, ~w2, wResult)) {
			goto overflowBasic;
		    }
		}
		break;

	    case INST_MULT:
		if ((type1 != TCL_NUMBER_LONG) || (type2 != TCL_NUMBER_LONG)
			|| (sizeof(Tcl_WideInt) < 2*sizeof(long))) {
		    goto overflowBasic;
		}
		wResult = w1 * w2;
		break;

	    case INST_DIV:
		if (w2 == 0) {
		    return DIVIDED_BY_ZERO;
		}

		/*
		 * Need a bignum to represent (LLONG_MIN / -1)
		 */

		if ((w1 == LLONG_MIN) && (w2 == -1)) {
		    goto overflowBasic;
		}
		wResult = w1 / w2;

		/*
		 * Force Tcl's integer division rules.
		 * TODO: examine for logic simplification
		 */

		if (((wResult < 0) || ((wResult == 0) &&
			((w1 < 0 && w2 > 0) || (w1 > 0 && w2 < 0)))) &&
			(wResult*w2 != w1)) {
		    wResult -= 1;
		}
		break;

	    default:
		/*
		 * Unused, here to silence compiler warning.
		 */

		wResult = 0;
	    }

	    WIDE_RESULT(wResult);
	}

    overflowBasic:
	Tcl_TakeBignumFromObj(NULL, valuePtr, &big1);
	Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	mp_init(&bigResult);
	switch (opcode) {
	case INST_ADD:
	    mp_add(&big1, &big2, &bigResult);
	    break;
	case INST_SUB:
	    mp_sub(&big1, &big2, &bigResult);
	    break;
	case INST_MULT:
	    mp_mul(&big1, &big2, &bigResult);
	    break;
	case INST_DIV:
	    if (mp_iszero(&big2)) {
		mp_clear(&big1);
		mp_clear(&big2);
		mp_clear(&bigResult);
		return DIVIDED_BY_ZERO;
	    }
	    mp_init(&bigRemainder);
	    mp_div(&big1, &big2, &bigResult, &bigRemainder);
	    /* TODO: internals intrusion */
	    if (!mp_iszero(&bigRemainder)
		    && (bigRemainder.sign != big2.sign)) {
		/*
		 * Convert to Tcl's integer division rules.
		 */

		mp_sub_d(&bigResult, 1, &bigResult);
		mp_add(&bigRemainder, &big2, &bigRemainder);
	    }
	    mp_clear(&bigRemainder);
	    break;
	}
	mp_clear(&big1);
	mp_clear(&big2);
	BIG_RESULT(&bigResult);
    }

    Tcl_Panic("unexpected opcode");
    return NULL;
}

static Tcl_Obj *
ExecuteExtendedUnaryMathOp(
    int opcode,			/* What operation to perform. */
    Tcl_Obj *valuePtr)		/* The operand on the stack. */
{
    ClientData ptr;
    int type;
    Tcl_WideInt w;
    mp_int big;
    Tcl_Obj *objResultPtr;

    (void) GetNumberFromObj(NULL, valuePtr, &ptr, &type);

    switch (opcode) {
    case INST_BITNOT:
#ifndef NO_WIDE_TYPE
	if (type == TCL_NUMBER_WIDE) {
	    w = *((const Tcl_WideInt *) ptr);
	    WIDE_RESULT(~w);
	}
#endif
	Tcl_TakeBignumFromObj(NULL, valuePtr, &big);
	/* ~a = - a - 1 */
	mp_neg(&big, &big);
	mp_sub_d(&big, 1, &big);
	BIG_RESULT(&big);
    case INST_UMINUS:
	switch (type) {
	case TCL_NUMBER_DOUBLE:
	    DOUBLE_RESULT(-(*((const double *) ptr)));
	case TCL_NUMBER_LONG:
	    w = (Tcl_WideInt) (*((const long *) ptr));
	    if (w != LLONG_MIN) {
		WIDE_RESULT(-w);
	    }
	    TclBNInitBignumFromLong(&big, *(const long *) ptr);
	    break;
#ifndef NO_WIDE_TYPE
	case TCL_NUMBER_WIDE:
	    w = *((const Tcl_WideInt *) ptr);
	    if (w != LLONG_MIN) {
		WIDE_RESULT(-w);
	    }
	    TclBNInitBignumFromWideInt(&big, w);
	    break;
#endif
	default:
	    Tcl_TakeBignumFromObj(NULL, valuePtr, &big);
	}
	mp_neg(&big, &big);
	BIG_RESULT(&big);
    }

    Tcl_Panic("unexpected opcode");
    return NULL;
}
#undef LONG_RESULT
#undef WIDE_RESULT
#undef BIG_RESULT
#undef DOUBLE_RESULT

/*
 *----------------------------------------------------------------------
 *
 * CompareTwoNumbers --
 *
 *	This function compares a pair of numbers in Tcl_Objs. Each argument
 *	must already be known to be numeric and not NaN.
 *
 * Results:
 *	One of MP_LT, MP_EQ or MP_GT, depending on whether valuePtr is less
 *	than, equal to, or greater than value2Ptr (respectively).
 *
 * Side effects:
 *	None, provided both values are numeric.
 *
 *----------------------------------------------------------------------
 */

int
TclCompareTwoNumbers(
    Tcl_Obj *valuePtr,
    Tcl_Obj *value2Ptr)
{
    int type1, type2, compare;
    ClientData ptr1, ptr2;
    mp_int big1, big2;
    double d1, d2, tmp;
    long l1, l2;
#ifndef NO_WIDE_TYPE
    Tcl_WideInt w1, w2;
#endif

    (void) GetNumberFromObj(NULL, valuePtr, &ptr1, &type1);
    (void) GetNumberFromObj(NULL, value2Ptr, &ptr2, &type2);

    switch (type1) {
    case TCL_NUMBER_LONG:
	l1 = *((const long *)ptr1);
	switch (type2) {
	case TCL_NUMBER_LONG:
	    l2 = *((const long *)ptr2);
	longCompare:
	    return (l1 < l2) ? MP_LT : ((l1 > l2) ? MP_GT : MP_EQ);
#ifndef NO_WIDE_TYPE
	case TCL_NUMBER_WIDE:
	    w2 = *((const Tcl_WideInt *)ptr2);
	    w1 = (Tcl_WideInt)l1;
	    goto wideCompare;
#endif
	case TCL_NUMBER_DOUBLE:
	    d2 = *((const double *)ptr2);
	    d1 = (double) l1;

	    /*
	     * If the double has a fractional part, or if the long can be
	     * converted to double without loss of precision, then compare as
	     * doubles.
	     */

	    if (DBL_MANT_DIG > CHAR_BIT*sizeof(long) || l1 == (long) d1
		    || modf(d2, &tmp) != 0.0) {
		goto doubleCompare;
	    }

	    /*
	     * Otherwise, to make comparision based on full precision, need to
	     * convert the double to a suitably sized integer.
	     *
	     * Need this to get comparsions like
	     *	  expr 20000000000000003 < 20000000000000004.0
	     * right. Converting the first argument to double will yield two
	     * double values that are equivalent within double precision.
	     * Converting the double to an integer gets done exactly, then
	     * integer comparison can tell the difference.
	     */

	    if (d2 < (double)LONG_MIN) {
		return MP_GT;
	    }
	    if (d2 > (double)LONG_MAX) {
		return MP_LT;
	    }
	    l2 = (long) d2;
	    goto longCompare;
	case TCL_NUMBER_BIG:
	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	    if (mp_cmp_d(&big2, 0) == MP_LT) {
		compare = MP_GT;
	    } else {
		compare = MP_LT;
	    }
	    mp_clear(&big2);
	    return compare;
	}

#ifndef NO_WIDE_TYPE
    case TCL_NUMBER_WIDE:
	w1 = *((const Tcl_WideInt *)ptr1);
	switch (type2) {
	case TCL_NUMBER_WIDE:
	    w2 = *((const Tcl_WideInt *)ptr2);
	wideCompare:
	    return (w1 < w2) ? MP_LT : ((w1 > w2) ? MP_GT : MP_EQ);
	case TCL_NUMBER_LONG:
	    l2 = *((const long *)ptr2);
	    w2 = (Tcl_WideInt)l2;
	    goto wideCompare;
	case TCL_NUMBER_DOUBLE:
	    d2 = *((const double *)ptr2);
	    d1 = (double) w1;
	    if (DBL_MANT_DIG > CHAR_BIT*sizeof(Tcl_WideInt)
		    || w1 == (Tcl_WideInt) d1 || modf(d2, &tmp) != 0.0) {
		goto doubleCompare;
	    }
	    if (d2 < (double)LLONG_MIN) {
		return MP_GT;
	    }
	    if (d2 > (double)LLONG_MAX) {
		return MP_LT;
	    }
	    w2 = (Tcl_WideInt) d2;
	    goto wideCompare;
	case TCL_NUMBER_BIG:
	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	    if (mp_cmp_d(&big2, 0) == MP_LT) {
		compare = MP_GT;
	    } else {
		compare = MP_LT;
	    }
	    mp_clear(&big2);
	    return compare;
	}
#endif

    case TCL_NUMBER_DOUBLE:
	d1 = *((const double *)ptr1);
	switch (type2) {
	case TCL_NUMBER_DOUBLE:
	    d2 = *((const double *)ptr2);
	doubleCompare:
	    return (d1 < d2) ? MP_LT : ((d1 > d2) ? MP_GT : MP_EQ);
	case TCL_NUMBER_LONG:
	    l2 = *((const long *)ptr2);
	    d2 = (double) l2;
	    if (DBL_MANT_DIG > CHAR_BIT*sizeof(long) || l2 == (long) d2
		    || modf(d1, &tmp) != 0.0) {
		goto doubleCompare;
	    }
	    if (d1 < (double)LONG_MIN) {
		return MP_LT;
	    }
	    if (d1 > (double)LONG_MAX) {
		return MP_GT;
	    }
	    l1 = (long) d1;
	    goto longCompare;
#ifndef NO_WIDE_TYPE
	case TCL_NUMBER_WIDE:
	    w2 = *((const Tcl_WideInt *)ptr2);
	    d2 = (double) w2;
	    if (DBL_MANT_DIG > CHAR_BIT*sizeof(Tcl_WideInt)
		    || w2 == (Tcl_WideInt) d2 || modf(d1, &tmp) != 0.0) {
		goto doubleCompare;
	    }
	    if (d1 < (double)LLONG_MIN) {
		return MP_LT;
	    }
	    if (d1 > (double)LLONG_MAX) {
		return MP_GT;
	    }
	    w1 = (Tcl_WideInt) d1;
	    goto wideCompare;
#endif
	case TCL_NUMBER_BIG:
	    if (TclIsInfinite(d1)) {
		return (d1 > 0.0) ? MP_GT : MP_LT;
	    }
	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	    if ((d1 < (double)LONG_MAX) && (d1 > (double)LONG_MIN)) {
		if (mp_cmp_d(&big2, 0) == MP_LT) {
		    compare = MP_GT;
		} else {
		    compare = MP_LT;
		}
		mp_clear(&big2);
		return compare;
	    }
	    if (DBL_MANT_DIG > CHAR_BIT*sizeof(long)
		    && modf(d1, &tmp) != 0.0) {
		d2 = TclBignumToDouble(&big2);
		mp_clear(&big2);
		goto doubleCompare;
	    }
	    Tcl_InitBignumFromDouble(NULL, d1, &big1);
	    goto bigCompare;
	}

    case TCL_NUMBER_BIG:
	Tcl_TakeBignumFromObj(NULL, valuePtr, &big1);
	switch (type2) {
#ifndef NO_WIDE_TYPE
	case TCL_NUMBER_WIDE:
#endif
	case TCL_NUMBER_LONG:
	    compare = mp_cmp_d(&big1, 0);
	    mp_clear(&big1);
	    return compare;
	case TCL_NUMBER_DOUBLE:
	    d2 = *((const double *)ptr2);
	    if (TclIsInfinite(d2)) {
		compare = (d2 > 0.0) ? MP_LT : MP_GT;
		mp_clear(&big1);
		return compare;
	    }
	    if ((d2 < (double)LONG_MAX) && (d2 > (double)LONG_MIN)) {
		compare = mp_cmp_d(&big1, 0);
		mp_clear(&big1);
		return compare;
	    }
	    if (DBL_MANT_DIG > CHAR_BIT*sizeof(long)
		    && modf(d2, &tmp) != 0.0) {
		d1 = TclBignumToDouble(&big1);
		mp_clear(&big1);
		goto doubleCompare;
	    }
	    Tcl_InitBignumFromDouble(NULL, d2, &big2);
	    goto bigCompare;
	case TCL_NUMBER_BIG:
	    Tcl_TakeBignumFromObj(NULL, value2Ptr, &big2);
	bigCompare:
	    compare = mp_cmp(&big1, &big2);
	    mp_clear(&big1);
	    mp_clear(&big2);
	    return compare;
	}
    default:
	Tcl_Panic("unexpected number type");
	return TCL_ERROR;
    }
}

#ifdef TCL_COMPILE_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * PrintByteCodeInfo --
 *
 *	This procedure prints a summary about a bytecode object to stdout. It
 *	is called by TclNRExecuteByteCode when starting to execute the bytecode
 *	object if tclTraceExec has the value 2 or more.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
PrintByteCodeInfo(
    register ByteCode *codePtr)	/* The bytecode whose summary is printed to
				 * stdout. */
{
    Proc *procPtr = codePtr->procPtr;
    Interp *iPtr = (Interp *) *codePtr->interpHandle;

    fprintf(stdout, "\nExecuting ByteCode 0x%p, refCt %u, epoch %u, interp 0x%p (epoch %u)\n",
	    codePtr, codePtr->refCount, codePtr->compileEpoch, iPtr,
	    iPtr->compileEpoch);

    fprintf(stdout, "  Source: ");
    TclPrintSource(stdout, codePtr->source, 60);

    fprintf(stdout, "\n  Cmds %d, src %d, inst %u, litObjs %u, aux %d, stkDepth %u, code/src %.2f\n",
	    codePtr->numCommands, codePtr->numSrcBytes,
	    codePtr->numCodeBytes, codePtr->numLitObjects,
	    codePtr->numAuxDataItems, codePtr->maxStackDepth,
#ifdef TCL_COMPILE_STATS
	    codePtr->numSrcBytes?
		    ((float)codePtr->structureSize)/codePtr->numSrcBytes :
#endif
	    0.0);

#ifdef TCL_COMPILE_STATS
    fprintf(stdout, "  Code %lu = header %lu+inst %d+litObj %lu+exc %lu+aux %lu+cmdMap %d\n",
	    (unsigned long) codePtr->structureSize,
	    (unsigned long) (sizeof(ByteCode)-sizeof(size_t)-sizeof(Tcl_Time)),
	    codePtr->numCodeBytes,
	    (unsigned long) (codePtr->numLitObjects * sizeof(Tcl_Obj *)),
	    (unsigned long) (codePtr->numExceptRanges*sizeof(ExceptionRange)),
	    (unsigned long) (codePtr->numAuxDataItems * sizeof(AuxData)),
	    codePtr->numCmdLocBytes);
#endif /* TCL_COMPILE_STATS */
    if (procPtr != NULL) {
	fprintf(stdout,
		"  Proc 0x%p, refCt %d, args %d, compiled locals %d\n",
		procPtr, procPtr->refCount, procPtr->numArgs,
		procPtr->numCompiledLocals);
    }
}
#endif /* TCL_COMPILE_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * ValidatePcAndStackTop --
 *
 *	This procedure is called by TclNRExecuteByteCode when debugging to
 *	verify that the program counter and stack top are valid during
 *	execution.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Prints a message to stderr and panics if either the pc or stack top
 *	are invalid.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_COMPILE_DEBUG
static void
ValidatePcAndStackTop(
    register ByteCode *codePtr,	/* The bytecode whose summary is printed to
				 * stdout. */
    const unsigned char *pc,	/* Points to first byte of a bytecode
				 * instruction. The program counter. */
    int stackTop,		/* Current stack top. Must be between
				 * stackLowerBound and stackUpperBound
				 * (inclusive). */
    int stackLowerBound,	/* Smallest legal value for stackTop. */
    int checkStack)		/* 0 if the stack depth check should be
				 * skipped. */
{
    int stackUpperBound = stackLowerBound + codePtr->maxStackDepth;
				/* Greatest legal value for stackTop. */
    unsigned relativePc = (unsigned) (pc - codePtr->codeStart);
    unsigned long codeStart = (unsigned long) codePtr->codeStart;
    unsigned long codeEnd = (unsigned long)
	    (codePtr->codeStart + codePtr->numCodeBytes);
    unsigned char opCode = *pc;

    if (((unsigned long) pc < codeStart) || ((unsigned long) pc > codeEnd)) {
	fprintf(stderr, "\nBad instruction pc 0x%p in TclNRExecuteByteCode\n",
		pc);
	Tcl_Panic("TclNRExecuteByteCode execution failure: bad pc");
    }
    if ((unsigned) opCode > LAST_INST_OPCODE) {
	fprintf(stderr, "\nBad opcode %d at pc %u in TclNRExecuteByteCode\n",
		(unsigned) opCode, relativePc);
	Tcl_Panic("TclNRExecuteByteCode execution failure: bad opcode");
    }
    if (checkStack &&
	    ((stackTop < stackLowerBound) || (stackTop > stackUpperBound))) {
	int numChars;
	const char *cmd = GetSrcInfoForPc(pc, codePtr, &numChars, NULL);

	fprintf(stderr, "\nBad stack top %d at pc %u in TclNRExecuteByteCode (min %i, max %i)",
		stackTop, relativePc, stackLowerBound, stackUpperBound);
	if (cmd != NULL) {
	    Tcl_Obj *message;

	    TclNewLiteralStringObj(message, "\n executing ");
	    Tcl_IncrRefCount(message);
	    Tcl_AppendLimitedToObj(message, cmd, numChars, 100, NULL);
	    fprintf(stderr,"%s\n", Tcl_GetString(message));
	    Tcl_DecrRefCount(message);
	} else {
	    fprintf(stderr, "\n");
	}
	Tcl_Panic("TclNRExecuteByteCode execution failure: bad stack top");
    }
}
#endif /* TCL_COMPILE_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * IllegalExprOperandType --
 *
 *	Used by TclNRExecuteByteCode to append an error message to the interp
 *	result when an illegal operand type is detected by an expression
 *	instruction. The argument opndPtr holds the operand object in error.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An error message is appended to the interp result.
 *
 *----------------------------------------------------------------------
 */

static void
IllegalExprOperandType(
    Tcl_Interp *interp,		/* Interpreter to which error information
				 * pertains. */
    const unsigned char *pc, /* Points to the instruction being executed
				 * when the illegal type was found. */
    Tcl_Obj *opndPtr)		/* Points to the operand holding the value
				 * with the illegal type. */
{
    ClientData ptr;
    int type;
    const unsigned char opcode = *pc;
    const char *description, *operator = operatorStrings[opcode - INST_LOR];

    if (opcode == INST_EXPON) {
	operator = "**";
    }

    if (GetNumberFromObj(NULL, opndPtr, &ptr, &type) != TCL_OK) {
	description = "non-numeric string";
    } else if (type == TCL_NUMBER_NAN) {
	description = "non-numeric floating-point value";
    } else if (type == TCL_NUMBER_DOUBLE) {
	description = "floating-point value";
    } else {
	/* TODO: No caller needs this. Eliminate? */
	description = "(big) integer";
    }

    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
	    "can't use %s \"%s\" as operand of \"%s\"", description,
	    Tcl_GetString(opndPtr), operator));
    Tcl_SetErrorCode(interp, "ARITH", "DOMAIN", description, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetSrcInfoForPc, GetSrcInfoForPc, TclGetSrcInfoForCmd --
 *
 *	Given a program counter value, finds the closest command in the
 *	bytecode code unit's CmdLocation array and returns information about
 *	that command's source: a pointer to its first byte and the number of
 *	characters.
 *
 * Results:
 *	If a command is found that encloses the program counter value, a
 *	pointer to the command's source is returned and the length of the
 *	source is stored at *lengthPtr. If multiple commands resulted in code
 *	at pc, information about the closest enclosing command is returned. If
 *	no matching command is found, NULL is returned and *lengthPtr is
 *	unchanged.
 *
 * Side effects:
 *	The CmdFrame at *cfPtr is updated.
 *
 *----------------------------------------------------------------------
 */

const char *
TclGetSrcInfoForCmd(
    Interp *iPtr,
    size_t *lenPtr)
{
    CmdFrame *cfPtr = iPtr->cmdFramePtr;
    ByteCode *codePtr = (ByteCode *) cfPtr->data.tebc.codePtr;

    return GetSrcInfoForPc((unsigned char *) cfPtr->data.tebc.pc,
	    codePtr, lenPtr, NULL);
}

void
TclGetSrcInfoForPc(
    CmdFrame *cfPtr)
{
    ByteCode *codePtr = (ByteCode *) cfPtr->data.tebc.codePtr;

    if (cfPtr->cmd.str.cmd == NULL) {
	cfPtr->cmd.str.cmd = GetSrcInfoForPc(
		(unsigned char *) cfPtr->data.tebc.pc, codePtr,
		&cfPtr->cmd.str.len, NULL);
    }

    if (cfPtr->cmd.str.cmd != NULL) {
	/*
	 * We now have the command. We can get the srcOffset back and from
	 * there find the list of word locations for this command.
	 */

	ExtCmdLoc *eclPtr;
	ECL *locPtr = NULL;
	int srcOffset, i;
	Interp *iPtr = (Interp *) *codePtr->interpHandle;
	Tcl_HashEntry *hePtr =
		Tcl_FindHashEntry(iPtr->lineBCPtr, codePtr);

	if (!hePtr) {
	    return;
	}

	srcOffset = cfPtr->cmd.str.cmd - codePtr->source;
	eclPtr = Tcl_GetHashValue(hePtr);

	for (i=0; i < eclPtr->nuloc; i++) {
	    if (eclPtr->loc[i].srcOffset == srcOffset) {
		locPtr = eclPtr->loc+i;
		break;
	    }
	}
	if (locPtr == NULL) {
	    Tcl_Panic("LocSearch failure");
	}

	cfPtr->line = locPtr->line;
	cfPtr->nline = locPtr->nline;
	cfPtr->type = eclPtr->type;

	if (eclPtr->type == TCL_LOCATION_SOURCE) {
	    cfPtr->data.eval.path = eclPtr->path;
	    Tcl_IncrRefCount(cfPtr->data.eval.path);
	}

	/*
	 * Do not set cfPtr->data.eval.path NULL for non-SOURCE. Needed for
	 * cfPtr->data.tebc.codePtr.
	 */
    }
}

static const char *
GetSrcInfoForPc(
    const unsigned char *pc,	/* The program counter value for which to
				 * return the closest command's source info.
				 * This points within a bytecode instruction
				 * in codePtr's code. */
    ByteCode *codePtr,		/* The bytecode sequence in which to look up
				 * the command source for the pc. */
    size_t *lengthPtr,		/* If non-NULL, the location where the length
				 * of the command's source should be stored.
				 * If NULL, no length is stored. */
    const unsigned char **pcBeg)/* If non-NULL, the bytecode location
				 * where the current instruction starts.
				 * If NULL; no pointer is stored. */
{
    register int pcOffset = (pc - codePtr->codeStart);
    int numCmds = codePtr->numCommands;
    unsigned char *codeDeltaNext, *codeLengthNext;
    unsigned char *srcDeltaNext, *srcLengthNext;
    int codeOffset, codeLen, codeEnd, srcOffset, srcLen, delta, i;
    int bestDist = INT_MAX;	/* Distance of pc to best cmd's start pc. */
    int bestSrcOffset = -1;	/* Initialized to avoid compiler warning. */
    int bestSrcLength = -1;	/* Initialized to avoid compiler warning. */

    if ((pcOffset < 0) || (pcOffset >= codePtr->numCodeBytes)) {
	if (pcBeg != NULL) *pcBeg = NULL;
	return NULL;
    }

    /*
     * Decode the code and source offset and length for each command. The
     * closest enclosing command is the last one whose code started before
     * pcOffset.
     */

    codeDeltaNext = codePtr->codeDeltaStart;
    codeLengthNext = codePtr->codeLengthStart;
    srcDeltaNext = codePtr->srcDeltaStart;
    srcLengthNext = codePtr->srcLengthStart;
    codeOffset = srcOffset = 0;
    for (i = 0;  i < numCmds;  i++) {
	if ((unsigned) *codeDeltaNext == (unsigned) 0xFF) {
	    codeDeltaNext++;
	    delta = TclGetInt4AtPtr(codeDeltaNext);
	    codeDeltaNext += 4;
	} else {
	    delta = TclGetInt1AtPtr(codeDeltaNext);
	    codeDeltaNext++;
	}
	codeOffset += delta;

	if ((unsigned) *codeLengthNext == (unsigned) 0xFF) {
	    codeLengthNext++;
	    codeLen = TclGetInt4AtPtr(codeLengthNext);
	    codeLengthNext += 4;
	} else {
	    codeLen = TclGetInt1AtPtr(codeLengthNext);
	    codeLengthNext++;
	}
	codeEnd = (codeOffset + codeLen - 1);

	if ((unsigned) *srcDeltaNext == (unsigned) 0xFF) {
	    srcDeltaNext++;
	    delta = TclGetInt4AtPtr(srcDeltaNext);
	    srcDeltaNext += 4;
	} else {
	    delta = TclGetInt1AtPtr(srcDeltaNext);
	    srcDeltaNext++;
	}
	srcOffset += delta;

	if ((unsigned) *srcLengthNext == (unsigned) 0xFF) {
	    srcLengthNext++;
	    srcLen = TclGetInt4AtPtr(srcLengthNext);
	    srcLengthNext += 4;
	} else {
	    srcLen = TclGetInt1AtPtr(srcLengthNext);
	    srcLengthNext++;
	}

	if (codeOffset > pcOffset) {	/* Best cmd already found */
	    break;
	}
	if (pcOffset <= codeEnd) {	/* This cmd's code encloses pc */
	    int dist = (pcOffset - codeOffset);

	    if (dist <= bestDist) {
		bestDist = dist;
		bestSrcOffset = srcOffset;
		bestSrcLength = srcLen;
	    }
	}
    }

    if (pcBeg != NULL) {
	const unsigned char *curr, *prev;

	/*
	 * Walk from beginning of command or BC to pc, by complete
	 * instructions. Stop when crossing pc; keep previous.
	 */

	curr = ((bestDist == INT_MAX) ? codePtr->codeStart : pc - bestDist);
	prev = curr;
	while (curr <= pc) {
	    prev = curr;
	    curr += tclInstructionTable[*curr].numBytes;
	}
	*pcBeg = prev;
    }

    if (bestDist == INT_MAX) {
	return NULL;
    }

    if (lengthPtr != NULL) {
	*lengthPtr = bestSrcLength;
    }

    return (codePtr->source + bestSrcOffset);
}

/*
 *----------------------------------------------------------------------
 *
 * GetExceptRangeForPc --
 *
 *	Given a program counter value, return the closest enclosing
 *	ExceptionRange.
 *
 * Results:
 *	In the normal case, catchOnly is 0 (false) and this procedure returns
 *	a pointer to the most closely enclosing ExceptionRange structure
 *	regardless of whether it is a loop or catch exception range. This is
 *	appropriate when processing a TCL_BREAK or TCL_CONTINUE, which will be
 *	"handled" either by a loop exception range or a closer catch range. If
 *	catchOnly is nonzero, this procedure ignores loop exception ranges and
 *	returns a pointer to the closest catch range. If no matching
 *	ExceptionRange is found that encloses pc, a NULL is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static ExceptionRange *
GetExceptRangeForPc(
    const unsigned char *pc, /* The program counter value for which to
				 * search for a closest enclosing exception
				 * range. This points to a bytecode
				 * instruction in codePtr's code. */
    int catchOnly,		/* If 0, consider either loop or catch
				 * ExceptionRanges in search. If nonzero
				 * consider only catch ranges (and ignore any
				 * closer loop ranges). */
    ByteCode *codePtr)		/* Points to the ByteCode in which to search
				 * for the enclosing ExceptionRange. */
{
    ExceptionRange *rangeArrayPtr;
    int numRanges = codePtr->numExceptRanges;
    register ExceptionRange *rangePtr;
    int pcOffset = pc - codePtr->codeStart;
    register int start;

    if (numRanges == 0) {
	return NULL;
    }

    /*
     * This exploits peculiarities of our compiler: nested ranges are always
     * *after* their containing ranges, so that by scanning backwards we are
     * sure that the first matching range is indeed the deepest.
     */

    rangeArrayPtr = codePtr->exceptArrayPtr;
    rangePtr = rangeArrayPtr + numRanges;
    while (--rangePtr >= rangeArrayPtr) {
	start = rangePtr->codeOffset;
	if ((start <= pcOffset) &&
		(pcOffset < (start + rangePtr->numCodeBytes))) {
	    if ((!catchOnly)
		    || (rangePtr->type == CATCH_EXCEPTION_RANGE)) {
		return rangePtr;
	    }
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * GetOpcodeName --
 *
 *	This procedure is called by the TRACE and TRACE_WITH_OBJ macros used
 *	in TclNRExecuteByteCode when debugging. It returns the name of the
 *	bytecode instruction at a specified instruction pc.
 *
 * Results:
 *	A character string for the instruction.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_COMPILE_DEBUG
static const char *
GetOpcodeName(
    const unsigned char *pc)	/* Points to the instruction whose name should
				 * be returned. */
{
    unsigned char opCode = *pc;

    return tclInstructionTable[opCode].name;
}
#endif /* TCL_COMPILE_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * TclExprFloatError --
 *
 *	This procedure is called when an error occurs during a floating-point
 *	operation. It reads errno and sets interp->objResultPtr accordingly.
 *
 * Results:
 *	interp->objResultPtr is set to hold an error message.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TclExprFloatError(
    Tcl_Interp *interp,		/* Where to store error message. */
    double value)		/* Value returned after error; used to
				 * distinguish underflows from overflows. */
{
    const char *s;

    if ((errno == EDOM) || TclIsNaN(value)) {
	s = "domain error: argument not in valid range";
	Tcl_SetObjResult(interp, Tcl_NewStringObj(s, TCL_STRLEN));
	Tcl_SetErrorCode(interp, "ARITH", "DOMAIN", s, NULL);
    } else if ((errno == ERANGE) || TclIsInfinite(value)) {
	if (value == 0.0) {
	    s = "floating-point value too small to represent";
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(s, TCL_STRLEN));
	    Tcl_SetErrorCode(interp, "ARITH", "UNDERFLOW", s, NULL);
	} else {
	    s = "floating-point value too large to represent";
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(s, TCL_STRLEN));
	    Tcl_SetErrorCode(interp, "ARITH", "OVERFLOW", s, NULL);
	}
    } else {
	Tcl_Obj *objPtr = Tcl_ObjPrintf(
		"unknown floating-point error, errno = %d", errno);

	Tcl_SetErrorCode(interp, "ARITH", "UNKNOWN",
		Tcl_GetString(objPtr), NULL);
	Tcl_SetObjResult(interp, objPtr);
    }
}

#ifdef TCL_COMPILE_STATS
/*
 *----------------------------------------------------------------------
 *
 * TclLog2 --
 *
 *	Procedure used while collecting compilation statistics to determine
 *	the log base 2 of an integer.
 *
 * Results:
 *	Returns the log base 2 of the operand. If the argument is less than or
 *	equal to zero, a zero is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TclLog2(
    register int value)		/* The integer for which to compute the log
				 * base 2. */
{
    register int n = value;
    register int result = 0;

    while (n > 1) {
	n = n >> 1;
	result++;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * EvalStatsCmd --
 *
 *	Implements the "evalstats" command that prints instruction execution
 *	counts to stdout.
 *
 * Results:
 *	Standard Tcl results.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
EvalStatsCmd(
    ClientData unused,		/* Unused. */
    Tcl_Interp *interp,		/* The current interpreter. */
    int objc,			/* The number of arguments. */
    Tcl_Obj *const objv[])	/* The argument strings. */
{
    Interp *iPtr = (Interp *) interp;
    LiteralTable *globalTablePtr = &iPtr->literalTable;
    ByteCodeStats *statsPtr = &iPtr->stats;
    double totalCodeBytes, currentCodeBytes;
    double totalLiteralBytes, currentLiteralBytes;
    double objBytesIfUnshared, strBytesIfUnshared, sharingBytesSaved;
    double strBytesSharedMultX, strBytesSharedOnce;
    double numInstructions, currentHeaderBytes;
    long numCurrentByteCodes, numByteCodeLits;
    long refCountSum, literalMgmtBytes, sum;
    int numSharedMultX, numSharedOnce;
    int decadeHigh, minSizeDecade, maxSizeDecade, length, i;
    char *litTableStats;
    LiteralEntry *entryPtr;
    Tcl_Obj *objPtr;

#define Percent(a,b) ((a) * 100.0 / (b))

    objPtr = Tcl_NewObj();
    Tcl_IncrRefCount(objPtr);

    numInstructions = 0.0;
    for (i = 0;  i < 256;  i++) {
	if (statsPtr->instructionCount[i] != 0) {
	    numInstructions += statsPtr->instructionCount[i];
	}
    }

    totalLiteralBytes = sizeof(LiteralTable)
	    + iPtr->literalTable.numBuckets * sizeof(LiteralEntry *)
	    + (statsPtr->numLiteralsCreated * sizeof(LiteralEntry))
	    + (statsPtr->numLiteralsCreated * sizeof(Tcl_Obj))
	    + statsPtr->totalLitStringBytes;
    totalCodeBytes = statsPtr->totalByteCodeBytes + totalLiteralBytes;

    numCurrentByteCodes =
	    statsPtr->numCompilations - statsPtr->numByteCodesFreed;
    currentHeaderBytes = numCurrentByteCodes
	    * (sizeof(ByteCode) - sizeof(size_t) - sizeof(Tcl_Time));
    literalMgmtBytes = sizeof(LiteralTable)
	    + (iPtr->literalTable.numBuckets * sizeof(LiteralEntry *))
	    + (iPtr->literalTable.numEntries * sizeof(LiteralEntry));
    currentLiteralBytes = literalMgmtBytes
	    + iPtr->literalTable.numEntries * sizeof(Tcl_Obj)
	    + statsPtr->currentLitStringBytes;
    currentCodeBytes = statsPtr->currentByteCodeBytes + currentLiteralBytes;

    /*
     * Summary statistics, total and current source and ByteCode sizes.
     */

    Tcl_AppendPrintfToObj(objPtr, "\n----------------------------------------------------------------\n");
    Tcl_AppendPrintfToObj(objPtr,
	    "Compilation and execution statistics for interpreter %#lx\n",
	    (long int)iPtr);

    Tcl_AppendPrintfToObj(objPtr, "\nNumber ByteCodes executed\t%ld\n",
	    statsPtr->numExecutions);
    Tcl_AppendPrintfToObj(objPtr, "Number ByteCodes compiled\t%ld\n",
	    statsPtr->numCompilations);
    Tcl_AppendPrintfToObj(objPtr, "  Mean executions/compile\t%.1f\n",
	    statsPtr->numExecutions / (float)statsPtr->numCompilations);

    Tcl_AppendPrintfToObj(objPtr, "\nInstructions executed\t\t%.0f\n",
	    numInstructions);
    Tcl_AppendPrintfToObj(objPtr, "  Mean inst/compile\t\t%.0f\n",
	    numInstructions / statsPtr->numCompilations);
    Tcl_AppendPrintfToObj(objPtr, "  Mean inst/execution\t\t%.0f\n",
	    numInstructions / statsPtr->numExecutions);

    Tcl_AppendPrintfToObj(objPtr, "\nTotal ByteCodes\t\t\t%ld\n",
	    statsPtr->numCompilations);
    Tcl_AppendPrintfToObj(objPtr, "  Source bytes\t\t\t%.6g\n",
	    statsPtr->totalSrcBytes);
    Tcl_AppendPrintfToObj(objPtr, "  Code bytes\t\t\t%.6g\n",
	    totalCodeBytes);
    Tcl_AppendPrintfToObj(objPtr, "    ByteCode bytes\t\t%.6g\n",
	    statsPtr->totalByteCodeBytes);
    Tcl_AppendPrintfToObj(objPtr, "    Literal bytes\t\t%.6g\n",
	    totalLiteralBytes);
    Tcl_AppendPrintfToObj(objPtr, "      table %lu + bkts %lu + entries %lu + objects %lu + strings %.6g\n",
	    (unsigned long) sizeof(LiteralTable),
	    (unsigned long) (iPtr->literalTable.numBuckets * sizeof(LiteralEntry *)),
	    (unsigned long) (statsPtr->numLiteralsCreated * sizeof(LiteralEntry)),
	    (unsigned long) (statsPtr->numLiteralsCreated * sizeof(Tcl_Obj)),
	    statsPtr->totalLitStringBytes);
    Tcl_AppendPrintfToObj(objPtr, "  Mean code/compile\t\t%.1f\n",
	    totalCodeBytes / statsPtr->numCompilations);
    Tcl_AppendPrintfToObj(objPtr, "  Mean code/source\t\t%.1f\n",
	    totalCodeBytes / statsPtr->totalSrcBytes);

    Tcl_AppendPrintfToObj(objPtr, "\nCurrent (active) ByteCodes\t%ld\n",
	    numCurrentByteCodes);
    Tcl_AppendPrintfToObj(objPtr, "  Source bytes\t\t\t%.6g\n",
	    statsPtr->currentSrcBytes);
    Tcl_AppendPrintfToObj(objPtr, "  Code bytes\t\t\t%.6g\n",
	    currentCodeBytes);
    Tcl_AppendPrintfToObj(objPtr, "    ByteCode bytes\t\t%.6g\n",
	    statsPtr->currentByteCodeBytes);
    Tcl_AppendPrintfToObj(objPtr, "    Literal bytes\t\t%.6g\n",
	    currentLiteralBytes);
    Tcl_AppendPrintfToObj(objPtr, "      table %lu + bkts %lu + entries %lu + objects %lu + strings %.6g\n",
	    (unsigned long) sizeof(LiteralTable),
	    (unsigned long) (iPtr->literalTable.numBuckets * sizeof(LiteralEntry *)),
	    (unsigned long) (iPtr->literalTable.numEntries * sizeof(LiteralEntry)),
	    (unsigned long) (iPtr->literalTable.numEntries * sizeof(Tcl_Obj)),
	    statsPtr->currentLitStringBytes);
    Tcl_AppendPrintfToObj(objPtr, "  Mean code/source\t\t%.1f\n",
	    currentCodeBytes / statsPtr->currentSrcBytes);
    Tcl_AppendPrintfToObj(objPtr, "  Code + source bytes\t\t%.6g (%0.1f mean code/src)\n",
	    (currentCodeBytes + statsPtr->currentSrcBytes),
	    (currentCodeBytes / statsPtr->currentSrcBytes) + 1.0);

    /*
     * Tcl_IsShared statistics check
     *
     * This gives the refcount of each obj as Tcl_IsShared was called for it.
     * Shared objects must be duplicated before they can be modified.
     */

    numSharedMultX = 0;
    Tcl_AppendPrintfToObj(objPtr, "\nTcl_IsShared object check (all objects):\n");
    Tcl_AppendPrintfToObj(objPtr, "  Object had refcount <=1 (not shared)\t%ld\n",
	    tclObjsShared[1]);
    for (i = 2;  i < TCL_MAX_SHARED_OBJ_STATS;  i++) {
	Tcl_AppendPrintfToObj(objPtr, "  refcount ==%d\t\t%ld\n",
		i, tclObjsShared[i]);
	numSharedMultX += tclObjsShared[i];
    }
    Tcl_AppendPrintfToObj(objPtr, "  refcount >=%d\t\t%ld\n",
	    i, tclObjsShared[0]);
    numSharedMultX += tclObjsShared[0];
    Tcl_AppendPrintfToObj(objPtr, "  Total shared objects\t\t\t%d\n",
	    numSharedMultX);

    /*
     * Literal table statistics.
     */

    numByteCodeLits = 0;
    refCountSum = 0;
    numSharedMultX = 0;
    numSharedOnce = 0;
    objBytesIfUnshared = 0.0;
    strBytesIfUnshared = 0.0;
    strBytesSharedMultX = 0.0;
    strBytesSharedOnce = 0.0;
    for (i = 0;  i < globalTablePtr->numBuckets;  i++) {
	for (entryPtr = globalTablePtr->buckets[i];  entryPtr != NULL;
		entryPtr = entryPtr->nextPtr) {
	    if (entryPtr->objPtr->typePtr == &tclByteCodeType) {
		numByteCodeLits++;
	    }
	    (void) Tcl_GetStringFromObj(entryPtr->objPtr, &length);
	    refCountSum += entryPtr->refCount;
	    objBytesIfUnshared += (entryPtr->refCount * sizeof(Tcl_Obj));
	    strBytesIfUnshared += (entryPtr->refCount * (length+1));
	    if (entryPtr->refCount > 1) {
		numSharedMultX++;
		strBytesSharedMultX += (length+1);
	    } else {
		numSharedOnce++;
		strBytesSharedOnce += (length+1);
	    }
	}
    }
    sharingBytesSaved = (objBytesIfUnshared + strBytesIfUnshared)
	    - currentLiteralBytes;

    Tcl_AppendPrintfToObj(objPtr, "\nTotal objects (all interps)\t%ld\n",
	    tclObjsAlloced);
    Tcl_AppendPrintfToObj(objPtr, "Current objects\t\t\t%ld\n",
	    (tclObjsAlloced - tclObjsFreed));
    Tcl_AppendPrintfToObj(objPtr, "Total literal objects\t\t%ld\n",
	    statsPtr->numLiteralsCreated);

    Tcl_AppendPrintfToObj(objPtr, "\nCurrent literal objects\t\t%d (%0.1f%% of current objects)\n",
	    globalTablePtr->numEntries,
	    Percent(globalTablePtr->numEntries, tclObjsAlloced-tclObjsFreed));
    Tcl_AppendPrintfToObj(objPtr, "  ByteCode literals\t\t%ld (%0.1f%% of current literals)\n",
	    numByteCodeLits,
	    Percent(numByteCodeLits, globalTablePtr->numEntries));
    Tcl_AppendPrintfToObj(objPtr, "  Literals reused > 1x\t\t%d\n",
	    numSharedMultX);
    Tcl_AppendPrintfToObj(objPtr, "  Mean reference count\t\t%.2f\n",
	    ((double) refCountSum) / globalTablePtr->numEntries);
    Tcl_AppendPrintfToObj(objPtr, "  Mean len, str reused >1x \t%.2f\n",
	    (numSharedMultX ? strBytesSharedMultX/numSharedMultX : 0.0));
    Tcl_AppendPrintfToObj(objPtr, "  Mean len, str used 1x\t\t%.2f\n",
	    (numSharedOnce ? strBytesSharedOnce/numSharedOnce : 0.0));
    Tcl_AppendPrintfToObj(objPtr, "  Total sharing savings\t\t%.6g (%0.1f%% of bytes if no sharing)\n",
	    sharingBytesSaved,
	    Percent(sharingBytesSaved, objBytesIfUnshared+strBytesIfUnshared));
    Tcl_AppendPrintfToObj(objPtr, "    Bytes with sharing\t\t%.6g\n",
	    currentLiteralBytes);
    Tcl_AppendPrintfToObj(objPtr, "      table %lu + bkts %lu + entries %lu + objects %lu + strings %.6g\n",
	    (unsigned long) sizeof(LiteralTable),
	    (unsigned long) (iPtr->literalTable.numBuckets * sizeof(LiteralEntry *)),
	    (unsigned long) (iPtr->literalTable.numEntries * sizeof(LiteralEntry)),
	    (unsigned long) (iPtr->literalTable.numEntries * sizeof(Tcl_Obj)),
	    statsPtr->currentLitStringBytes);
    Tcl_AppendPrintfToObj(objPtr, "    Bytes if no sharing\t\t%.6g = objects %.6g + strings %.6g\n",
	    (objBytesIfUnshared + strBytesIfUnshared),
	    objBytesIfUnshared, strBytesIfUnshared);
    Tcl_AppendPrintfToObj(objPtr, "  String sharing savings \t%.6g = unshared %.6g - shared %.6g\n",
	    (strBytesIfUnshared - statsPtr->currentLitStringBytes),
	    strBytesIfUnshared, statsPtr->currentLitStringBytes);
    Tcl_AppendPrintfToObj(objPtr, "  Literal mgmt overhead\t\t%ld (%0.1f%% of bytes with sharing)\n",
	    literalMgmtBytes,
	    Percent(literalMgmtBytes, currentLiteralBytes));
    Tcl_AppendPrintfToObj(objPtr, "    table %lu + buckets %lu + entries %lu\n",
	    (unsigned long) sizeof(LiteralTable),
	    (unsigned long) (iPtr->literalTable.numBuckets * sizeof(LiteralEntry *)),
	    (unsigned long) (iPtr->literalTable.numEntries * sizeof(LiteralEntry)));

    /*
     * Breakdown of current ByteCode space requirements.
     */

    Tcl_AppendPrintfToObj(objPtr, "\nBreakdown of current ByteCode requirements:\n");
    Tcl_AppendPrintfToObj(objPtr, "                         Bytes      Pct of    Avg per\n");
    Tcl_AppendPrintfToObj(objPtr, "                                     total    ByteCode\n");
    Tcl_AppendPrintfToObj(objPtr, "Total             %12.6g     100.00%%   %8.1f\n",
	    statsPtr->currentByteCodeBytes,
	    statsPtr->currentByteCodeBytes / numCurrentByteCodes);
    Tcl_AppendPrintfToObj(objPtr, "Header            %12.6g   %8.1f%%   %8.1f\n",
	    currentHeaderBytes,
	    Percent(currentHeaderBytes, statsPtr->currentByteCodeBytes),
	    currentHeaderBytes / numCurrentByteCodes);
    Tcl_AppendPrintfToObj(objPtr, "Instructions      %12.6g   %8.1f%%   %8.1f\n",
	    statsPtr->currentInstBytes,
	    Percent(statsPtr->currentInstBytes,statsPtr->currentByteCodeBytes),
	    statsPtr->currentInstBytes / numCurrentByteCodes);
    Tcl_AppendPrintfToObj(objPtr, "Literal ptr array %12.6g   %8.1f%%   %8.1f\n",
	    statsPtr->currentLitBytes,
	    Percent(statsPtr->currentLitBytes,statsPtr->currentByteCodeBytes),
	    statsPtr->currentLitBytes / numCurrentByteCodes);
    Tcl_AppendPrintfToObj(objPtr, "Exception table   %12.6g   %8.1f%%   %8.1f\n",
	    statsPtr->currentExceptBytes,
	    Percent(statsPtr->currentExceptBytes,statsPtr->currentByteCodeBytes),
	    statsPtr->currentExceptBytes / numCurrentByteCodes);
    Tcl_AppendPrintfToObj(objPtr, "Auxiliary data    %12.6g   %8.1f%%   %8.1f\n",
	    statsPtr->currentAuxBytes,
	    Percent(statsPtr->currentAuxBytes,statsPtr->currentByteCodeBytes),
	    statsPtr->currentAuxBytes / numCurrentByteCodes);
    Tcl_AppendPrintfToObj(objPtr, "Command map       %12.6g   %8.1f%%   %8.1f\n",
	    statsPtr->currentCmdMapBytes,
	    Percent(statsPtr->currentCmdMapBytes,statsPtr->currentByteCodeBytes),
	    statsPtr->currentCmdMapBytes / numCurrentByteCodes);

    /*
     * Detailed literal statistics.
     */

    Tcl_AppendPrintfToObj(objPtr, "\nLiteral string sizes:\n");
    Tcl_AppendPrintfToObj(objPtr, "\t Up to length\t\tPercentage\n");
    maxSizeDecade = 0;
    for (i = 31;  i >= 0;  i--) {
	if (statsPtr->literalCount[i] > 0) {
	    maxSizeDecade = i;
	    break;
	}
    }
    sum = 0;
    for (i = 0;  i <= maxSizeDecade;  i++) {
	decadeHigh = (1 << (i+1)) - 1;
	sum += statsPtr->literalCount[i];
	Tcl_AppendPrintfToObj(objPtr, "\t%10d\t\t%8.0f%%\n",
		decadeHigh, Percent(sum, statsPtr->numLiteralsCreated));
    }

    litTableStats = TclLiteralStats(globalTablePtr);
    Tcl_AppendPrintfToObj(objPtr, "\nCurrent literal table statistics:\n%s\n",
	    litTableStats);
    ckfree(litTableStats);

    /*
     * Source and ByteCode size distributions.
     */

    Tcl_AppendPrintfToObj(objPtr, "\nSource sizes:\n");
    Tcl_AppendPrintfToObj(objPtr, "\t Up to size\t\tPercentage\n");
    minSizeDecade = maxSizeDecade = 0;
    for (i = 0;  i < 31;  i++) {
	if (statsPtr->srcCount[i] > 0) {
	    minSizeDecade = i;
	    break;
	}
    }
    for (i = 31;  i >= 0;  i--) {
	if (statsPtr->srcCount[i] > 0) {
	    maxSizeDecade = i;
	    break;
	}
    }
    sum = 0;
    for (i = minSizeDecade;  i <= maxSizeDecade;  i++) {
	decadeHigh = (1 << (i+1)) - 1;
	sum += statsPtr->srcCount[i];
	Tcl_AppendPrintfToObj(objPtr, "\t%10d\t\t%8.0f%%\n",
		decadeHigh, Percent(sum, statsPtr->numCompilations));
    }

    Tcl_AppendPrintfToObj(objPtr, "\nByteCode sizes:\n");
    Tcl_AppendPrintfToObj(objPtr, "\t Up to size\t\tPercentage\n");
    minSizeDecade = maxSizeDecade = 0;
    for (i = 0;  i < 31;  i++) {
	if (statsPtr->byteCodeCount[i] > 0) {
	    minSizeDecade = i;
	    break;
	}
    }
    for (i = 31;  i >= 0;  i--) {
	if (statsPtr->byteCodeCount[i] > 0) {
	    maxSizeDecade = i;
	    break;
	}
    }
    sum = 0;
    for (i = minSizeDecade;  i <= maxSizeDecade;  i++) {
	decadeHigh = (1 << (i+1)) - 1;
	sum += statsPtr->byteCodeCount[i];
	Tcl_AppendPrintfToObj(objPtr, "\t%10d\t\t%8.0f%%\n",
		decadeHigh, Percent(sum, statsPtr->numCompilations));
    }

    Tcl_AppendPrintfToObj(objPtr, "\nByteCode longevity (excludes Current ByteCodes):\n");
    Tcl_AppendPrintfToObj(objPtr, "\t       Up to ms\t\tPercentage\n");
    minSizeDecade = maxSizeDecade = 0;
    for (i = 0;  i < 31;  i++) {
	if (statsPtr->lifetimeCount[i] > 0) {
	    minSizeDecade = i;
	    break;
	}
    }
    for (i = 31;  i >= 0;  i--) {
	if (statsPtr->lifetimeCount[i] > 0) {
	    maxSizeDecade = i;
	    break;
	}
    }
    sum = 0;
    for (i = minSizeDecade;  i <= maxSizeDecade;  i++) {
	decadeHigh = (1 << (i+1)) - 1;
	sum += statsPtr->lifetimeCount[i];
	Tcl_AppendPrintfToObj(objPtr, "\t%12.3f\t\t%8.0f%%\n",
		decadeHigh/1000.0, Percent(sum, statsPtr->numByteCodesFreed));
    }

    /*
     * Instruction counts.
     */

    Tcl_AppendPrintfToObj(objPtr, "\nInstruction counts:\n");
    for (i = 0;  i <= LAST_INST_OPCODE;  i++) {
	Tcl_AppendPrintfToObj(objPtr, "%20s %8ld ",
		tclInstructionTable[i].name, statsPtr->instructionCount[i]);
	if (statsPtr->instructionCount[i]) {
	    Tcl_AppendPrintfToObj(objPtr, "%6.1f%%\n",
		    Percent(statsPtr->instructionCount[i], numInstructions));
	} else {
	    Tcl_AppendPrintfToObj(objPtr, "0\n");
	}
    }

#ifdef TCL_MEM_DEBUG
    Tcl_AppendPrintfToObj(objPtr, "\nHeap Statistics:\n");
    TclDumpMemoryInfo((ClientData) objPtr, 1);
#endif
    Tcl_AppendPrintfToObj(objPtr, "\n----------------------------------------------------------------\n");

    if (objc == 1) {
	Tcl_SetObjResult(interp, objPtr);
    } else {
	Tcl_Channel outChan;
	char *str = Tcl_GetStringFromObj(objv[1], &length);

	if (length) {
	    if (strcmp(str, "stdout") == 0) {
		outChan = Tcl_GetStdChannel(TCL_STDOUT);
	    } else if (strcmp(str, "stderr") == 0) {
		outChan = Tcl_GetStdChannel(TCL_STDERR);
	    } else {
		outChan = Tcl_OpenFileChannel(NULL, str, "w", 0664);
	    }
	} else {
	    outChan = Tcl_GetStdChannel(TCL_STDOUT);
	}
	if (outChan != NULL) {
	    Tcl_WriteObj(outChan, objPtr);
	}
    }
    Tcl_DecrRefCount(objPtr);
    return TCL_OK;
}
#endif /* TCL_COMPILE_STATS */

#ifdef TCL_COMPILE_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * StringForResultCode --
 *
 *	Procedure that returns a human-readable string representing a Tcl
 *	result code such as TCL_ERROR.
 *
 * Results:
 *	If the result code is one of the standard Tcl return codes, the result
 *	is a string representing that code such as "TCL_ERROR". Otherwise, the
 *	result string is that code formatted as a sequence of decimal digit
 *	characters. Note that the resulting string must not be modified by the
 *	caller.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static const char *
StringForResultCode(
    int result)			/* The Tcl result code for which to generate a
				 * string. */
{
    static char buf[TCL_INTEGER_SPACE];

    if ((result >= TCL_OK) && (result <= TCL_CONTINUE)) {
	return resultStrings[result];
    }
    TclFormatInt(buf, result);
    return buf;
}
#endif /* TCL_COMPILE_DEBUG */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
