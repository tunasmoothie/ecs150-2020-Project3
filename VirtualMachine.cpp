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
    TVMMutexState state = VM_MUTEX_STATE_UNLOCKED;
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

    Mutex* fileMutex = new Mutex();
    fileMutex->mid = 0;
    fileMutex->owner = 0;
    fileMutex->state = VM_MUTEX_STATE_UNLOCKED;
    mutexList.push_back(fileMutex);

    main(argc, argv);
    MachineTerminate();
    VMUnloadModule();

    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickMS(int *tickmsref){
    if(tickmsref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    *tickmsref = tickMS;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickCount(TVMTickRef tickref){
    if(tickref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    *tickref = g_tick;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority priority, TVMThreadIDRef tidRef) {
    if (entry == NULL || tidRef == NULL)
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

TVMStatus VMThreadDelete(TVMThreadID threadID){
    MachineSuspendSignals(&sigState);
    for(auto it = threadList.begin(); it != threadList.end(); it++){
        if((*it)->tid == threadID){
            threadList.erase(it);
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
    }
    MachineResumeSignals(&sigState);
    return VM_STATUS_ERROR_INVALID_ID;
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
    for(auto it = threadList.begin(); it != threadList.end(); it++){
        if ((*it)->tid == threadID && (*it)->state == VM_THREAD_STATE_DEAD){
            (*it)->state = VM_THREAD_STATE_READY;
            MachineContextCreate(&(*it)->cntx, &ThreadWrapper, (*it), (*it)->stackAdr, (*it)->stackSize);
            readyThreadList.push(*it);

            if(threadID > 1 && readyThreadList.top()->prio > runningThread->prio){
                TCB* prev = runningThread;
                prev->state = VM_THREAD_STATE_READY;
                readyThreadList.push(prev);
                runningThread = readyThreadList.top();
                runningThread->state = VM_THREAD_STATE_RUNNING;
                readyThreadList.pop();
                MachineResumeSignals(&sigState);
                MachineContextSwitch(&prev->cntx, &runningThread->cntx);
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
    for(auto it = threadList.begin(); it != threadList.end(); it++){
        if((*it)->tid == threadID){
            if((*it)->state == VM_THREAD_STATE_DEAD)
                return VM_STATUS_ERROR_INVALID_STATE;

            (*it)->state = VM_THREAD_STATE_DEAD;
            if(runningThread == (*it)){
                TCB* prev = runningThread;
                runningThread = readyThreadList.top();
                runningThread->state = VM_THREAD_STATE_RUNNING;
                readyThreadList.pop();
                MachineResumeSignals(&sigState);
                MachineContextSwitch(&prev->cntx, &runningThread->cntx);
            }
            return VM_STATUS_SUCCESS;
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadID(TVMThreadIDRef threadref){
    if(threadref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    *threadref = runningThread->tid;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadState(TVMThreadID threadID, TVMThreadStateRef stateref){
    if(stateref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    for(auto it = threadList.begin(); it != threadList.end(); it++){
        if((*it)->tid == threadID){
            *stateref = (*it)->state;
            return VM_STATUS_SUCCESS;
        }
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
    if(tick == VM_TIMEOUT_INFINITE){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
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
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor) {
    VMMutexAcquire(0, VM_TIMEOUT_INFINITE);
    MachineSuspendSignals(&sigState);
    MachineFileOpen(filename, flags, mode, FileCallback, runningThread);
    VMMutexRelease(0);
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
    VMMutexAcquire(0, VM_TIMEOUT_INFINITE);
    MachineSuspendSignals(&sigState);
    MachineFileClose(fd, &FileCallback, runningThread);
    VMMutexRelease(0);
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
    VMMutexAcquire(0, VM_TIMEOUT_INFINITE);
    MachineSuspendSignals(&sigState);
    MachineFileRead(fd, sharedMem, *length, &FileCallback, runningThread);
    std::memcpy(data, sharedMem, *length);
    sharedMem = (char*)sharedMem + *length;
    VMMutexRelease(0);

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
    VMMutexAcquire(0, VM_TIMEOUT_INFINITE);
    MachineSuspendSignals(&sigState);
    std::memcpy(sharedMem, data, 512);
    MachineFileWrite(fd, sharedMem, *length, &FileCallback, runningThread);
    VMMutexRelease(0);
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
    VMMutexAcquire(0, VM_TIMEOUT_INFINITE);
    MachineSuspendSignals(&sigState);
    MachineFileSeek(fd, offset, whence, &FileCallback, runningThread);
    VMMutexRelease(0);
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

TVMStatus VMMutexDelete(TVMMutexID mutexID){
    for(auto it = mutexList.begin(); it != mutexList.end(); it++){
        if((*it)->mid == mutexID)
            mutexList.erase(it);
        return VM_STATUS_SUCCESS;
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMMutexQuery(TVMMutexID mutexID, TVMThreadIDRef ownerref){
    if(ownerref == NULL){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    for(auto it = mutexList.begin(); it != mutexList.end(); it++){
        if((*it)->mid == mutexID){
            //*ownerref = (*it)->owner;
            *ownerref = VM_THREAD_ID_INVALID;
            return VM_STATUS_SUCCESS;
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMMutexAcquire(TVMMutexID mutexID, TVMTick timeout){
    MachineSuspendSignals(&sigState);
    Mutex* mx = NULL;
    for(Mutex* x : mutexList){
        if(x->mid == mutexID)
            mx = x;
    }
    if(mx == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }

    while(mx->owner != runningThread->tid){
        if(mx->state == VM_MUTEX_STATE_UNLOCKED && mx->waitingThreads.empty()){
            mx->state = VM_MUTEX_STATE_LOCKED;
            mx->owner = runningThread->tid;
        }
        else{
            mx->waitingThreads.insert(mx->waitingThreads.begin(), runningThread);
            TCB* prev = runningThread;
            prev->state = VM_THREAD_STATE_READY;
            runningThread = readyThreadList.top();
            runningThread->state = VM_THREAD_STATE_RUNNING;
            readyThreadList.pop();
            MachineResumeSignals(&sigState);
            MachineContextSwitch(&prev->cntx, &runningThread->cntx);
        }
    }

    MachineResumeSignals(&sigState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexRelease(TVMMutexID mutexID){
    Mutex* mx = NULL;
    for(Mutex* x : mutexList){
        if(x->mid == mutexID)
            mx = x;
    }
    if(mx == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }

    mx->state = VM_MUTEX_STATE_UNLOCKED;
    mx->owner = 0;
    if(!mx->waitingThreads.empty()){
        readyThreadList.push(mx->waitingThreads.back());
        mx->waitingThreads.pop_back();
        if(readyThreadList.top()->prio > runningThread->prio){
            TCB* prev = runningThread;
            prev->state = VM_THREAD_STATE_READY;
            runningThread = readyThreadList.top();
            runningThread->state = VM_THREAD_STATE_RUNNING;
            readyThreadList.pop();
            readyThreadList.push(prev);
            MachineResumeSignals(&sigState);
            MachineContextSwitch(&prev->cntx, &runningThread->cntx);
        }
    }


    MachineResumeSignals(&sigState);
    return VM_STATUS_SUCCESS;
}

}