#ifndef C_COROUTINE_H
#define C_COROUTINE_H

#define COROUTINE_DEAD 0
#define COROUTINE_READY 1
#define COROUTINE_RUNNING 2
#define COROUTINE_SUSPEND 3

struct schedule; //协程调度器

typedef void (*coroutine_func)(struct schedule *, void *ud); //协程执行函数

struct schedule * coroutine_open(void); //创建协程调度器
void coroutine_close(struct schedule *); //关闭协程调度器

int coroutine_new(struct schedule *, coroutine_func, void *ud); //用协程调度器创建一个协程
void coroutine_resume(struct schedule *, int id); //恢复id号协程
int coroutine_status(struct schedule *, int id); //返回id号协程的状态
int coroutine_running(struct schedule *); //返回正在执行的协程id
void coroutine_yield(struct schedule *); //保存上下文后中断当前协程执行

#endif
