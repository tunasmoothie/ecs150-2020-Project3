#include "VirtualMachine.h"
#include "Machine.h"
#include <unistd.h>
#include <vector>
#include <queue>
#include <iostream>
#include <cstring>

extern "C" {
TVMMainEntry VMLoadModule(const char *module);
void VMUnloadModule(void);
void AlarmCallback(void* calldata);

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

// HELPER FUNCTIONS
//=============================================================
void EmptyMain(void* param){
}

void IdleMain(void* param){
    std::cout << "-Idling..." << "\n";
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
    for (TCB* t : threadList) {
        if (t->tid == threadID && t->state == VM_THREAD_STATE_DEAD) {
            t->state = VM_THREAD_STATE_READY;
            readyThreadList.push(t);
            MachineContextCreate(&t->cntx, &ThreadWrapper, t, t->stackAdr, t->stackSize);
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
    }
    MachineResumeSignals(&sigState);
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadTerminate(TVMThreadID threadID){
    MachineSuspendSignals(&sigState);
    for (TCB *tcb : threadList) {
        if (tcb->tid == threadID) {
            tcb->state = VM_THREAD_STATE_DEAD;
            // If terminated thread is the current thread, switch to another thread
            if(tcb->tid == runningThread->tid){
                MachineResumeSignals(&sigState);
                TCB* prev = runningThread;
                runningThread = readyThreadList.top();
                MachineResumeSignals(&sigState);
                //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
                MachineContextSwitch(&prev->cntx, &runningThread->cntx);
            }
        }
    }
    MachineResumeSignals(&sigState);
    return VM_STATUS_ERROR_INVALID_ID;
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
        readyThreadList.push(runningThread);
        TCB* prev = runningThread;
        runningThread = readyThreadList.top();
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
    runningThread = readyThreadList.top();
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
    runningThread = readyThreadList.top();
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
    runningThread = readyThreadList.top();
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileRead(int fd, void *data, int *length){
    MachineSuspendSignals(&sigState);
    std::memcpy(sharedMem, data, 512);
    MachineFileRead(fd, sharedMem, *length, &FileCallback, runningThread);
    sharedMem = (char*)sharedMem + *length;

    TCB* prev = runningThread;
    runningThread = readyThreadList.top();
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);

    *length = fileResult;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileWrite(int fd, void *data, int *length){
    MachineSuspendSignals(&sigState);
    std::memcpy(sharedMem, data, *length);
    MachineFileWrite(fd, sharedMem, *length, &FileCallback, runningThread);
    sharedMem = (char*)sharedMem + *length;

    TCB* prev = runningThread;
    runningThread = readyThreadList.top();
    readyThreadList.pop();
    MachineResumeSignals(&sigState);
    //std::cout << "-Switching " << prev->tid << "->" << runningThread->tid << "\n";
    MachineContextSwitch(&prev->cntx, &runningThread->cntx);

    *length = fileResult;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileSeek(int fd, int offset, int whence, int *newoffset){
    MachineFileSeek(fd, offset, whence, &FileCallback, runningThread);
    return VM_STATUS_SUCCESS;
}

}