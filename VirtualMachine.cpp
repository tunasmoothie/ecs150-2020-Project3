#include "VirtualMachine.h"
#include "Machine.h"
#include <unistd.h>
#include <vector>
#include <queue>
#include <cstring>
#include <iostream>

typedef unsigned int TVMMutexState, *TVMMutexStateRef;
#define VM_MUTEX_STATE_LOCKED                   ((TVMThreadState)0x00)
#define VM_MUTEX_STATE_UNLOCKED                 ((TVMThreadState)0x01)

extern "C" {
TVMMainEntry VMLoadModule(const char *module);
void VMUnloadModule(void);
void AlarmCallback(void* calldata);

// OBJECTS
//=============================================================
struct TCB{
    TVMThreadID tid;
    TVMThreadState state;
    TVMThreadPriority prio;
    SMachineContext cntx;
    TVMThreadEntry entry;
    void *param;
    void *stackAdr;
    size_t stackSize;
    TVMTick timeup;
};

struct Mutex{
    TVMMutexID mid;
    TVMThreadID owner;
    int status = VM_MUTEX_STATE_UNLOCKED;
    std::vector<TCB*> waitingThreads;
};
//=============================================================


// HELPER FUNCTIONS
//=============================================================
void EmptyMain(void* param){
}

void IdleMain(void* param){
    //std::cout << "-Idling..." << "\n";
    while(1);
}
struct TCBComparePrio {
    bool operator()(TCB *lhs, TCB *rhs) {
        return lhs->prio < rhs->prio;
    }
};
struct TCBCompareTimeup {
    bool operator()(TCB *lhs, TCB *rhs) {
        return lhs->timeup > rhs->timeup;
    }
};
//=============================================================


// VARIABLES & CONTAINERS
//=============================================================
volatile unsigned int g_tick;
unsigned int tickMS;
TMachineSignalState sigState;
int fileResult;
void* sharedMem;

TCB* runningThread;
TCB* idleThread;
std::vector<TCB*> threadList;
std::vector<Mutex*> mutexList;
//std::priority_queue<TCB*, std::vector<TCB*>, TCBCompare> deadThreadList;
std::priority_queue<TCB*, std::vector<TCB*>, TCBCompareTimeup> waitThreadList;
std::priority_queue<TCB*, std::vector<TCB*>, TCBComparePrio> readyThreadList;
//=============================================================


TVMStatus VMStart(int tickms, TVMMemorySize sharedsize, int argc, char *argv[]){
    TVMMainEntry main = VMLoadModule(argv[0]);
    tickMS = tickms;
    sharedMem = MachineInitialize(512);
    MachineEnableSignals();
    MachineRequestAlarm(useconds_t(1000 * tickMS), &AlarmCallback, nullptr);


    //Create main and idle threads, with IDs idle = 0, main = 1, set current thread to 1
    TVMThreadID idleThreadID, mainThreadID;
    VMThreadCreate(IdleMain, NULL, 0x10000, 0, &idleThreadID);
    VMThreadActivate(idleThreadID);
    idleThread = readyThreadList.top();
    VMThreadCreate(EmptyMain, NULL, 0x100000, VM_THREAD_PRIORITY_NORMAL, &mainThreadID);
    VMThreadActivate(mainThreadID);
    runningThread = readyThreadList.top();
    runningThread->state = VM_THREAD_STATE_RUNNING;
    readyThreadList.pop();


    main(argc, argv);
    MachineTerminate();
    VMUnloadModule();

    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickMS(int *tickmsref){
    *tickmsref = tickMS;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickCount(TVMTickRef tickref){
    *tickref = g_tick;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority priority, TVMThreadIDRef tidRef) {
    if (entry == NULL || memsize == 0)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineSuspendSignals(&sigState);
    *tidRef = (TVMThreadID) threadList.size();
    TCB *tcb = new TCB();
    tcb->tid = (TVMThreadID) threadList.size();
    tcb->prio = priority;
    tcb->state = VM_THREAD_STATE_DEAD;
    tcb->entry = entry;
    tcb->param = param;
    tcb->stackSize = memsize;
    tcb->stackAdr = (void *) new uint8_t[memsize];
    threadList.push_back(tcb);
    MachineResumeSignals(&sigState);

    return VM_STATUS_SUCCESS;
}

// Skeleton function
void ThreadWrapper(void* param){
    TCB* tcb = (TCB*)(param);
    (tcb->entry)(tcb->param);

    if(tcb->tid != 0)
        VMThreadTerminate(tcb->tid);
}

TVMStatus VMThreadActivate(TVMThreadID threadID) {
    MachineSuspendSignals(&sigState);
    if(threadID >= threadList.size())
        return VM_STATUS_ERROR_INVALID_ID;

    for (TCB* tcb : threadList) {
        if (tcb->tid == threadID && tcb->state == VM_THREAD_STATE_DEAD) {
            tcb->state = VM_THREAD_STATE_READY;
            MachineContextCreate(&tcb->cntx, &ThreadWrapper, tcb, tcb->stackAdr, tcb->stackSize);
            if(threadID > 1 && tcb->prio > runningThread->prio){
                TCB* prev = runningThread;
                prev->state = VM_THREAD_STATE_READY;
                readyThreadList.push(prev);
                runningThread = tcb;
                runningThread->state = VM_THREAD_STATE_RUNNING;
                MachineResumeSignals(&sigState);
                //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
                MachineContextSwitch(&prev->cntx, &runningThread->cntx);
                //std::cout << "-Switching " << "back" << "\n";
            }
            else{
                readyThreadList.push(tcb);
            }
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
    }

    MachineResumeSignals(&sigState);
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadTerminate(TVMThreadID threadID){
    MachineSuspendSignals(&sigState);
    if(threadID >= threadList.size())
        return VM_STATUS_ERROR_INVALID_ID;

    for (TCB *tcb : threadList) {
        if (tcb->tid == threadID) {
            tcb->state = VM_THREAD_STATE_DEAD;
            // If terminated thread is the current thread, switch to another thread
            if(tcb->tid == runningThread->tid){
                TCB* prev = runningThread;
                runningThread = readyThreadList.top();
                runningThread->state = VM_THREAD_STATE_RUNNING;
                readyThreadList.pop();
                MachineResumeSignals(&sigState);
                MachineContextSwitch(&prev->cntx, &runningThread->cntx);
            }
        }
    }

    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadState(TVMThreadID threadID, TVMThreadStateRef stateref){
    for(TCB* tcb : threadList){
        if(threadID == tcb->tid)
            *stateref = tcb->state;
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

void AlarmCallback(void* calldata){
    g_tick++;
    MachineSuspendSignals(&sigState);
    while(readyThreadList.top()->state == VM_THREAD_STATE_DEAD){
        readyThreadList.pop();
    }


    if(waitThreadList.size() != 0){
        TCB* tcb = waitThreadList.top();
        if(tcb->timeup <= g_tick){
            //std::cout << "-Thread " <<  tcb->tid << " Woke up" << "\n";
            tcb->state = VM_THREAD_STATE_READY;
            readyThreadList.push(tcb);
            waitThreadList.pop();

        }
    }
    if(runningThread->tid == 0 || readyThreadList.top()->prio > runningThread->prio){
        runningThread->state = VM_THREAD_STATE_READY;
        readyThreadList.push(runningThread);
        TCB* prev = runningThread;
        runningThread = readyThreadList.top();
        runningThread->state = VM_THREAD_STATE_RUNNING;
        readyThreadList.pop();
        MachineResumeSignals(&sigState);
        //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
        if(prev->tid != runningThread->tid)
            MachineContextSwitch(&prev->cntx, &runningThread->cntx);
    }
    MachineResumeSignals(&sigState);
}

TVMStatus VMThreadSleep(TVMTick tick){
    MachineSuspendSignals(&sigState);
    //std::cout << "-Thread " <<  runningThread->tid << " Sleeping..." << "\n";
    runningThread->state = VM_THREAD_STATE_WAITING;
    runningThread->timeup = g_tick + tick;
    waitThreadList.push(runningThread);
    TCB* prev = runningThread;
    prev->state = VM_THREAD_STATE_READY;
    runningThread = readyThreadList.top();
    runningThread->state = VM_THREAD_STATE_RUNNING;
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);
    return VM_STATUS_SUCCESS;
}

void FileCallback(void* calldata, int result){
    TCB* callThread = (TCB*)(calldata);
    //std::cout << "-Thread " <<  callThread->tid << " File Done." << "\n";
    readyThreadList.push(callThread);
    fileResult = result;
    return;
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor) {
    MachineSuspendSignals(&sigState);
    MachineFileOpen(filename, flags, mode, FileCallback, runningThread);
    TCB* prev = runningThread;
    prev->state = VM_THREAD_STATE_READY;
    runningThread = readyThreadList.top();
    runningThread->state = VM_THREAD_STATE_RUNNING;
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);

    *filedescriptor = fileResult;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileClose(int fd){
    MachineSuspendSignals(&sigState);
    MachineFileClose(fd, &FileCallback, runningThread);
    TCB* prev = runningThread;
    prev->state = VM_THREAD_STATE_READY;
    runningThread = readyThreadList.top();
    runningThread->state = VM_THREAD_STATE_RUNNING;
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileRead(int fd, void *data, int *length){
    MachineSuspendSignals(&sigState);
    MachineFileRead(fd, sharedMem, *length, &FileCallback, runningThread);
    std::memcpy(data, sharedMem, *length);
    sharedMem = (char*)sharedMem + *length;

    TCB* prev = runningThread;
    prev->state = VM_THREAD_STATE_READY;
    runningThread = readyThreadList.top();
    runningThread->state = VM_THREAD_STATE_RUNNING;
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);

    *length = fileResult;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileWrite(int fd, void *data, int *length){
    MachineSuspendSignals(&sigState);
    std::memcpy(sharedMem, data, 512);
    MachineFileWrite(fd, sharedMem, *length, &FileCallback, runningThread);
    sharedMem = (char*)sharedMem + *length;

    TCB* prev = runningThread;
    prev->state = VM_THREAD_STATE_READY;
    runningThread = readyThreadList.top();
    runningThread->state = VM_THREAD_STATE_RUNNING;
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);

    *length = fileResult;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileSeek(int fd, int offset, int whence, int *newoffset){
    MachineSuspendSignals(&sigState);
    MachineFileSeek(fd, offset, whence, &FileCallback, runningThread);
    TCB* prev = runningThread;
    prev->state = VM_THREAD_STATE_READY;
    runningThread = readyThreadList.top();
    runningThread->state = VM_THREAD_STATE_RUNNING;
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);
    *newoffset = fileResult;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexCreate(TVMMutexIDRef mutexref){
    if(mutexref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineSuspendSignals(&sigState);
    Mutex* mx = new Mutex();
    mx->mid = mutexList.size();
    mutexList.push_back(mx);
    *mutexref = mx->mid;
    MachineResumeSignals(&sigState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutexID, TVMThreadIDRef ownerref){
    if(ownerref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    for(Mutex* mx : mutexList){
        if(mx->mid == mutexID){
            *ownerref = mx->owner;
            return VM_STATUS_SUCCESS;
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMMutexAcquire(TVMMutexID mutexID, TVMTick timeout){
    if(mutexID >= mutexList.size())
        return VM_STATUS_ERROR_INVALID_ID;

    MachineSuspendSignals(&sigState);
    if(mutexList[mutexID]->status == VM_MUTEX_STATE_UNLOCKED){
        std::cout << "-Locking thread " << runningThread->tid << " on mutex " << mutexList[mutexID]->mid << "\n";
        mutexList[mutexID]->status = VM_MUTEX_STATE_LOCKED;
        mutexList[mutexID]->owner = runningThread->tid;
    }
    else{
        mutexList[mutexID]->waitingThreads.insert(mutexList[mutexID]->waitingThreads.begin(), runningThread);
        TCB* prev = runningThread;
        prev->state = VM_THREAD_STATE_READY;
        runningThread = readyThreadList.top();
        runningThread->state = VM_THREAD_STATE_RUNNING;
        readyThreadList.pop();
        std::cout << "-Waiting thread " << prev->tid << " on mutex " << mutexList[mutexID]->mid << "\n";
        MachineResumeSignals(&sigState);
        MachineContextSwitch(&prev->cntx, &runningThread->cntx);
    }

    MachineResumeSignals(&sigState);
    if(mutexList[mutexID]->owner == runningThread->tid)
        return VM_STATUS_SUCCESS;
    else
        return VM_STATUS_FAILURE;
}

TVMStatus VMMutexRelease(TVMMutexID mutexID){
    if(mutexID >= mutexList.size())
        return VM_STATUS_ERROR_INVALID_ID;

    //if(mutexList[mutexID]->waitingThreads.empty()){
        mutexList[mutexID]->status = VM_MUTEX_STATE_UNLOCKED;
        mutexList[mutexID]->owner = 0;
   //}
    /*
    else{
        mutexList[mutexID]->owner = mutexList[mutexID]->waitingThreads.back()->tid;
        readyThreadList.push(mutexList[mutexID]->waitingThreads.back());
        mutexList[mutexID]->waitingThreads.pop_back();
        //std::cout << "-Mutex " << mutexID << " ownership " << runningThread << "\n";
    }*/
    return VM_STATUS_SUCCESS;
}

}
