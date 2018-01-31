#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;

//协程调度器
struct schedule {
	char stack[STACK_SIZE]; //共享栈
	ucontext_t main; //协程中断返回到调度器时回复的上下文
	int nco; //已有协程数量
	int cap; //最大协程数量
	int running; //正在运行的协程id
	struct coroutine **co; //保存所有协程
};

//协程
struct coroutine {
	coroutine_func func; //执行函数
	void *ud; //参数
	ucontext_t ctx; //中断时保存的上下文
	struct schedule * sch; //所属调度器
	ptrdiff_t cap; //栈容量
	ptrdiff_t size; //已使用栈大小
	int status; //协程状态
	char *stack; //栈
};

//创建协程
struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
	struct coroutine * co = malloc(sizeof(*co)); //分配空间
	co->func = func; //设置执行函数
	co->ud = ud; //设置参数
	co->sch = S; //设置调度器
	co->cap = 0;
	co->size = 0; 
	co->status = COROUTINE_READY; //初始化为ready状态
	co->stack = NULL;
	return co;
}

//删除协程
void
_co_delete(struct coroutine *co) {
	free(co->stack);
	free(co); //释放空间
}

//创建调度器
struct schedule * 
coroutine_open(void) {
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	S->cap = DEFAULT_COROUTINE;
	S->running = -1; //初始化工作
	S->co = malloc(sizeof(struct coroutine *) * S->cap); //为所有协程分配空间
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

//删除调度器
void 
coroutine_close(struct schedule *S) {
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co); //释放每一个协程
		}
	}
	free(S->co); //释放给所有协程分配的空间
	S->co = NULL;
	free(S); //释放调度器
}

//创建协程并加入调度器
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
	struct coroutine *co = _co_new(S, func , ud);
	if (S->nco >= S->cap) { //如果协程数量超过容量，则重新分配，容量扩大两倍，返回协程id
		int id = S->cap;
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
		for (i=0;i<S->cap;i++) { //当前协程数量小于容量
			int id = (i+S->nco) % S->cap; // nco是当前有的协程数量，之前的协程释放可能产生空位，所以取余
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

//协程第一次执行调用的函数
static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32); //不知道为什么拆成高低位
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running; //取得当前正在执行的协程id
	struct coroutine *C = S->co[id];
	C->func(S,C->ud); //调用协程绑定的函数
	_co_delete(C); //协程已经执行完，可以清除
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

//恢复id号协程
void 
coroutine_resume(struct schedule * S, int id) {
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;
	switch(status) {
	case COROUTINE_READY: //如果状态是ready也就是第一次创建
		getcontext(&C->ctx); //获取当前上下文
		C->ctx.uc_stack.ss_sp = S->stack; //将协程栈设置为调度器的共享栈
		C->ctx.uc_stack.ss_size = STACK_SIZE; //设置栈容量  使用时栈顶栈底同时指向S->stack+STACK_SIZE，栈顶向下扩张
		C->ctx.uc_link = &S->main; //将返回上下文设置为调度器的上下文，协程执行完后会返回到main上下文
		S->running = id; //设置调度器当前运行的协程id
		C->status = COROUTINE_RUNNING; //设置协程状态
		uintptr_t ptr = (uintptr_t)S;
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));//重置上下文执行mainfunc
		swapcontext(&S->main, &C->ctx); //保存当前上下文到main，跳转到ctx的上下文
		break;
	case COROUTINE_SUSPEND: //如果状态时暂停也就是之前yield过
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size); //将协程之前保存的栈拷贝到调度器的共享栈
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx); //同上
		break;
	default:
		assert(0);
	}
}

//保存共享栈到私有栈
static void
_save_stack(struct coroutine *C, char *top) { //top为栈底
	char dummy = 0; //这里定义一个char变量，dummy地址为栈顶
	assert(top - &dummy <= STACK_SIZE); //dummy地址减栈底地址为当前使用的栈大小
	if (C->cap < top - &dummy) { //如果当前协程栈大小小于已用大小，重新分配
		free(C->stack);
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	C->size = top - &dummy;
	memcpy(C->stack, &dummy, C->size); //将共享栈拷贝到协程栈
}

//中断协程执行
void
coroutine_yield(struct schedule * S) {
	int id = S->running; //回去当前正在执行的协程id
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	_save_stack(C,S->stack + STACK_SIZE);//保存当前协程的栈
	C->status = COROUTINE_SUSPEND;
	S->running = -1; //设置状态
	swapcontext(&C->ctx , &S->main);//保存当前上下文，跳转到main上下文
}

//返回id号协程的状态
int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

//返回正在运行的协程id
int 
coroutine_running(struct schedule * S) {
	return S->running;
}

