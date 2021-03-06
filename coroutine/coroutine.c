#include "coroutine.h"

#include <stddef.h>
#include <malloc.h>
#include <assert.h>

#if defined(__amd64__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#	define ARCH_X86_64
#elif defined (__i386__) || defined(__i386) || defined(_M_IX86) || defined(_X86_) || defined(__X86__) || defined(__THW_INTEL__) || defined(__I86__) || defined(__INTEL__) || defined(__386)
#	define ARCH_X86
#else
#error "Architectures other than x86 or x86-64 are not supported"
#endif

#if defined(_WIN32) && !defined(_WIN64)
#	define CALL_CONV __fastcall
#elif defined(_WIN32) && defined(_WIN64)
// We use the microsoft x64 ABI in the assembly routines anyway
#	define CALL_CONV
#elif (defined __GNUC__ || defined __clang__) && defined ARCH_X86
#	define CALL_CONV __fastcall
#elif (defined __GNUC__ || defined __clang__) && defined ARCH_X86_64
#	define CALL_CONV ms_abi
#else
#	pragma warning "Unknown platform, the calling convention for the assembly routines may be incorrect"
#	define CALL_CONV
#endif

/* Adjustment procedures for various architectures (Only necessary if the stack grows down) */
#if defined(ARCH_X86_64)
// AMD64 (x86-64) architecture
#define ADJUST_SP(size, sp) ((void*)((char*)sp + size))
#define VALID_SP(s_st, sp) ((uintptr_t)sp > (uintptr_t)s_st)
#elif defined (ARCH_X86)
// x86 architecture
#define ADJUST_SP(size, sp) ((void*)((char*)sp + size))
#define VALID_SP(s_st, sp) ((uintptr_t)sp > (uintptr_t)s_st)
#endif

#define TRUE 1
#define FALSE 0

#pragma pack(push)
// Make sure it is arranged in packed bytes
#pragma pack(1)
// Information used to initialize the coroutine
// this data will not persist beyond the first
// yield
typedef struct _tmpinfo
{
	void* stack_base;
	void(CALL_CONV *internalfunc)(struct _tmpinfo*);
	void(*funcptr)(coroutine*, void*);
	coroutine* ctx;
} tmpinfo;
#pragma pack(pop)

struct _coroutine
{
	struct _x1
	{
		void* stack_pointer;
		void* stack_start;
	} coroutine;
	struct _x2
	{
		void* stack_pointer;
	} caller;
	void* datap;
	unsigned char complete;
	unsigned char external_mem;
};

/* ASM Routines */
// Switch to the given stack
// new_stack_ptr: the top of the stack to switch to
// old_stack_ptr: the current top of the stack
extern void CALL_CONV coroutine_jmp_stack(void* new_stack_ptr, void** old_stack_ptr);

extern void CALL_CONV coroutine_init_stack(const tmpinfo* info, void** old_stack_ptr);

/* C routines */

// Yield without checking parameters
void* coroutine_unsafe_yield(coroutine* ctx, void* datap)
{
	assert(ctx != NULL);

	// If this assert triggers then the coroutine stack has overflowed
	// there isn't really any recovery that can be done here. It's already
	// too late. The only solution is to increase the stack size of the 
	// coroutine at creation time. Alternatively platform specific features
	// can be used to build a large stack that doesn't use much actual memory.
	// NOTE: This assertion will not catch stack overflows that don't call coroutine_yield
	assert(ctx->coroutine.stack_pointer == NULL || 
		VALID_SP(ctx->coroutine.stack_start, ctx->coroutine.stack_pointer));

	ctx->datap = datap;
	coroutine_jmp_stack(ctx->caller.stack_pointer, &ctx->coroutine.stack_pointer);
	return ctx->datap;
}
// Switch into coroutine without 
void* coroutine_unsafe_next(coroutine* ctx, void* datap)
{
	// Calling this function with a NULL pointer violates the preconditions.
	// If ctx can be null then use coroutine_next instead.
	assert(ctx != NULL);

	// For the sake of performance this function does not check if the coroutine
	// is complete before jumping to it. In release mode this will cause the
	// program to crash. 
	assert(!coroutine_is_complete(ctx));

	ctx->datap = datap;
	coroutine_jmp_stack(ctx->coroutine.stack_pointer, &ctx->caller.stack_pointer);
	return ctx->datap;
}

void CALL_CONV _coroutine_init_func(const tmpinfo* info)
{
	// info stops being valid after we call yield
	// so we have to save ctx now
	coroutine* ctx = info->ctx;
	// Same with funcptr
	void(*funcptr)(coroutine*, void*) = info->funcptr;

	// Yield and get the first value from the caller
	// Unsafe yield is fine since we should not have
	// been able to get here if ctx is null
	void* datap = coroutine_unsafe_yield(ctx, NULL);

	// Now that the caller has called next
	// we can execute the coroutine method
	funcptr(ctx, datap);

	// Indicate that the coroutine has completed
	ctx->complete = TRUE;
}

/* External Interface */

void* coroutine_yield(coroutine* ctx, void* datap)
{
	if (!ctx)
		// If the user has gotten a corrupt context then this could cause
		// them to loop indefinitely. However, there isn't anything we 
		// could switch to. Also, returning NULL could be confusing, 
		// but there isn't a better value to return. NULL is the least surprising.
		return NULL;
	return coroutine_unsafe_yield(ctx, datap);
}

char coroutine_is_complete(const coroutine* ctx)
{
	if (!ctx)
		// Rationale behind returning -1 on error:
		//  - It is recognizable
		//  - When in an if statement it evaluates to true
		return -1;

	return ctx->complete;
}

void* coroutine_next(coroutine* ctx, void* datap)
{
	if (!ctx)
		// If we don't have a context we should just return NULL.
		// This is the most recognizable value and we should behave
		// robustly in face of user errors. 
		return NULL;

	if (!coroutine_is_complete(ctx))
	{
		return coroutine_unsafe_next(ctx, datap);
	}
	return ctx->datap;
}
void* coroutine_continue(coroutine* ctx, coroutine* next, void* datap)
{
	if (!ctx || !next)
		return NULL;

	// If this assert triggers then the coroutine stack has overflowed
	// there isn't really any recovery that can be done here. It's already
	// too late. The only solution is to increase the stack size of the 
	// coroutine at creation time. Alternatively platform specific features
	// can be used to build a large stack that doesn't use much actual memory.
	// NOTE: This assertion will not catch stack overflows that don't call coroutine_yield
	assert(ctx->coroutine.stack_pointer == NULL ||
		VALID_SP(ctx->coroutine.stack_start, ctx->coroutine.stack_pointer));

	if (ctx == next)
		// NOTE: It should be safe not to set next->datap here
		// since there shouldn't be a way for user code to access
		// it without having it set by yield, next or continue.
		// If this assumption turns out to be invalid then 
		// next->datap or ctx->datap should be set here as well.
		return datap;

	next->caller.stack_pointer = ctx->caller.stack_pointer;

	next->datap = datap;
	// Jump stacks and set our own stack pointer. 
	coroutine_jmp_stack(next->coroutine.stack_pointer, &ctx->coroutine.stack_pointer);
	return ctx->datap;
}

coroutine* coroutine_start(coroutine_data initdata)
{
	void* buffer = malloc(initdata.stack_size);
	coroutine* ctx = coroutine_start_with_mem(initdata, buffer);

	if (ctx != NULL)
		ctx->external_mem = FALSE;
	else if (buffer != NULL)
		// Free buffer if coroutine creation failed
		free(buffer);

	return ctx;
}
coroutine* coroutine_start_with_mem(coroutine_data initdata, void* stackmem)
{
	if (!stackmem) // Make sure we got a valid stack pointer
		return NULL;
	if (!initdata.funcptr) // Make sure that we were given a valid function pointer
		return NULL;
	if (initdata.stack_size == 0) // Ensure that we want to create a stack with an actual size
		return NULL;

	coroutine* ctx = malloc(sizeof(coroutine));

	if (!ctx) // Make sure malloc succeeded
		return NULL;

	ctx->coroutine.stack_start = stackmem;
	ctx->coroutine.stack_pointer = NULL;
	ctx->caller.stack_pointer = NULL;
	ctx->complete = FALSE;
	ctx->external_mem = TRUE;
	ctx->datap = NULL;

	tmpinfo info = {
		// Adjust stack pointer so that the stack grows into
		// the buffer. This needs to happen because the stack
		// can grow in different directions depending on the 
		// CPU that this is running on.
		ADJUST_SP(initdata.stack_size, ctx->coroutine.stack_start),
		&_coroutine_init_func,
		initdata.funcptr,
		ctx
	};

	coroutine_init_stack(&info, &ctx->caller.stack_pointer);

	return ctx;
}
void coroutine_destroy(coroutine* ctx, void* datap)
{
	if (ctx != NULL)
	{
		// Keep executing the coroutine until it is finished
		while (!coroutine_is_complete(ctx))
		{
			coroutine_next(ctx, datap);
		}

		// Free up resources
		coroutine_abort(ctx);
	}
}
void coroutine_abort(coroutine* ctx)
{
	if (ctx != NULL)
	{
		if (!ctx->external_mem)
			free(ctx->coroutine.stack_start);
		free(ctx);
	}
}
