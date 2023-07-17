#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>  
#include <linux/sched.h>
#include <sched.h>
#include <unistd.h>
#include <syscall.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <signal.h>
#include <limits.h>
#include "many-many.h"
#include "utils.h"
#include "lock.h"

node_list thread_list;
int alarm_index = -1;
thread_id main_ktid;
int no_of_kthreads = NO_OF_KTHREADS;
static int is_init_done = 0;


void scheduler();
void thread_exit(void *retval);


node* scheduler_node_array;
node** curr_running_proc_array;
int* kthread_index;

void traverse() {
    node *nn = thread_list.list;

    while(nn) {
        printf("traverse %d %ld %ld  ", nn->state, nn->tid, nn->kernel_tid);
        nn = nn->next;
    }
    printf("\n");
    
}

void cleanup(thread_id tid) {
	// printf("given tid %ld\n", tid);
	// acquire(&thread_list.lock);
	node *prev = NULL, *curr = thread_list.list;
	while(curr && curr->tid != tid) {
		prev = curr;
		curr = curr->next;
	}
	if(curr == thread_list.list) {
		// head node
		node *tmp = thread_list.list;
		thread_list.list = thread_list.list->next;
        // release(&thread_list.lock);
		munmap(tmp, tmp->stack_size);
		free(tmp->wrapper_fun);
        free(tmp->sig_info);
		free(tmp);
        // printf("cleaned one node\n");
		return;
	}
	if(! curr) {
		printf("DEBUG: cleanup node not found\n");
        // release(&thread_list.lock);
		return;
	}
	prev->next = curr->next;
	// release(&thread_list.lock);
	munmap(curr->stack_start, curr->stack_size);
	free(curr->wrapper_fun);
    free(curr->sig_info);
	free(curr);
    // printf("cleaned one node\n");
    return;
}

void cleanupAll() {
    // printf("in exit\n");
    acquire(&thread_list.lock);
    // kill(getpid(), SIGKILL);
    if(thread_list.list == NULL) printf("null thread list\n");
	while(thread_list.list) {
        // traverse();
		cleanup(thread_list.list->tid);
    }
	// printf("Cleaning all theread stacks\n");
    release(&thread_list.lock);
}

void handle_pending_signals() {
    int curr_kthread_index = get_curr_kthread_index();
    
    // if currently no thread is executing on current kernel thread
    if (! curr_running_proc_array[curr_kthread_index])
        return;

    ualarm(0,0);
    node* curr_thread = curr_running_proc_array[curr_kthread_index];
    int k = curr_thread->sig_info->rem_sig_cnt;
    // printf("total pending signals %d\n", k);
    sigset_t signal_list;
    for (int i = 0; i < k; i++) {
        int signal_to_handle = curr_thread->sig_info->signal_list->t_signal;
        printf("handle pending signal %d of %ld tid\n", signal_to_handle, curr_thread->tid);
        sigaddset(&signal_list, signal_to_handle);
        sigprocmask(SIG_UNBLOCK, &signal_list, NULL);
        // printf("ss = %d\n",  curr_thread->sig_info->arr[curr_thread->sig_info->rem_sig_cnt - 1]);
        curr_thread->sig_info->rem_sig_cnt--;
        curr_thread->sig_info->signal_list = curr_thread->sig_info->signal_list->next;
        // printf("ps = %d\n", curr_running_proc->sig_info->rem_sig_cnt);
        raise(signal_to_handle);
        // kill(getpid(), --curr_running_proc->sig_info->arr[curr_running_proc->sig_info->rem_sig_cnt]);
    }
    ualarm(ALARM_TIME,0);
    enable_alarm_signal();
    // printf("kk %d\n",  curr_thread->sig_info->rem_sig_cnt);
}

// setjump returns 0 for the first time, next time it returns value used in longjump(here 2) 
// so switch to scheduler will execute only once.
void signal_handler_alarm() {
    // printf("inside signal handler\n");    
    // disable alarm
    ualarm(0,0);
    alarm_index = (alarm_index + 1) % no_of_kthreads;
	syscall(SYS_tgkill, getpid(), kthread_index[alarm_index], SIGVTALRM);
    return;
}

// setjump returns 0 for the first time, next time it returns value used in longjump(here 2) 
// so switch to scheduler will execute only once.
void signal_handler_vtalarm() {
    // printf("inside vt signal handler\n");    
    // disable alarm
    ualarm(0,0);
    // printf("vtalarm handler\n");
    // switch context to scheduler
    int value = sigsetjmp(*(curr_running_proc_array[get_curr_kthread_index()]->t_context), 1);
    if(! value) {
        siglongjmp(*(scheduler_node_array[get_curr_kthread_index()].t_context), 2);
    }
    // handle_pending_signals();
    return;
}


int execute_me_oo(void *new_node) {
	node *nn = (node*)new_node;
    if(nn->state==THREAD_EMBRYO)
	    nn->state = THREAD_RUNNING;
    // printf("changed from embryo to running\n");

    // printf("in execute me");
    // if(nn->wrapper_fun->args)
    //     printf("pa = %p\n", nn->wrapper_fun->fun);
    // printf("pa = %p\n", nn->wrapper_fun->fun);thread_kill
    ualarm(K_ALARM_TIME,0);
    enable_alarm_signal();
    nn->kernel_tid = gettid();
	nn->wrapper_fun->fun(nn->wrapper_fun->args);
    // printf("in execute end");
	// nn->state = THREAD_TERMINATED;
    thread_exit(NULL);

	// printf("termination done %d\n", nn->kthread_index);
    // exit(1);
    // return 0;
	// siglongjmp(*(scheduler_node_array[nn->kthread_index].t_context), 2);
}

int execute_me_mo() {
    // thread_id curr_ktid = gettid();
    // int i;
    // for(i = 0; i < NO_OF_KTHREADS; i++) {
    //     if(curr_ktid == thread_list.list_array[i]->kernel_tid)
    //         break;
    // }
    acquire(&thread_list.lock);
    node *nn = curr_running_proc_array[get_curr_kthread_index()];
    // printf("state = %d %ld", nn->state, nn->tid);
    // while(nn->state != THREAD_RUNNING)
    //     nn = nn->next;
    release(&thread_list.lock);
    
	// printf("inside execute me\n");
    nn->kernel_tid = gettid();
	nn->wrapper_fun->fun(nn->wrapper_fun->args);
    // printf("execute me end\n");
	// nn->state = THREAD_TERMINATED;
    thread_exit(NULL);

	// printf("termination done\n");
    //TODO: IMP: don't call scheduler() directly,instead use long jump
    // siglongjmp(*(scheduler_node.t_context), 2);
    // siglongjmp(*(scheduler_node_array[nn->kthread_index].t_context), 2);
	return 0;
}

void insert_sig_node(signal_info *info, sig_node *node) {
    node->next = info->signal_list;
    info->signal_list = node;
}

node* search_thread(thread_id tid) {

    node n;
    int found_flag = 0;

    acquire(&thread_list.lock);
    node *tmp = thread_list.list;
    while(tmp && tmp->tid != tid)
        tmp = tmp->next;
    release(&thread_list.lock);

    if(! tmp) {
        // error condition
        // printf("Debug: thread not found error. exiting\n");
        // exit(1);
    }
	return tmp;
}



// insert thread_id node in beginning of list
void thread_insert(node* nn) {
    acquire(&thread_list.lock);
	nn->next = thread_list.list;
	thread_list.list = nn;
    // traverse();
    release(&thread_list.lock);
    // printf("tid insert done\n");
}


void scheduler() {
    while(1) {
        // printf("inside scheduler\n");
        // traverse();
        // exit(1);
        int index = get_curr_kthread_index();
        //   printf("inside scheduler2 %d \n", index);

        node* curr_running_proc = curr_running_proc_array[index];
        curr_running_proc_array[index] = &(scheduler_node_array[index]);
        // printf("inside scheduler\n");
        // printf("tis %ld %d\n", curr_running_proc->tid, curr_running_proc->state);
        // traverse();

        if(curr_running_proc->state == THREAD_RUNNING)
            curr_running_proc->state = THREAD_RUNNABLE;
            
        // point next_proc to next thread of currently running process
        acquire(&thread_list.lock);
        node *next_proc = curr_running_proc->next;
        if(! next_proc) next_proc = thread_list.list;            // TODO wrong
        
        while(next_proc->state != THREAD_RUNNABLE) {
            if(next_proc->next) next_proc = next_proc->next;
            else next_proc = thread_list.list;
            // if(next_proc==curr_running_proc){
                // printf("%d %d gggggggggggggggggggggggggggggggggggggggg\n ", next_proc->state, gettid());
                release(&thread_list.lock);
                // printf("sleeping\n");
                sleep(0.5);
                acquire(&thread_list.lock);
                // printf("lock acquired again\n");
            // }
        }

        next_proc->kthread_index = index;
        next_proc->state = THREAD_RUNNING;
        next_proc->kernel_tid = gettid();
        release(&thread_list.lock);
        curr_running_proc_array[index] = next_proc;

        enable_alarm_signal();
        ualarm(ALARM_TIME, 0);
        // printf("%ld %ld %d gg ", next_proc->kernel_tid, next_proc->tid, next_proc->kthread_index);
        siglongjmp(*(next_proc->t_context), 2);
    }
    
}

void signal_handler_usr2() {
	printf("SIGUSR2 interrupt received.\n");
}

void init_many_many() {     // TODO call only once in therad_create
    thread_list.list = NULL;
    kthread_index = (int*)malloc(sizeof(int)*no_of_kthreads);
    scheduler_node_array = (node*)malloc(sizeof(node)*no_of_kthreads);
    for(int i=0; i<no_of_kthreads; i++){
        
        scheduler_node_array[i].t_context = (jmp_buf*)malloc(sizeof(jmp_buf));

        scheduler_node_array[i].stack_start = mmap(NULL, GUARD_PAGE_SIZE + DEFAULT_STACK_SIZE , PROT_READ|PROT_WRITE,MAP_STACK|MAP_ANONYMOUS|MAP_PRIVATE, -1 , 0);
        mprotect(scheduler_node_array[i].stack_start, GUARD_PAGE_SIZE, PROT_NONE);
        scheduler_node_array[i].stack_size = DEFAULT_STACK_SIZE;
        scheduler_node_array[i].tid = INT_MAX - i;
        scheduler_node_array[i].wrapper_fun = (wrap_fun_info*)malloc(sizeof(wrap_fun_info));
        scheduler_node_array[i].wrapper_fun->fun = scheduler;
        scheduler_node_array[i].wrapper_fun->args = NULL;

        (*(scheduler_node_array[i].t_context))->__jmpbuf[6] = mangle((long int)scheduler_node_array[i].stack_start+DEFAULT_STACK_SIZE+GUARD_PAGE_SIZE );
        (*(scheduler_node_array[i].t_context))->__jmpbuf[7] = mangle((long int)scheduler_node_array[i].wrapper_fun->fun);
    }
    main_ktid = gettid();
    // acquire(&thread_list.lock);
    // thread_list.list = (node*)malloc(sizeof(node)*NO_OF_KTHREADS);
    // release(&thread_list.lock);

    curr_running_proc_array = (node**)malloc(sizeof(node*) * no_of_kthreads);
    for(int i=0; i<no_of_kthreads; i++)
        curr_running_proc_array[i] = NULL;

    signal(SIGALRM, signal_handler_alarm);
    signal(SIGVTALRM, signal_handler_vtalarm);
    signal(SIGUSR2, signal_handler_usr2);
    // traverse();
    // printf("init done\n");
    // ualarm(K_ALARM_TIME, 0);
}

void init_mThread_attr(mThread_attr **attr) {
	*attr = (mThread_attr*)malloc(sizeof(mThread_attr));
	(*attr)->guardSize = GUARD_PAGE_SIZE;
	(*attr)->stack = NULL;
	(*attr)->stackSize = DEFAULT_STACK_SIZE;
}

int thread_kill(mThread thread, int signal){
    ualarm(0,0);
    if (signal == SIGINT || signal == SIGKILL || signal == SIGSTOP || signal == SIGCONT)
        kill(getpid(), signal);
    else if(signal == SIGTERM) {
        int curr_kthread_index = get_curr_kthread_index();

        if(curr_kthread_index != -1 && curr_running_proc_array[curr_kthread_index]->tid == thread) {
            thread_exit(NULL);
            return 0;
        }
        else {
            node* n = search_thread(thread);
            if(! n) return NO_THREAD_FOUND;
            acquire(&thread_list.lock);
            // printf("set terminate done\n");
            n->state = THREAD_TERMINATED;
            release(&thread_list.lock);
            // traverse();
            return 0;
        }
    }
    else {

        int curr_kthread_index = get_curr_kthread_index();

        if(curr_kthread_index != -1 && curr_running_proc_array[curr_kthread_index]->tid == thread) {
            int val = raise(signal);
            if(val == -1)
		        return RAISE_ERROR;
        }
        else {
            node* n = (node *)malloc(sizeof(node)); // redundant
            sig_node *signal_node = (sig_node*)malloc(sizeof(sig_node));
            signal_node->t_signal = signal;
            int ktid;
            n = search_thread(thread);
            // printf("adding signal node in %ld tid node having %d ktid\n", n->tid, n->kthread_index);
            if(! n) return NO_THREAD_FOUND;

            insert_sig_node(n->sig_info, signal_node);
            n->sig_info->rem_sig_cnt++;
            // printf("inside thread kill %d %d\n", n->sig_info->signal_list->t_signal, signal);
        }
    }
    ualarm(ALARM_TIME, 0);
}


void set_no_of_kthreads(int num){
    if(! is_init_done)
        no_of_kthreads = num;
    return;
}

int thread_create(mThread *thread, mThread_attr *attr, void *routine, void *args) {
	if(! is_init_done) {
        // atexit(cleanupAll);
		init_many_many();
		is_init_done = 1;
	}

    if(! thread || ! routine) return INVAL_INP;

    int guardSize, stackSize;
	void *stack;
	if(attr) {
		if(attr->guardSize) guardSize = attr->guardSize;
		else guardSize = GUARD_PAGE_SIZE;
		
        if(attr->stackSize && !attr->stack) stackSize = attr->stackSize;
		else stackSize = DEFAULT_STACK_SIZE;
		
        if(attr->stack) {
			stack = attr->stack;
            stackSize = -1; // indicating current stack user stack
        }
		else {
			stack = mmap(NULL, guardSize + stackSize , PROT_READ|PROT_WRITE,MAP_STACK|MAP_ANONYMOUS|MAP_PRIVATE, -1 , 0);
			if(stack == MAP_FAILED)
				return MMAP_FAILED;
            mprotect(stack, guardSize, PROT_NONE);
		}
	}
	else {
		guardSize = GUARD_PAGE_SIZE;
		stackSize = DEFAULT_STACK_SIZE;
		stack = mmap(NULL, guardSize + stackSize , PROT_READ|PROT_WRITE,MAP_STACK|MAP_ANONYMOUS|MAP_PRIVATE, -1 , 0);
		if(stack == MAP_FAILED)
			return MMAP_FAILED;
		mprotect(stack, guardSize, PROT_NONE);
	}

    static thread_id id = 0;

    wrap_fun_info *info = (wrap_fun_info*)malloc(sizeof(wrap_fun_info));
    info->fun = routine;
    info->args = args;
    info->thread = thread;
    // printf("p = %p\n", routine);
    node *t_node = (node *)malloc(sizeof(node));
    t_node->tid = id++;
    *thread = t_node->tid;
    t_node->t_context = (jmp_buf*)malloc(sizeof(jmp_buf));
    t_node->ret_val = 0;         // not required
    t_node->stack_start = stack;
    t_node->stack_size = stackSize;
    t_node->wrapper_fun = info;  // not required

    t_node->sig_info = (signal_info*)malloc(sizeof(signal_info));
    t_node->sig_info->signal_list = NULL;
    t_node->sig_info->rem_sig_cnt = 0;
    
    
    if(t_node->tid < no_of_kthreads) {
        // printf("clone called\n");
        curr_running_proc_array[t_node->tid] = t_node;
        t_node->state = THREAD_EMBRYO;
        thread_insert(t_node);
        // kthread_index[t_node->tid] = t_node;
        kthread_index[t_node->tid] = clone(execute_me_oo, t_node->stack_start + stackSize + guardSize, CLONE_FLAGS, (void *)t_node);	
        if(kthread_index[t_node->tid] == -1) 
		    return CLONE_FAILED;
        return 0;
    }
    else {
        (*(t_node->t_context))->__jmpbuf[6] = mangle((long int)t_node->stack_start + stackSize + guardSize );
        (*(t_node->t_context))->__jmpbuf[7] = mangle((long int)execute_me_mo);
        
        t_node->state = THREAD_RUNNABLE;
        // t_node->kernel_tid = thread_list.list_array[t_node->tid % NO_OF_KTHREADS]->kernel_tid;
        thread_insert(t_node);    // TODO modifye this scheme
    }
    
    return 0;
}


int thread_join(mThread tid, void **retval) {
	// if(! retval)
	// 	return INVAL_INP;
    int found_flag = 0;

    // printf("acquired\n");
    acquire(&thread_list.lock);
	node* n = thread_list.list;

    while(n) {
        if(n->tid == tid) {
            // found_flag = 1;
            break;
        }
        n = n->next;
    }
    release(&thread_list.lock);
    // printf("rel done\n");

	if(!n)
		return NO_THREAD_FOUND;

    // printf("thread id %ld %ld\n", tid, n->tid);
    // traverse();
	while(n->state != THREAD_TERMINATED)
		;
    // printf("dddd\n");
    if(retval)
	    *retval = n->ret_val;
    // acquire(&thread_list.lock);
    // cleanup(tid);
    // release(&thread_list.lock);
	return 0;
}

void thread_exit(void *retval) {
    node* nn;
    // printf("start\n");
    int index = get_curr_kthread_index();
    // printf("index %d\n", index);

    acquire(&thread_list.lock);
    nn = thread_list.list;
    while(nn->state != THREAD_RUNNING && nn->kernel_tid == gettid())
        nn = nn->next;

	nn->ret_val = retval;
	nn->state = THREAD_TERMINATED;
    release(&thread_list.lock);
    
    siglongjmp(*(scheduler_node_array[index].t_context), 2);
    return;

	// syscall(SYS_exit, EXIT_SUCCESS);
}

void init_thread_lock(struct spinlock *lk){
    initlock(lk);
}

void thread_lock(struct spinlock *lk){
    acquire(lk);
}

void thread_unlock(struct spinlock *lk){
    release(lk);
}

void init_mutex_thread_lock(struct sleeplock *lk){
    initsleeplock(lk);
}

void thread_mutex_lock(struct sleeplock *lk){
    acquiresleep(lk);
}

void thread_mutex_unlock(struct sleeplock *lk){
    releasesleep(lk);
}
