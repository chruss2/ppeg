#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

#include <python.h>

typedef enum {
    iEnd = 0,
    iChar,
    iJump,
    iChoice,
    iCall,
    iReturn,
    iCommit,
    iCapture,
    iFail,

    /* Extended Codes */
    iAny,
    iCharset,
    iPartialCommit,
    iSpan,
    iFailTwice,
    iBackCommit,

    /* Non-executable instructions */
    iOpenCall,
} OpCode;

typedef struct {
    const char *name;
    int extra_len;
} OpData;

#define CHARSET_BYTES ((UCHAR_MAX/CHAR_BIT) + 1)
typedef unsigned char Charset[CHARSET_BYTES];
#define IN_CHARSET(set, ch) ((int)(set)[(ch)>>3] & (1 << ((ch)&7)))
#define SET_CHARSET(set, ch) ((set)[(ch)>>3] |= (1 << ((ch)&7)))

OpData opdata[] = {
    { "End", 0 },
    { "Char", 1 /* Character */ },
    { "Jump", 0 },
    { "Choice", 0 },
    { "Call", 0 },
    { "Return", 0 },
    { "Commit", 0 },
    { "Capture", sizeof(void*) /* Capture info */ },
    { "Fail", 0 },
    { "Any", 4 /* Count */ },
    { "Charset", CHARSET_BYTES },
    { "PartialCommit", 0 },
    { "Span", 0 },
    { "FailTwice", 0 },
    { "BackCommit", 0 },
    { "OpenCall", 1 /* Rule number */ },
    { NULL, 0 }
};

/* Instruction layout:
 *
 * +------------------+
 * | Opcode (1 byte)  |
 * +------------------+
 * |                  |
 * | Offset (3 bytes) |
 * |                  |
 * +------------------+
 * |                  |
 * | Additional data  |
 * |                  |
 * +------------------+
 *
 * Additional data is opcode-dependent, fixed length, and always a multiple of
 * 4 bytes (so that opcodes are always word aligned). Offset is a 3-byte
 * signed integer.
 */
typedef struct {
    OpCode instr;
    signed int offset;
    union {
	unsigned int count;
	Py_UNICODE character;
	Charset cset;
	void *capture_info;
	unsigned int rule;
    };
} Instruction;

typedef struct stackentry {
    enum { ReturnData, BacktrackData } entry_type;
    union {
	unsigned int ret;
	struct {
	    unsigned int alternative;
	    const Py_UNICODE *pos;
	    void *capture_info;
	};
    };
} StackEntry;

static StackEntry *stack = NULL;
static int stackpos = -1;
static int stacksize = 0;

#define STACK_CHUNK (100)
#define STACK_PUSHRET(rtn) (Stack_Ensure(), (++stackpos), \
	(stack[stackpos].entry_type = ReturnData), \
	(stack[stackpos].ret = (rtn)))
#define STACK_PUSHALT(alt, psn, cap) (Stack_Ensure(), (++stackpos), \
	(stack[stackpos].entry_type = BacktrackData), \
	(stack[stackpos].alternative = (alt)), \
	(stack[stackpos].pos = (psn)), \
	(stack[stackpos].capture_info = (cap)))
#define STACK_EMPTY() (stackpos == -1)
#define STACK_TOPTYPE() (stack[stackpos].entry_type)
#define STACK_POP() (--stackpos)
#define STACK_TOP() (stack[stackpos])

void Stack_Ensure (void) {
    /* At start, stack == NULL, stacksize == 0, stackpos == -1, so we allocate
     * first time
     */
    if (stackpos + 1 >= stacksize) {
	stacksize += STACK_CHUNK;
	stack = realloc(stack, stacksize * sizeof(StackEntry));
    }
}

#define FAIL ((unsigned int)(-1))

/* Returns a pointer to the first unmatched character, or NULL if the match
 * failed
 */
const Py_UNICODE *run (Instruction *prog, const Py_UNICODE *target, const Py_UNICODE *end)
{
    unsigned int pc = 0;
    const Py_UNICODE *pos = target;
    void *capture = NULL;
    Instruction *instr;

#if 0
    printf("sizeof(Instruction) = %d\n", sizeof(Instruction));
    for (instr = prog; instr->instr != iEnd; ++instr) {
	printf("Instr: %s, offset: %d\n", opdata[instr->instr].name, instr->offset);
    }
#endif

    for (;;) {
	if (pc == FAIL) {
	    /* Machine is in fail state */
	    if (STACK_EMPTY()) {
		/* No further options */
		return NULL;
	    }
	    if (STACK_TOPTYPE() == BacktrackData) {
		/* Backtrack to stacked alternative */
		pc = STACK_TOP().alternative;
		pos = STACK_TOP().pos;
		capture = STACK_TOP().capture_info;
	    }
	    /* Pop one stack entry */
	    STACK_POP();
	    continue;
	}
	instr = &prog[pc];
#if 0
	printf("pc = %d, pos = %d, instr = %s\n", pc, pos-target, 
		opdata[instr->instr].name);
#endif
	switch (instr->instr) {
            case iEnd:
		return pos;
            case iJump:
		pc += instr->offset;
		break;
            case iCall:
		STACK_PUSHRET(pc + 1);
		pc += instr->offset;
		break;
            case iReturn:
		pc = STACK_TOP().ret;
		STACK_POP();
		break;
            case iCommit:
		STACK_POP();
		pc += instr->offset;
		break;
            case iChoice:
		STACK_PUSHALT(pc + instr->offset, pos - instr->count, capture);
		pc += 1;
		break;
            case iPartialCommit:
		if (STACK_TOPTYPE() == BacktrackData) {
		    /* Replace the backtrack data on the top of the stack */
		    STACK_TOP().pos = pos;
		    STACK_TOP().capture_info = capture;
		} else {
		    /* Cannot happen */
		    assert(0);
		}
		pc += instr->offset;
                break;
            case iBackCommit:
		if (STACK_TOPTYPE() == BacktrackData) {
		    /* Pop the position and capture info, but jump */
		    pos = STACK_TOP().pos;
		    capture = STACK_TOP().capture_info;
		    STACK_POP();
		} else {
		    /* Cannot happen */
		    assert(0);
		}
		pc += instr->offset;
                break;
            case iCapture:
		/* TODO: Add capture info */
		pc += 1;
		break;
            case iFailTwice:
		STACK_POP();
		pc = FAIL;
                break;
            case iFail:
		pc = FAIL;
		break;
            case iAny:
		if (instr->count <= end - pos) {
		    pc += 1;
		    pos += instr->count;
		} else if (instr->offset)
		    pc += instr->offset;
		else
		    pc = FAIL;
		break;
            case iChar:
		if (pos < end && *pos == instr->character) {
		    pos += 1;
		    pc += 1;
		} else if (instr->offset)
		    pc += instr->offset;
		else
		    pc = FAIL;
		break;
            case iCharset:
		if (IN_CHARSET(instr->cset, *pos)) {
		    pc += 1;
		    pos += 1;
		} else if (instr->offset)
		    pc += instr->offset;
		else
		    pc = FAIL;
                break;
            case iSpan:
		if (IN_CHARSET(instr->cset, *pos))
		    pos += 1;
		else
		    pc += 1;
                break;
	    default:
		/* Cannot happen - skip the instruction */
		pc += 1;
		break;
	}
    }

    /* We never actually reach here - we exit at the End instruction */
    return pos;
}

static PyObject *cpeg_match (PyObject *self, PyObject *args) {
    void *instr;
    int instr_len;
    const Py_UNICODE *str;
    int str_len;
    const Py_UNICODE *result;

    if (!PyArg_ParseTuple(args, "s#u#:match", &instr, &instr_len, &str, &str_len))
        return NULL;

    result = run(instr, str, str + str_len);

    if (result)
	return Py_BuildValue("i", result - str);

    Py_RETURN_NONE;
}

static PyMethodDef _cpeg_methods[] = {
    {"match", (PyCFunction)cpeg_match, METH_VARARGS,
	"Match a string to the supplied PEG"},
    {NULL}  /* Sentinel */
};

#ifndef PyMODINIT_FUNC  /* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
init_cpeg(void)
{
    PyObject* m;
    long i;
    OpData *op;

    m = Py_InitModule3("_cpeg", _cpeg_methods, "PEG matcher module.");
    if (m == NULL)
        return;

    for (i = 0, op = opdata; op->name; ++i, ++op) {
	if (PyModule_AddIntConstant(m, op->name, i) != 0)
	    return;
    }
}